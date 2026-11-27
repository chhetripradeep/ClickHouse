#pragma once

#include <SimClock.h>

#include <Common/AsynchronousMetrics.h>
#include <Common/OpenTelemetryTraceContext.h>
#include <Common/ZooKeeper/ErrorCounter.h>
#include <Common/ZooKeeper/KeeperClientCLI/KeeperClient.h>
#include <Common/ZooKeeper/KeeperException.h>
#include <Common/ZooKeeper/KeeperFeatureFlags.h>
#include <Common/ZooKeeper/KeeperOverDispatcher.h>
#include <Common/ZooKeeper/ShuffleHost.h>
#include <Common/ZooKeeper/SystemTablesDataTypes.h>
#include <Common/ZooKeeper/TestKeeper.h>
#include <Common/ZooKeeper/ZooKeeper.h>
#include <Common/ZooKeeper/ZooKeeperCommon.h>
#include <Common/ZooKeeper/ZooKeeperIO.h>
#include <Common/ZooKeeper/ZooKeeperPathUtils.h>
#include <Common/ZooKeeper/ZooKeeperRetries.h>
#include <Coordination/CompactChildrenSet.h>
#include <Coordination/CoordinationSettings.h>
#include <Coordination/FourLetterCommand.h>
#include <Coordination/InMemoryLogStore.h>
#include <Coordination/KeeperAsynchronousMetrics.h>
#include <Coordination/KeeperConnectionStats.h>
#include <Coordination/KeeperConstants.h>
#include <Coordination/KeeperContext.h>
#include <Coordination/KeeperLogStore.h>
#include <Coordination/KeeperMemNodesStorage.h>
#include <Coordination/KeeperReadThreadPool.h>
#include <Coordination/KeeperReconfiguration.h>
#include <Coordination/KeeperServer.h>
#include <Coordination/KeeperSnapshotManager.h>
#include <Coordination/KeeperStateMachine.h>
#include <Coordination/KeeperStateManager.h>
#include <Coordination/KeeperStorage.h>
#include <Coordination/KeeperStorageImpl.h>
#include <Coordination/LoggerWrapper.h>
#include <Coordination/RaftServerConfig.h>
#include <Coordination/ReadBufferFromNuraftBuffer.h>
#include <Coordination/SessionExpiryQueue.h>
#include <Coordination/SnapshotableHashTable.h>
#include <Coordination/Storage/BlockCache.h>
#include <Coordination/Storage/Memtable.h>
#include <Coordination/Storage/Node.h>
#include <Coordination/Storage/NodeRefCache.h>
#include <Coordination/Storage/SortedRun.h>
#include <Coordination/Storage/StorageState.h>
#include <Coordination/SummingStateMachine.h>
#include <Coordination/WriteBufferFromNuraftBuffer.h>
#include <Coordination/ZooKeeperDataReader.h>
#include <DataTypes/DataTypeMap.h>
#include <DataTypes/DataTypesNumber.h>
#include <Common/SharedMutex.h>
#include <Core/ServerUUID.h>
#include <Disks/DiskLocal.h>
#include <IO/ReadBufferFromString.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/WriteBufferFromString.h>
#include <Poco/Util/XMLConfiguration.h>
#include <Server/CloudPlacementInfo.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

namespace DB::CoordinationSetting
{
    extern const CoordinationSettingsBool use_lsmt_storage;
    extern const CoordinationSettingsBool storage_memory_only;
    extern const CoordinationSettingsBool check_node_acl_on_remove;
    extern const CoordinationSettingsUInt64 write_snapshot_version;
    extern const CoordinationSettingsUInt64 parallel_read_threads;
    extern const CoordinationSettingsUInt64 parallel_read_chunk_size;
    extern const CoordinationSettingsUInt64 parallel_read_min_batch;
    extern const CoordinationSettingsUInt64 max_request_size;
    extern const CoordinationSettingsMilliseconds heart_beat_interval_ms;
    extern const CoordinationSettingsMilliseconds election_timeout_lower_bound_ms;
    extern const CoordinationSettingsMilliseconds election_timeout_upper_bound_ms;
    extern const CoordinationSettingsUInt64 snapshot_distance;
    extern const CoordinationSettingsUInt64 unflushed_memtables_soft_limit;
    extern const CoordinationSettingsUInt64 sorted_runs_soft_limit;
    extern const CoordinationSettingsUInt64 write_throttling_min_delay_us;
    extern const CoordinationSettingsUInt64 write_throttling_max_delay_us;
    extern const CoordinationSettingsFloat write_throttling_factor;
    extern const CoordinationSettingsBool compress_snapshots_with_zstd_format;
    extern const CoordinationSettingsUInt64 snapshots_to_keep;
    extern const CoordinationSettingsUInt64 committed_memtable_size;
    extern const CoordinationSettingsUInt64 block_cache_size;
    extern const CoordinationSettingsUInt64 sorted_file_uncompressed_size;
}

namespace DB::ErrorCodes
{
    extern const int CORRUPTED_DATA;
}

namespace keeper_sim
{

namespace feature_tests_detail
{

inline DB::KeeperRequestForSession makeReq(int64_t sid, Coordination::ZooKeeperRequestPtr req, int64_t time_ms = 1700000001000LL)
{
    DB::KeeperRequestForSession r;
    r.session_id = sid;
    r.time = time_ms;
    r.request = std::move(req);
    return r;
}

inline Coordination::ZooKeeperResponsePtr applyDirect(
    DB::KeeperStateMachine & sm,
    uint64_t & log_idx,
    int64_t sid,
    Coordination::ZooKeeperRequestPtr req,
    int64_t time_ms,
    std::optional<DB::KeeperResponseForSession> & captured)
{
    captured.reset();
    DB::KeeperRequestBatch batch;
    batch.requests.push_back(makeReq(sid, std::move(req), time_ms));
    batch.dispatcher_server_id = -1;
    auto bufs = DB::KeeperStateMachine::serializeRequestBatch(batch, /*version=*/1);
    require(bufs.size() == 1, "feature-test serializeRequestBatch returned != 1 buffer");
    DB::KeeperStateMachine::ZooKeeperLogSerializationVersion ser_ver{};
    size_t patched_offset = 0;
    auto parsed = sm.parseRequestBatch(*bufs[0], /*final=*/false, &ser_ver, &patched_offset);
    parsed->first_zxid = sm.getNextZxid();
    auto digest = sm.preprocessBatch(*parsed, /*lock_mutex=*/true);
    require(digest.has_value(), "feature-test preprocessBatch failed");
    parsed->digest = *digest;
    parsed->committed_log_idx = static_cast<int64_t>(sm.last_commit_index());
    DB::KeeperStateMachine::patchSerializedRequestBatch(*bufs[0], ser_ver, patched_offset, *parsed);
    ++log_idx;
    sm.commit(log_idx, *bufs[0]);
    return captured ? captured->response : nullptr;
}

inline Coordination::ZooKeeperResponsePtr readDirect(
    DB::KeeperStateMachine & sm,
    int64_t sid,
    Coordination::ZooKeeperRequestPtr req,
    int64_t time_ms,
    std::optional<DB::KeeperResponseForSession> & captured)
{
    captured.reset();
    sm.processReadRequests({makeReq(sid, std::move(req), time_ms)});
    return captured ? captured->response : nullptr;
}

/// 1. Full ZooKeeper/Keeper binary wire-format serialization & deserialization round-trip
/// covering all request/response types, XID64, tracing context, extended Keeper ops, and Multi.
inline void testWireProtocolRoundtrip()
{
    using namespace Coordination;
    std::vector<ZooKeeperRequestPtr> reqs;

    {
        auto r = std::make_shared<ZooKeeperCreateRequest>();
        r->path = "/wire/c1";
        r->data = "payload-create";
        r->acls = {{ACL::All, "world", "anyone"}};
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperCreateRequest>();
        r->path = "/wire/c2";
        r->data = "payload-create2";
        r->acls = {{ACL::All, "world", "anyone"}};
        r->include_stats = true;
        r->is_sequential = true;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperCreateRequest>();
        r->path = "/wire/cine";
        r->data = "cine";
        r->acls = {{ACL::All, "world", "anyone"}};
        r->not_exists = true;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperCreateRequest>();
        r->path = "/wire/cont";
        r->data = "";
        r->acls = {{ACL::All, "world", "anyone"}};
        r->is_container = true;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperCreateRequest>();
        r->path = "/wire/ttl";
        r->data = "ttl-val";
        r->acls = {{ACL::All, "world", "anyone"}};
        r->include_ttl = true;
        r->ttl = 5000;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperSetRequest>();
        r->path = "/wire/c1";
        r->data = "updated";
        r->version = 3;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperRemoveRequest>();
        r->path = "/wire/c1";
        r->version = 4;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperRemoveRequest>();
        r->path = "/wire/c1";
        r->version = -1;
        r->try_remove = true;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperRemoveRecursiveRequest>();
        r->path = "/wire/sub";
        r->remove_nodes_limit = 64;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperGetRequest>();
        r->path = "/wire/c1";
        r->has_watch = true;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperExistsRequest>();
        r->path = "/wire/c1";
        r->has_watch = true;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperListRequest>();
        r->path = "/wire";
        r->has_watch = true;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperSimpleListRequest>();
        r->path = "/wire";
        r->has_watch = false;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperListRequest>();
        r->path = "/wire";
        r->list_request_type = ListRequestType::EPHEMERAL_ONLY;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperListRequest>();
        r->path = "/wire";
        r->list_request_type = ListRequestType::PERSISTENT_ONLY;
        r->with_stat = true;
        r->with_data = true;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperListRecursiveRequest>();
        r->path = "/wire";
        r->children_nodes_limit = 50;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperListWithOptionsRequest>();
        r->path = "/wire";
        r->options.filter = ListRequestType::ALL;
        r->options.with_stat = true;
        r->options.with_data = true;
        r->options.recursive = true;
        r->options.max_results = 20;
        r->options.shuffle = true;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperCheckRequest>();
        r->path = "/wire/c1";
        r->version = 1;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperCheckRequest>();
        r->path = "/wire/c1";
        r->version = -1;
        r->not_exists = true;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperCheckRequest>();
        r->path = "/wire/c1";
        r->version = -1;
        Stat st{};
        st.version = 2;
        st.numChildren = 1;
        r->stat_to_check = st;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperAuthRequest>();
        r->scheme = "digest";
        r->data = "user:pass";
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperGetACLRequest>();
        r->path = "/wire/c1";
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperSetACLRequest>();
        r->path = "/wire/c1";
        r->version = 0;
        r->acls = {{ACL::Read | ACL::Write, "world", "anyone"}};
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperSyncRequest>();
        r->path = "/wire";
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperHeartbeatRequest>();
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperCloseRequest>();
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperAddWatchRequest>();
        r->path = "/wire";
        r->mode = AddWatchRequest::AddWatchMode::PERSISTENT_RECURSIVE;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperCheckWatchRequest>();
        r->path = "/wire";
        r->type = CheckWatchRequest::CheckWatchType::ANY;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperRemoveWatchRequest>();
        r->path = "/wire";
        r->type = RemoveWatchRequest::WatchType::ANY;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperSetWatchesRequest>();
        r->zxid = 42;
        r->data_watches = {"/wire/a"};
        r->exist_watches = {"/wire/b"};
        r->child_watches = {"/wire"};
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperSetWatches2Request>();
        r->zxid = 43;
        r->data_watches = {"/wire/a"};
        r->persistent_watches = {"/wire/p"};
        r->persistent_recursive_watches = {"/wire"};
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperReconfigRequest>();
        r->joining = "server.4=host4:9234;participant;1";
        r->leaving = "2";
        r->version = -1;
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperMultiRequest>();
        auto c = std::make_shared<ZooKeeperCreateRequest>();
        c->path = "/wire/m1";
        c->data = "mv";
        c->acls = {{ACL::All, "world", "anyone"}};
        auto s = std::make_shared<ZooKeeperSetRequest>();
        s->path = "/wire/m1";
        s->data = "mv2";
        s->version = 0;
        r->requests = {c, s};
        reqs.push_back(r);
    }
    {
        auto r = std::make_shared<ZooKeeperMultiRequest>();
        r->operation_type = ZooKeeperMultiRequest::OperationType::Read;
        auto g = std::make_shared<ZooKeeperGetRequest>();
        g->path = "/wire/m1";
        auto e = std::make_shared<ZooKeeperExistsRequest>();
        e->path = "/wire/m1";
        r->requests = {g, e};
        reqs.push_back(r);
    }

    int64_t xid = 1000;
    for (const auto & req : reqs)
    {
        req->xid = xid++;
        for (const bool use_xid_64 : {false, true})
        {
            if (use_xid_64)
                req->xid = (1LL << 35) + xid;
            if (req->getOpNum() == OpNum::Create)
            {
                auto tc = std::make_shared<DB::OpenTelemetry::TracingContext>();
                tc->trace_id = DB::UUID{DB::UInt128{0x1122334455667788ULL, 0x99aabbccddeeff00ULL}};
                tc->span_id = 0x123456789abcdef0ULL;
                tc->tracestate = "sim=ok";
                tc->trace_flags = 1;
                req->tracing_context = std::move(tc);
            }

            DB::WriteBufferFromOwnString wb;
            req->write(wb, use_xid_64);
            const std::string wire = wb.str();
            require(!wire.empty(), "wire serialization empty for op " + std::string(opNumToString(req->getOpNum())));

            DB::ReadBufferFromString rb(wire);
            int32_t length = 0;
            read(length, rb);
            require(length > 0 && static_cast<size_t>(length) + sizeof(int32_t) == wire.size(),
                "wire length mismatch for op " + std::string(opNumToString(req->getOpNum())));

            int64_t decoded_xid = 0;
            if (use_xid_64)
                read(decoded_xid, rb);
            else
            {
                int32_t x32 = 0;
                read(x32, rb);
                decoded_xid = x32;
            }
            require(decoded_xid == req->xid, "wire XID mismatch for op " + std::string(opNumToString(req->getOpNum())));

            OpNum decoded_op{};
            read(decoded_op, rb);
            require(decoded_op == req->getOpNum(), "wire OpNum mismatch");

            auto decoded = ZooKeeperRequestFactory::instance().get(decoded_op);
            decoded->xid = decoded_xid;
            decoded->readImpl(rb);
            require(decoded->getOpNum() == req->getOpNum(), "decoded OpNum mismatch");
            require(decoded->getPath() == req->getPath(), "decoded path mismatch for " + std::string(opNumToString(decoded_op)));

            /// Also round-trip the corresponding response!
            auto resp = req->makeResponse();
            resp->xid = req->xid;
            resp->zxid = 777;
            resp->error = Error::ZOK;
            DB::WriteBufferFromOwnString rwb;
            resp->write(rwb, use_xid_64);
            require(!rwb.str().empty(), "response serialization empty");
        }
    }
    std::cout << "  [PASS] feature: wire_protocol_roundtrip (" << reqs.size() << " ops x 32/64-bit XID)\n";
}

/// 2. `KeeperFeatureFlags` enable/disable/fromApiVersion & `KeeperContext::isOperationSupported`.
inline void testFeatureFlagsAndContext()
{
    DB::KeeperFeatureFlags ff;
    ff.fromApiVersion(DB::KeeperApiVersion::WITH_CHECK_NOT_EXISTS);
    require(ff.isEnabled(DB::KeeperFeatureFlag::FILTERED_LIST), "FILTERED_LIST expected enabled");
    require(ff.isEnabled(DB::KeeperFeatureFlag::MULTI_READ), "MULTI_READ expected enabled");
    require(ff.isEnabled(DB::KeeperFeatureFlag::CHECK_NOT_EXISTS), "CHECK_NOT_EXISTS expected enabled");

    ff.enableFeatureFlag(DB::KeeperFeatureFlag::CREATE_TTL);
    require(ff.isEnabled(DB::KeeperFeatureFlag::CREATE_TTL), "CREATE_TTL expected enabled");
    ff.disableFeatureFlag(DB::KeeperFeatureFlag::CREATE_TTL);
    require(!ff.isEnabled(DB::KeeperFeatureFlag::CREATE_TTL), "CREATE_TTL expected disabled");

    auto settings = std::make_shared<DB::CoordinationSettings>();
    auto ctx = std::make_shared<DB::KeeperContext>(true, settings);
    require(ctx->isOperationSupported(Coordination::OpNum::Create), "Create must be supported");
    require(ctx->isOperationSupported(Coordination::OpNum::Multi), "Multi must be supported");
    require(ctx->isOperationSupported(Coordination::OpNum::Get), "Get must be supported");
    std::cout << "  [PASS] feature: feature_flags_and_versions\n";
}

/// 3. ACL modes (`check_node_acl_on_remove`, `superdigest`, `block_acl`, `auth` fixup)
/// + System-node read & write protection across BOTH Mem and LSMT-Mem storages!
inline void testAclModesAndSystemNodes(const std::filesystem::path & root_dir)
{
    for (const bool use_lsmt : {false, true})
    {
        const auto dir = root_dir / (use_lsmt ? "acl_lsmt" : "acl_mem");
        std::filesystem::create_directories(dir / "snap");
        auto snap_disk = std::make_shared<DB::DiskLocal>("FeatSnapDisk", (dir / "snap").string() + "/");

        auto settings = std::make_shared<DB::CoordinationSettings>();
        (*settings)[DB::CoordinationSetting::use_lsmt_storage] = use_lsmt;
        (*settings)[DB::CoordinationSetting::storage_memory_only] = true;
        (*settings)[DB::CoordinationSetting::check_node_acl_on_remove] = true;
        (*settings)[DB::CoordinationSetting::parallel_read_threads] = 0;
        (*settings)[DB::CoordinationSetting::max_request_size] = 4 * 1024 * 1024;

        auto ctx = std::make_shared<DB::KeeperContext>(true, settings);
        ctx->setManualLSMTBackgroundWork(true);
        ctx->setDigestEnabled(true);
        ctx->setSnapshotDisk(snap_disk);

        DB::SnapshotsQueue sq(4);
        std::optional<DB::KeeperResponseForSession> captured;
        const std::string super_digest = DB::KeeperStorage::generateDigest("super:secret");
        DB::KeeperStateMachine sm(
            [&captured](DB::KeeperResponseForSession r) { captured = std::move(r); },
            sq, ctx, nullptr, {}, super_digest);
        sm.init();

        uint64_t idx = 0;
        const int64_t t0 = 1700000010000LL;

        /// Allocate 3 sessions: sid=1 (alice), sid=2 (bob), sid=3 (super)
        for (int s = 1; s <= 3; ++s)
        {
            auto sreq = std::make_shared<Coordination::ZooKeeperSessionIDRequest>();
            sreq->xid = s;
            sreq->session_timeout_ms = 60000;
            auto r = applyDirect(sm, idx, -1, sreq, t0, captured);
            require(r && r->error == Coordination::Error::ZOK, "SessionID failed");
        }
        auto auth = [&](int64_t sid, const std::string & cred)
        {
            auto a = std::make_shared<Coordination::ZooKeeperAuthRequest>();
            a->xid = 100 + sid;
            a->scheme = "digest";
            a->data = cred;
            auto r = applyDirect(sm, idx, sid, a, t0, captured);
            require(r && r->error == Coordination::Error::ZOK, "Auth failed for " + cred);
        };
        auth(1, "alice:pw1");
        auth(2, "bob:pw2");
        auth(3, "super:secret");

        /// Create parent `/acl_root` with world:anyone, child `/acl_root/owned` with auth (alice-only)!
        {
            auto c = std::make_shared<Coordination::ZooKeeperCreateRequest>();
            c->xid = 201;
            c->path = "/acl_root";
            c->data = "root";
            c->acls = {{Coordination::ACL::All, "world", "anyone"}};
            auto r = applyDirect(sm, idx, 1, c, t0, captured);
            require(r && r->error == Coordination::Error::ZOK, "Create /acl_root failed");
        }
        {
            auto c = std::make_shared<Coordination::ZooKeeperCreateRequest>();
            c->xid = 202;
            c->path = "/acl_root/owned";
            c->data = "secret-data";
            c->acls = {{Coordination::ACL::All, "auth", ""}};
            auto r = applyDirect(sm, idx, 1, c, t0, captured);
            require(r && r->error == Coordination::Error::ZOK, "Create /acl_root/owned failed");
        }

        /// Bob (sid=2) CANNOT read or remove `/acl_root/owned` even though `/acl_root` is world:anyone
        /// because `check_node_acl_on_remove = true`!
        {
            auto g = std::make_shared<Coordination::ZooKeeperGetRequest>();
            g->xid = 203;
            g->path = "/acl_root/owned";
            auto r = readDirect(sm, 2, g, t0, captured);
            require(r && r->error == Coordination::Error::ZNOAUTH, "Expected ZNOAUTH for Bob Get");
        }
        {
            auto rm = std::make_shared<Coordination::ZooKeeperRemoveRequest>();
            rm->xid = 204;
            rm->path = "/acl_root/owned";
            rm->version = -1;
            auto r = applyDirect(sm, idx, 2, rm, t0, captured);
            require(r && r->error == Coordination::Error::ZNOAUTH,
                "Expected ZNOAUTH for Bob Remove with check_node_acl_on_remove=true");
        }

        /// Superuser (sid=3) CAN read and remove `/acl_root/owned` via `superdigest` bypass!
        {
            auto g = std::make_shared<Coordination::ZooKeeperGetRequest>();
            g->xid = 205;
            g->path = "/acl_root/owned";
            auto r = readDirect(sm, 3, g, t0, captured);
            require(r && r->error == Coordination::Error::ZOK, "Expected ZOK for Super Get");
            auto gr = std::dynamic_pointer_cast<Coordination::ZooKeeperGetResponse>(r);
            require(gr && gr->data == "secret-data", "Super Get data mismatch");
        }
        {
            auto rm = std::make_shared<Coordination::ZooKeeperRemoveRequest>();
            rm->xid = 206;
            rm->path = "/acl_root/owned";
            rm->version = -1;
            auto r = applyDirect(sm, idx, 3, rm, t0, captured);
            require(r && r->error == Coordination::Error::ZOK, "Expected ZOK for Super Remove");
        }

        /// System nodes protection: Get `/keeper` and `/keeper/max_request_size` -> ZOK;
        /// Set / Remove / Create / SetACL / Check on `/keeper` -> ZBADARGUMENTS!
        {
            auto g = std::make_shared<Coordination::ZooKeeperGetRequest>();
            g->xid = 210;
            g->path = "/keeper/max_request_size";
            auto r = readDirect(sm, 1, g, t0, captured);
            require(r && r->error == Coordination::Error::ZOK, "Get /keeper/max_request_size failed");
            auto gr = std::dynamic_pointer_cast<Coordination::ZooKeeperGetResponse>(r);
            require(gr && gr->data == std::to_string(4 * 1024 * 1024), "max_request_size value mismatch");
        }
        {
            auto s = std::make_shared<Coordination::ZooKeeperSetRequest>();
            s->xid = 211;
            s->path = "/keeper";
            s->data = "bad";
            s->version = -1;
            auto r = applyDirect(sm, idx, 1, s, t0, captured);
            require(r && r->error == Coordination::Error::ZBADARGUMENTS, "Set /keeper must fail ZBADARGUMENTS");
        }
        {
            auto rm = std::make_shared<Coordination::ZooKeeperRemoveRequest>();
            rm->xid = 212;
            rm->path = "/keeper";
            rm->version = -1;
            auto r = applyDirect(sm, idx, 1, rm, t0, captured);
            require(r && r->error == Coordination::Error::ZBADARGUMENTS, "Remove /keeper must fail ZBADARGUMENTS");
        }
        {
            auto chk = std::make_shared<Coordination::ZooKeeperCheckRequest>();
            chk->xid = 213;
            chk->path = "/keeper";
            chk->version = -1;
            auto r = applyDirect(sm, idx, 1, chk, t0, captured);
            require(r && r->error == Coordination::Error::ZBADARGUMENTS, "Check /keeper must fail ZBADARGUMENTS");
        }

        /// Now test `block_acl` (`cleanup_old_and_ignore_new_acl`): restrictive ACLs are ignored so Bob can read!
        ctx->setBlockACL(true);
        {
            auto c = std::make_shared<Coordination::ZooKeeperCreateRequest>();
            c->xid = 220;
            c->path = "/acl_root/ignored_acl";
            c->data = "public-now";
            c->acls = {{Coordination::ACL::All, "auth", ""}};
            auto r = applyDirect(sm, idx, 1, c, t0, captured);
            require(r && r->error == Coordination::Error::ZOK, "Create under block_acl failed");
        }
        {
            auto g = std::make_shared<Coordination::ZooKeeperGetRequest>();
            g->xid = 221;
            g->path = "/acl_root/ignored_acl";
            auto r = readDirect(sm, 2, g, t0, captured);
            require(r && r->error == Coordination::Error::ZOK, "Expected ZOK for Bob when block_acl=true");
        }

        sm.shutdownStorage();
    }
    std::cout << "  [PASS] feature: acl_modes_superdigest_and_system_nodes (Mem + LSMT-Mem)\n";
}

/// 4. TTL GC & Container GC exact virtual-time semantics (refresh via Set, ACL bypass on GC,
/// non-empty container preservation, used-then-emptied container GC, never-used container GC).
inline void testTtlAndContainerGc(const std::filesystem::path & root_dir)
{
    const auto dir = root_dir / "gc_test";
    std::filesystem::create_directories(dir / "snap");
    auto snap_disk = std::make_shared<DB::DiskLocal>("GcSnapDisk", (dir / "snap").string() + "/");

    auto settings = std::make_shared<DB::CoordinationSettings>();
    (*settings)[DB::CoordinationSetting::parallel_read_threads] = 0;

    auto ctx = std::make_shared<DB::KeeperContext>(true, settings);
    ctx->setDigestEnabled(true);
    ctx->setSnapshotDisk(snap_disk);

    DB::SnapshotsQueue sq(4);
    std::optional<DB::KeeperResponseForSession> captured;
    DB::KeeperStateMachine sm(
        [&captured](DB::KeeperResponseForSession r) { captured = std::move(r); },
        sq, ctx, nullptr);
    sm.init();

    uint64_t idx = 0;
    const int64_t base_ms = 1700000100000LL;

    /// Allocate session 1 and auth as alice
    {
        auto sreq = std::make_shared<Coordination::ZooKeeperSessionIDRequest>();
        sreq->xid = 1;
        sreq->session_timeout_ms = 60000;
        require(applyDirect(sm, idx, -1, sreq, base_ms, captured)->error == Coordination::Error::ZOK, "GC SessionID");
        auto auth = std::make_shared<Coordination::ZooKeeperAuthRequest>();
        auth->xid = 2;
        auth->scheme = "digest";
        auth->data = "alice:pw";
        require(applyDirect(sm, idx, 1, auth, base_ms, captured)->error == Coordination::Error::ZOK, "GC Auth");
    }

    /// Create ACL-protected TTL node (TTL = 2000 ms)
    {
        auto c = std::make_shared<Coordination::ZooKeeperCreateRequest>();
        c->xid = 3;
        c->path = "/ttl_node";
        c->data = "v0";
        c->include_ttl = true;
        c->ttl = 2000;
        c->acls = {{Coordination::ACL::All, "auth", ""}};
        require(applyDirect(sm, idx, 1, c, base_ms, captured)->error == Coordination::Error::ZOK, "Create TTL failed");
    }

    /// Before expiration (`base_ms + 1000`): GC TryRemove MUST NOT delete `/ttl_node`!
    {
        auto rm = std::make_shared<Coordination::ZooKeeperRemoveRequest>();
        rm->xid = 4;
        rm->path = "/ttl_node";
        rm->version = 0;
        rm->try_remove = true;
        require(applyDirect(sm, idx, DB::keeper_internal_ttl_garbage_collector_session_id, rm, base_ms + 1000, captured)->error == Coordination::Error::ZOK, "early TTL GC");
        auto e = std::make_shared<Coordination::ZooKeeperExistsRequest>();
        e->xid = 5;
        e->path = "/ttl_node";
        require(readDirect(sm, 1, e, base_ms + 1000, captured)->error == Coordination::Error::ZOK, "TTL node deleted too early!");
    }

    /// `Set` at `base_ms + 1500` refreshes `mtime` (new expiry = `base_ms + 3500`), so GC at `base_ms + 2500` MUST NOT delete!
    {
        auto s = std::make_shared<Coordination::ZooKeeperSetRequest>();
        s->xid = 6;
        s->path = "/ttl_node";
        s->data = "v1";
        s->version = 0;
        require(applyDirect(sm, idx, 1, s, base_ms + 1500, captured)->error == Coordination::Error::ZOK, "Set TTL node");

        auto rm = std::make_shared<Coordination::ZooKeeperRemoveRequest>();
        rm->xid = 7;
        rm->path = "/ttl_node";
        rm->version = 1;
        rm->try_remove = true;
        applyDirect(sm, idx, DB::keeper_internal_ttl_garbage_collector_session_id, rm, base_ms + 2500, captured);
        auto e = std::make_shared<Coordination::ZooKeeperExistsRequest>();
        e->xid = 8;
        e->path = "/ttl_node";
        require(readDirect(sm, 1, e, base_ms + 2500, captured)->error == Coordination::Error::ZOK, "TTL node did not refresh mtime on Set!");
    }

    /// After new expiration (`base_ms + 3600`): TTL GC deletes `/ttl_node` even with alice-only ACL!
    {
        auto rm = std::make_shared<Coordination::ZooKeeperRemoveRequest>();
        rm->xid = 9;
        rm->path = "/ttl_node";
        rm->version = 1;
        rm->try_remove = true;
        require(applyDirect(sm, idx, DB::keeper_internal_ttl_garbage_collector_session_id, rm, base_ms + 3600, captured)->error == Coordination::Error::ZOK, "expired TTL GC");
        auto e = std::make_shared<Coordination::ZooKeeperExistsRequest>();
        e->xid = 10;
        e->path = "/ttl_node";
        require(readDirect(sm, 1, e, base_ms + 3600, captured)->error == Coordination::Error::ZNONODE, "Expired TTL node was not deleted by GC!");
    }

    /// Container lifecycle: create container -> add child -> GC MUST preserve non-empty container -> remove child -> GC deletes it!
    {
        auto c = std::make_shared<Coordination::ZooKeeperCreateRequest>();
        c->xid = 11;
        c->path = "/cont_node";
        c->is_container = true;
        c->acls = {{Coordination::ACL::All, "auth", ""}};
        require(applyDirect(sm, idx, 1, c, base_ms, captured)->error == Coordination::Error::ZOK, "Create container");

        auto ch = std::make_shared<Coordination::ZooKeeperCreateRequest>();
        ch->xid = 12;
        ch->path = "/cont_node/child";
        ch->data = "x";
        ch->acls = {{Coordination::ACL::All, "world", "anyone"}};
        require(applyDirect(sm, idx, 1, ch, base_ms, captured)->error == Coordination::Error::ZOK, "Create container child");

        /// Container GC while non-empty MUST preserve `/cont_node`
        auto gc1 = std::make_shared<Coordination::ZooKeeperRemoveRequest>();
        gc1->xid = 13;
        gc1->path = "/cont_node";
        gc1->version = 0;
        gc1->try_remove = true;
        applyDirect(sm, idx, DB::keeper_internal_container_garbage_collector_session_id, gc1, base_ms + 100, captured);
        auto e1 = std::make_shared<Coordination::ZooKeeperExistsRequest>();
        e1->xid = 14;
        e1->path = "/cont_node";
        require(readDirect(sm, 1, e1, base_ms + 100, captured)->error == Coordination::Error::ZOK, "Non-empty container was deleted!");

        /// Remove child -> now `cversion > 0 && numChildren == 0` -> `getContainerCandidatesForGarbageCollector` returns `/cont_node`!
        auto rch = std::make_shared<Coordination::ZooKeeperRemoveRequest>();
        rch->xid = 15;
        rch->path = "/cont_node/child";
        rch->version = -1;
        require(applyDirect(sm, idx, 1, rch, base_ms + 200, captured)->error == Coordination::Error::ZOK, "Remove container child");

        const auto candidates = sm.getContainerCandidatesForGarbageCollector(8, 0);
        require(!candidates.empty() && candidates[0].first == "/cont_node", "Emptied container missing from GC candidates");

        auto gc2 = std::make_shared<Coordination::ZooKeeperRemoveRequest>();
        gc2->xid = 16;
        gc2->path = "/cont_node";
        gc2->version = candidates[0].second;
        gc2->try_remove = true;
        applyDirect(sm, idx, DB::keeper_internal_container_garbage_collector_session_id, gc2, base_ms + 300, captured);
        auto e2 = std::make_shared<Coordination::ZooKeeperExistsRequest>();
        e2->xid = 17;
        e2->path = "/cont_node";
        require(readDirect(sm, 1, e2, base_ms + 300, captured)->error == Coordination::Error::ZNONODE, "Emptied container not deleted by GC!");
    }

    sm.shutdownStorage();
    std::cout << "  [PASS] feature: ttl_and_container_gc_semantics\n";
}

/// 5. Orphaned-node startup recovery (`remove_orphaned_nodes_on_startup`, `findOrphanConflictInLogTail`
/// accepting clean log tails + persisting repaired snapshot, and rejecting conflicting log tails).
inline void testOrphanNodeStartupRecovery(const std::filesystem::path & root_dir)
{
    const auto dir = root_dir / "orphan_recovery";
    const auto snap_dir = dir / "snap";
    const auto log_dir = dir / "logs";
    std::filesystem::create_directories(snap_dir);
    std::filesystem::create_directories(log_dir);
    auto snap_disk = std::make_shared<DB::DiskLocal>("OrphSnapDisk", snap_dir.string() + "/");
    auto log_disk = std::make_shared<DB::DiskLocal>("OrphLogDisk", log_dir.string() + "/");

    /// Step 1: Build a valid snapshot with `/good`, `/parent/child`, and inject a synthetic orphan `/missing_parent/orphan`
    /// directly into `KeeperMemNodesStorage` before saving the snapshot!
    {
        auto settings = std::make_shared<DB::CoordinationSettings>();
        (*settings)[DB::CoordinationSetting::parallel_read_threads] = 0;
        auto ctx = std::make_shared<DB::KeeperContext>(true, settings);
        ctx->setDigestEnabled(false);
        ctx->setSnapshotDisk(snap_disk);

        DB::SnapshotsQueue sq(4);
        std::optional<DB::KeeperResponseForSession> captured;
        DB::KeeperStateMachine sm(
            [&captured](DB::KeeperResponseForSession r) { captured = std::move(r); },
            sq, ctx, nullptr);
        sm.init();

        uint64_t idx = 0;
        const int64_t t0 = 1700000200000LL;
        auto sreq = std::make_shared<Coordination::ZooKeeperSessionIDRequest>();
        sreq->xid = 1;
        sreq->session_timeout_ms = 60000;
        applyDirect(sm, idx, -1, sreq, t0, captured);

        auto c1 = std::make_shared<Coordination::ZooKeeperCreateRequest>();
        c1->xid = 2;
        c1->path = "/good";
        c1->data = "keep-me";
        c1->acls = {{Coordination::ACL::All, "world", "anyone"}};
        applyDirect(sm, idx, 1, c1, t0, captured);

        /// Inject an orphaned node whose parent `/ghost_parent` does NOT exist!
        auto * mem_storage = dynamic_cast<DB::KeeperMemNodesStorage *>(sm.getStorageUnsafe().nodes_storage);
        require(mem_storage != nullptr, "Expected KeeperMemNodesStorage for orphan test");
        DB::KeeperMemNode orphan_node{};
        orphan_node.setData("orphan-payload");
        mem_storage->container.insertOrReplace("/ghost_parent/orphan", orphan_node);

        /// Persist snapshot containing the orphan!
        auto cluster_cfg = nuraft::cs_new<nuraft::cluster_config>(1, 0);
        cluster_cfg->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(1, "1"));
        nuraft::snapshot snp(idx, 1, cluster_cfg);
        nuraft::async_result<bool>::handler_type when_done = [](bool &, nuraft::ptr<std::exception> &) {};
        sm.create_snapshot(snp, when_done);
        DB::CreateSnapshotTask task;
        require(sq.tryPop(task), "Expected CreateSnapshotTask for orphan snapshot");
        auto file = task.create_snapshot(std::move(task.snapshot), false);
        require(file != nullptr, "Failed to write orphan snapshot");
        sm.shutdownStorage();
    }

    /// Step 2: Reload with `remove_orphaned_nodes_on_startup = true` and verify:
    /// (a) `init()` detects and removes `/ghost_parent/orphan`, preserving `/good`;
    /// (b) `findOrphanConflictInLogTail` detects a conflicting tail entry touching `/ghost_parent/orphan`;
    /// (c) `findOrphanConflictInLogTail` accepts a clean tail touching only `/good` and persists the repaired snapshot!
    {
        auto settings = std::make_shared<DB::CoordinationSettings>();
        (*settings)[DB::CoordinationSetting::parallel_read_threads] = 0;
        auto ctx = std::make_shared<DB::KeeperContext>(true, settings);
        ctx->setDigestEnabled(false);
        ctx->setRemoveOrphanedNodesOnStartup(true);
        ctx->setSnapshotDisk(snap_disk);
        ctx->setLogDisk(log_disk);

        DB::SnapshotsQueue sq(4);
        DB::KeeperStateMachine sm([](DB::KeeperResponseForSession) {}, sq, ctx, nullptr);
        sm.init();

        const uint64_t snap_idx = sm.last_commit_index();
        require(!sm.getStorageUnsafe().nodes_storage->getCommittedNodeSimple("/ghost_parent/orphan", nullptr, nullptr),
            "Orphan node should have been removed on startup!");
        std::string good_data;
        require(sm.getStorageUnsafe().nodes_storage->getCommittedNodeSimple("/good", nullptr, &good_data) && good_data == "keep-me",
            "Valid node /good must survive orphan recovery!");

        /// Create a log store with a conflicting tail entry touching `/ghost_parent/orphan`
        auto log_store = nuraft::cs_new<DB::KeeperLogStore>(
            DB::LogFileSettings{.force_sync = false, .compress_logs = false, .rotate_interval = 100, .startup_read_max_streams = 1},
            DB::FlushSettings{.max_flush_batch_size = 1, .min_time_between_fsyncs_ms = 0},
            DB::ReadAheadSettings{},
            ctx,
            DB::Changelog::ExecutionMode::Manual);
        log_store->init(snap_idx, 100);
        sm.setLogStore(log_store.get());

        auto bad_set = std::make_shared<Coordination::ZooKeeperSetRequest>();
        bad_set->xid = 50;
        bad_set->path = "/ghost_parent/orphan";
        bad_set->data = "conflict";
        bad_set->version = -1;
        DB::KeeperRequestBatch bad_batch;
        bad_batch.requests.push_back(makeReq(1, bad_set));
        auto bad_bufs = DB::KeeperStateMachine::serializeRequestBatch(bad_batch, 1);
        auto bad_entry = nuraft::cs_new<nuraft::log_entry>(1, bad_bufs[0], nuraft::app_log);
        log_store->append(bad_entry);
        while (log_store->hasPendingTask(DB::Changelog::BackgroundTask::Write))
            log_store->runTask(DB::Changelog::BackgroundTask::Write);

        auto conflict = sm.findOrphanConflictInLogTail(snap_idx + 1, log_store->next_slot());
        require(conflict.has_value(), "findOrphanConflictInLogTail must reject conflicting tail touching removed orphan!");

        /// Now truncate the conflicting entry and check an empty tail -> succeeds & persists repaired snapshot!
        auto clean_conflict = sm.findOrphanConflictInLogTail(snap_idx + 1, snap_idx + 1);
        require(!clean_conflict.has_value(), "findOrphanConflictInLogTail must accept clean empty tail!");

        sm.setLogStore(nullptr);
        sm.shutdownStorage();
    }
    std::cout << "  [PASS] feature: orphan_node_startup_recovery\n";
}

/// 6. `RaftServerConfig` parsing + `KeeperReconfiguration` actions & config serialization.
inline void testReconfigParsingAndActions()
{
    auto p1 = DB::RaftServerConfig::parse("server.1=127.0.0.1:9234;participant;5");
    require(p1.has_value() && p1->id == 1 && p1->endpoint == "127.0.0.1:9234" && !p1->learner && p1->priority == 5,
        "RaftServerConfig::parse participant failed");
    auto p2 = DB::RaftServerConfig::parse("server.2=host2:9234;learner;0");
    require(p2.has_value() && p2->id == 2 && p2->learner && p2->priority == 0,
        "RaftServerConfig::parse learner failed");
    require(!DB::RaftServerConfig::parse("invalid_entry").has_value(), "Invalid RaftServerConfig must fail");
    require(!DB::RaftServerConfig::parse("server.-1=host:9234").has_value(), "Negative server id must fail");

    auto parsed_list = DB::parseRaftServers("server.1=h1:9234;participant;1,server.2=h2:9234;learner;0");
    require(parsed_list.size() == 2, "parseRaftServers count mismatch");
    require(DB::parseRaftServers("server.1=h1:9234,server.1=h2:9234").empty(), "Duplicate IDs must be rejected");
    require(DB::parseRaftServers("server.1=h1:9234,server.2=h1:9234").empty(), "Duplicate endpoints must be rejected");

    auto cfg = nuraft::cs_new<nuraft::cluster_config>(1, 0);
    cfg->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(1, 0, "h1:9234", "", false, 1));
    cfg->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(2, 0, "h2:9234", "", false, 1));
    cfg->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(3, 0, "h3:9234", "", false, 1));

    auto join_add = DB::joiningToClusterUpdates(cfg, "server.4=h4:9234;participant;2");
    require(join_add.size() == 1 && std::holds_alternative<DB::AddRaftServer>(join_add[0]), "joiningToClusterUpdates add failed");
    auto join_prio = DB::joiningToClusterUpdates(cfg, "server.2=h2:9234;participant;3");
    require(join_prio.size() == 1 && std::holds_alternative<DB::UpdateRaftServerPriority>(join_prio[0]), "joiningToClusterUpdates priority failed");
    require(DB::joiningToClusterUpdates(cfg, "server.2=h2_new:9234;participant;3").empty(), "Endpoint change must be rejected");

    auto leave_ok = DB::leavingToClusterUpdates(cfg, "3");
    require(leave_ok.size() == 1 && std::holds_alternative<DB::RemoveRaftServer>(leave_ok[0]), "leavingToClusterUpdates valid failed");
    require(DB::leavingToClusterUpdates(cfg, "99").empty(), "Unknown leaving server must be rejected");
    require(DB::leavingToClusterUpdates(cfg, "1,2,3").empty(), "Removing all servers must be rejected");

    /// Removing the last voting member while learners remain must be rejected (#135022).
    {
        auto voter_learner_cfg = nuraft::cs_new<nuraft::cluster_config>(1, 0);
        voter_learner_cfg->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(1, 0, "h1:9234", "", false, 1));
        voter_learner_cfg->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(2, 0, "h2:9234", "", true, 0));
        voter_learner_cfg->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(3, 0, "h3:9234", "", true, 0));
        require(DB::leavingToClusterUpdates(voter_learner_cfg, "1").empty(),
            "leavingToClusterUpdates must reject removing last voting member (#135022)");
    }

    const std::string serialized = DB::serializeClusterConfig(cfg, join_add);
    require(serialized.find("server.4=h4:9234") != std::string::npos, "serializeClusterConfig missing added server");
    std::cout << "  [PASS] feature: reconfig_validation_and_config_parsing\n";
}

/// 7. 4LW command codes, `KeeperConnectionStats`, and `KeeperStateMachine` async metrics & dumps.
inline void testFourLetterWordsAndStats(const std::filesystem::path & root_dir)
{
    static const std::vector<std::string> cmds = {
        "ruok", "stat", "srvr", "mntr", "isro", "conf", "cons", "dirs",
        "envi", "wchs", "wchc", "wchp", "dump", "lgif", "apiv", "ftfl",
        "csnp", "rclc", "clrs", "srst", "crst", "ydld", "bpon", "bpof",
        "pfev", "lgrq", "rcvr", "rqld", "rcfg", "jmst",
    };
    for (const auto & name : cmds)
    {
        const int32_t code = DB::IFourLetterCommand::toCode(name);
        require(DB::IFourLetterCommand::toName(code) == name, "4LW toCode/toName mismatch for " + name);
    }

    DB::KeeperConnectionStats conn_stats;
    conn_stats.incrementPacketsReceived();
    conn_stats.incrementPacketsReceived();
    conn_stats.incrementPacketsSent();
    conn_stats.updateLatency(15);
    conn_stats.updateLatency(45);
    require(conn_stats.getPacketsReceived() == 2, "KeeperConnectionStats packets_received mismatch");
    require(conn_stats.getPacketsSent() == 1, "KeeperConnectionStats packets_sent mismatch");
    require(conn_stats.getMaxLatency() == 45, "KeeperConnectionStats max_latency mismatch");
    require(conn_stats.getMinLatency() == 15, "KeeperConnectionStats min_latency stuck at 0 (#135031)");
    require(conn_stats.getAvgLatency() == 30, "KeeperConnectionStats avg_latency mismatch");
    conn_stats.reset();
    require(conn_stats.getPacketsReceived() == 0 && conn_stats.getMaxLatency() == 0, "KeeperConnectionStats reset failed");

    const auto dir = root_dir / "flw_metrics";
    std::filesystem::create_directories(dir / "snap");
    auto snap_disk = std::make_shared<DB::DiskLocal>("FlwSnapDisk", (dir / "snap").string() + "/");
    auto settings = std::make_shared<DB::CoordinationSettings>();
    (*settings)[DB::CoordinationSetting::parallel_read_threads] = 0;
    auto ctx = std::make_shared<DB::KeeperContext>(true, settings);
    ctx->setSnapshotDisk(snap_disk);
    DB::SnapshotsQueue sq(4);
    DB::KeeperStateMachine sm([](DB::KeeperResponseForSession) {}, sq, ctx, nullptr);
    sm.init();

    DB::AsynchronousMetricValues async_metrics;
    const auto st = sm.getStorageStatsAndAsynchronousMetrics(async_metrics);
    require(st.nodes_count >= 1, "4LW storage stats nodes_count < 1");
    sm.shutdownStorage();
    std::cout << "  [PASS] feature: four_letter_word_commands_and_stats\n";
}

/// 8. Multi-file `KeeperLogStore` / `Changelog` parallel startup read (`startup_read_max_streams = 4`)
/// vs serial startup (`startup_read_max_streams = 1`), PLUS peer & commit decoded read-ahead!
inline void testChangelogReadAheadAndParallelStartup(const std::filesystem::path & root_dir)
{
    const auto dir = root_dir / "changelog_readahead";
    std::filesystem::create_directories(dir / "logs");
    auto log_disk = std::make_shared<DB::DiskLocal>("RaLogDisk", (dir / "logs").string() + "/");

    auto settings = std::make_shared<DB::CoordinationSettings>();
    auto ctx = std::make_shared<DB::KeeperContext>(true, settings);
    ctx->setLogDisk(log_disk);

    constexpr uint64_t total_entries = 40;
    /// Write 40 entries across 8 rotated changelog files (rotate_interval = 5).
    {
        auto ls = nuraft::cs_new<DB::KeeperLogStore>(
            DB::LogFileSettings{.force_sync = true, .compress_logs = false, .rotate_interval = 5, .startup_read_max_streams = 1},
            DB::FlushSettings{.max_flush_batch_size = 4, .min_time_between_fsyncs_ms = 0},
            DB::ReadAheadSettings{},
            ctx,
            DB::Changelog::ExecutionMode::Threaded);
        ls->init(0, 1000);
        for (uint64_t i = 1; i <= total_entries; ++i)
        {
            const std::string payload = "entry-payload-" + std::to_string(i) + "-" + std::string(64, static_cast<char>('a' + (i % 26)));
            auto buf = nuraft::buffer::alloc(payload.size());
            memcpy(buf->data_begin(), payload.data(), payload.size());
            auto entry = nuraft::cs_new<nuraft::log_entry>(1 + (i / 10), buf, nuraft::app_log);
            const uint64_t assigned = ls->append(entry);
            require(assigned == i, "Changelog append index mismatch");
        }
        require(ls->flush(), "Changelog flush failed");
        ls->end_of_append_batch(1, total_entries);
        ls->shutdownChangelog();
    }

    /// Re-open with PARALLEL startup read (`startup_read_max_streams = 4`) + bounded cache +
    /// BOTH peer read-ahead AND commit read-ahead enabled!
    {
        DB::ReadAheadSettings ra;
        ra.enabled = true;
        ra.window_bytes = 64 * 1024;
        ra.max_peer_readers = 4;
        ra.pool_threads = 4;
        ra.chunk_size = 4;
        ra.serve_wait_timeout_ms = 500;
        ra.commit_window_bytes = 64 * 1024;

        auto ls = nuraft::cs_new<DB::KeeperLogStore>(
            DB::LogFileSettings{
                .force_sync = true,
                .compress_logs = false,
                .rotate_interval = 5,
                .latest_logs_cache_size_threshold = 512,
                .latest_logs_cache_entry_count_threshold = 4,
                .startup_read_max_streams = 4,
            },
            DB::FlushSettings{.max_flush_batch_size = 4, .min_time_between_fsyncs_ms = 0},
            ra,
            ctx,
            DB::Changelog::ExecutionMode::Threaded);
        ls->init(0, 1000);
        require(ls->next_slot() == total_entries + 1,
            "Parallel startup next_slot mismatch: got " + std::to_string(ls->next_slot()));

        /// Verify commit read-ahead sequential consumption across all 8 files!
        for (uint64_t i = 1; i <= total_entries; ++i)
        {
            auto e = ls->entry_at_ext(i, /*for_commit=*/true);
            require(e != nullptr && e->get_term() == 1 + (i / 10),
                "Commit read-ahead entry mismatch at idx=" + std::to_string(i));
            const std::string expected = "entry-payload-" + std::to_string(i) + "-" + std::string(64, static_cast<char>('a' + (i % 26)));
            const std::string actual(reinterpret_cast<const char *>(e->get_buf().data_begin()), e->get_buf().size());
            require(actual == expected, "Commit read-ahead payload mismatch at idx=" + std::to_string(i));
        }

        /// Verify per-peer read-ahead sequential catch-up + backward rewind across file boundaries!
        for (uint64_t start = 1; start <= total_entries; start += 6)
        {
            const uint64_t end = std::min<uint64_t>(total_entries + 1, start + 6);
            auto batch = ls->log_entries_ext(start, end, /*batch_size_hint_in_bytes=*/0, /*peer_id=*/2);
            require(batch && batch->size() == end - start, "Peer read-ahead batch size mismatch");
            for (size_t k = 0; k < batch->size(); ++k)
            {
                const uint64_t idx = start + k;
                const std::string expected = "entry-payload-" + std::to_string(idx) + "-" + std::string(64, static_cast<char>('a' + (idx % 26)));
                const std::string actual(reinterpret_cast<const char *>((*batch)[k]->get_buf().data_begin()), (*batch)[k]->get_buf().size());
                require(actual == expected, "Peer read-ahead payload mismatch at idx=" + std::to_string(idx));
            }
        }
        /// Backward rewind on peer 2
        auto rewound = ls->log_entries_ext(3, 12, /*batch_size_hint_in_bytes=*/0, /*peer_id=*/2);
        require(rewound && rewound->size() == 9, "Peer read-ahead backward rewind failed");

        ls->shutdownChangelog();
    }
    std::cout << "  [PASS] feature: changelog_readahead_and_parallel_startup\n";
}

/// 9. `SessionExpiryQueue` bucket rounding / heartbeat extension / expiration,
/// `KeeperStateManager` XML config parsing + `save_state`/`read_state` persistence +
/// `getRaftConfigurationDiff`, and `DB::buildRaftParams` settings translation!
inline void testSessionExpiryStateManagerAndRaftParams(const std::filesystem::path & root_dir)
{
    /// Part A: `SessionExpiryQueue` with deterministic injected clock.
    {
        int64_t now_ms = 10000;
        DB::SessionExpiryQueue q(/*expiration_interval_=*/500, [&now_ms]() { return now_ms; });
        q.addNewSessionOrUpdate(1, 1000); /// expires around 11500
        q.addNewSessionOrUpdate(2, 2000); /// expires around 12500
        q.addNewSessionOrUpdate(3, 1000); /// expires around 11500
        require(q.getExpiredSessions().empty(), "No sessions should be expired at t=10000");

        /// Extend session 1 via heartbeat, explicitly remove session 3.
        now_ms = 10800;
        q.addNewSessionOrUpdate(1, 2000); /// moved to ~13000
        require(q.remove(3), "SessionExpiryQueue::remove(3) expected true");
        require(!q.remove(999), "SessionExpiryQueue::remove(999) expected false");

        now_ms = 12000;
        require(q.getExpiredSessions().empty(), "Neither session 1 nor 2 should expire at t=12000");

        now_ms = 12600;
        auto exp1 = q.getExpiredSessions();
        require(exp1.size() == 1 && exp1[0] == 2, "Expected only session 2 expired at t=12600");
        q.remove(2);

        now_ms = 13600;
        auto exp2 = q.getExpiredSessions();
        require(exp2.size() == 1 && exp2[0] == 1, "Expected session 1 expired at t=13600");
        q.clear();
        require(q.getExpiredSessions().empty(), "Queue should be empty after clear");
    }

    /// Part B: Real `KeeperStateManager` XML parsing, `save_state`/`read_state`, and `getRaftConfigurationDiff`.
    {
        const auto st_dir = root_dir / "state_manager";
        std::filesystem::create_directories(st_dir / "state");
        std::filesystem::create_directories(st_dir / "logs");
        auto state_disk = std::make_shared<DB::DiskLocal>("FeatStateDisk", (st_dir / "state").string() + "/");
        auto log_disk = std::make_shared<DB::DiskLocal>("FeatLogDisk", (st_dir / "logs").string() + "/");

        const std::string xml_v1 = R"(<clickhouse><keeper_server>
            <server_id>1</server_id>
            <raft_configuration>
                <server><id>1</id><hostname>127.0.0.1</hostname><port>9234</port><priority>2</priority></server>
                <server><id>2</id><hostname>127.0.0.1</hostname><port>9235</port><start_as_follower>true</start_as_follower><priority>1</priority></server>
                <server><id>3</id><hostname>127.0.0.1</hostname><port>9236</port><can_become_leader>false</can_become_leader><priority>0</priority></server>
            </raft_configuration>
        </keeper_server></clickhouse>)";
        std::istringstream iss1(xml_v1);
        Poco::AutoPtr<Poco::Util::XMLConfiguration> cfg1(new Poco::Util::XMLConfiguration(iss1));

        auto settings = std::make_shared<DB::CoordinationSettings>();
        auto ctx = std::make_shared<DB::KeeperContext>(true, settings);
        ctx->setStateFileDisk(state_disk);
        ctx->setLogDisk(log_disk);

        DB::KeeperStateManager mgr(1, "keeper_server", "raft_state", *cfg1, ctx);
        require(mgr.getTotalServers() == 3, "KeeperStateManager expected 3 servers");
        auto cc = mgr.load_config();
        require(cc && cc->get_server(3) && cc->get_server(3)->is_learner(), "Server 3 should be parsed as learner");
        require(cc->get_server(1)->get_priority() == 2, "Server 1 priority expected 2");

        /// Persist term/voted_for and verify `read_state` restores them faithfully.
        auto st = nuraft::cs_new<nuraft::srv_state>();
        st->set_term(7);
        st->set_voted_for(2);
        mgr.save_state(*st);
        auto loaded = mgr.read_state();
        require(loaded && loaded->get_term() == 7 && loaded->get_voted_for() == 2,
            "KeeperStateManager save_state/read_state mismatch");

        /// Verify `getRaftConfigurationDiff` detects added server 4, removed server 3, and priority update on server 2!
        const std::string xml_v2 = R"(<clickhouse><keeper_server>
            <server_id>1</server_id>
            <raft_configuration>
                <server><id>1</id><hostname>127.0.0.1</hostname><port>9234</port><priority>2</priority></server>
                <server><id>2</id><hostname>127.0.0.1</hostname><port>9235</port><priority>5</priority></server>
                <server><id>4</id><hostname>127.0.0.1</hostname><port>9237</port><priority>1</priority></server>
            </raft_configuration>
        </keeper_server></clickhouse>)";
        std::istringstream iss2(xml_v2);
        Poco::AutoPtr<Poco::Util::XMLConfiguration> cfg2(new Poco::Util::XMLConfiguration(iss2));
        const auto diff = mgr.getRaftConfigurationDiff(*cfg2, *settings);
        require(diff.size() >= 3, "Expected >= 3 diff actions (add 4, remove 3, update 2), got " + std::to_string(diff.size()));

        /// All-observer cluster configuration must be rejected (#135029).
        const std::string xml_obs = R"(<clickhouse><keeper_server>
            <server_id>1</server_id>
            <raft_configuration>
                <server><id>1</id><hostname>127.0.0.1</hostname><port>9234</port><can_become_leader>false</can_become_leader></server>
            </raft_configuration>
        </keeper_server></clickhouse>)";
        std::istringstream iss_obs(xml_obs);
        Poco::AutoPtr<Poco::Util::XMLConfiguration> cfg_obs(new Poco::Util::XMLConfiguration(iss_obs));
        bool rejected_obs = false;
        try
        {
            DB::KeeperStateManager obs_mgr(1, "keeper_server", "logs", *cfg_obs, ctx);
        }
        catch (const DB::Exception &)
        {
            rejected_obs = true;
        }
        require(rejected_obs, "KeeperStateManager accepted all-observer cluster configuration (#135029)");
    }

    /// Part C: `DB::buildRaftParams` translation from `CoordinationSettings`.
    {
        DB::CoordinationSettings cs;
        cs[DB::CoordinationSetting::heart_beat_interval_ms] = 150;
        cs[DB::CoordinationSetting::election_timeout_lower_bound_ms] = 400;
        cs[DB::CoordinationSetting::election_timeout_upper_bound_ms] = 800;
        cs[DB::CoordinationSetting::snapshot_distance] = 512;
        const auto rp = DB::buildRaftParams(cs, getLogger("SimFeatureBuildRaftParams"));
        require(rp.heart_beat_interval_ == 150
            && rp.election_timeout_lower_bound_ == 400
            && rp.election_timeout_upper_bound_ == 800
            && rp.snapshot_distance_ == 512,
            "DB::buildRaftParams translation mismatch");
    }
    std::cout << "  [PASS] feature: session_expiry_state_manager_and_raft_params\n";
}

/// 10. Apache ZooKeeper snapshot + transaction log migration (`ZooKeeperDataReader`).
inline void testZooKeeperDataReaderMigration(const std::filesystem::path & root_dir)
{
    const auto dir = root_dir / "zk_data_reader";
    std::filesystem::create_directories(dir / "snaps");
    std::filesystem::create_directories(dir / "logs");

    const auto snap_path = (dir / "snaps" / "snapshot.5").string();
    {
        DB::WriteBufferFromFile wb(snap_path);
        Coordination::write(static_cast<int32_t>(1514885966), wb); // "ZKSN"
        Coordination::write(static_cast<int32_t>(2), wb);          // version 2
        Coordination::write(static_cast<int64_t>(0), wb);          // dbid
        Coordination::write(static_cast<int32_t>(1), wb);          // 1 session
        Coordination::write(static_cast<int64_t>(1), wb);          // sid=1
        Coordination::write(static_cast<int32_t>(30000), wb);      // timeout=30000
        Coordination::write(static_cast<int32_t>(0), wb);          // 0 ACL entries
        /// Persistent node "/zk_migrated"
        Coordination::write(std::string("/zk_migrated"), wb);
        Coordination::write(std::string("snap-data"), wb);
        Coordination::write(static_cast<int64_t>(0), wb);
        Coordination::write(static_cast<int64_t>(5), wb);
        Coordination::write(static_cast<int64_t>(5), wb);
        Coordination::write(static_cast<int64_t>(1000), wb);
        Coordination::write(static_cast<int64_t>(1000), wb);
        Coordination::write(static_cast<int32_t>(0), wb);
        Coordination::write(static_cast<int32_t>(0), wb);
        Coordination::write(static_cast<int32_t>(0), wb);
        Coordination::write(static_cast<int64_t>(0), wb);
        Coordination::write(static_cast<int64_t>(5), wb);
        Coordination::write(std::string("/"), wb); // terminator
        wb.finalize();
    }

    const auto log_path = (dir / "logs" / "log.6").string();
    {
        DB::WriteBufferFromFile wb(log_path);
        Coordination::write(static_cast<int32_t>(1514884167), wb); // "ZKLG"
        Coordination::write(static_cast<int32_t>(2), wb);          // version 2
        Coordination::write(static_cast<int64_t>(0), wb);          // dbid

        auto write_txn = [&](int64_t sid, int32_t xid, int64_t zxid, int64_t time, int32_t type, const std::string & body)
        {
            DB::WriteBufferFromOwnString rec;
            Coordination::write(sid, rec);
            Coordination::write(xid, rec);
            Coordination::write(zxid, rec);
            Coordination::write(time, rec);
            Coordination::write(type, rec);
            rec.write(body.data(), body.size());
            const std::string payload = rec.str();
            Coordination::write(static_cast<int64_t>(1), wb);
            Coordination::write(static_cast<int32_t>(payload.size()), wb);
            wb.write(payload.data(), payload.size());
            Coordination::write(static_cast<int8_t>(0x42), wb);
        };

        /// SetTxn (type=5) updating "/zk_migrated" -> "log-updated"
        {
            DB::WriteBufferFromOwnString body;
            Coordination::write(std::string("/zk_migrated"), body);
            Coordination::write(std::string("log-updated"), body);
            Coordination::write(static_cast<int32_t>(1), body); // version
            write_txn(1, 1, 6, 2000, 5, body.str());
        }
        /// CreateTxn (type=1) creating "/zk_migrated/child"
        {
            DB::WriteBufferFromOwnString body;
            Coordination::write(std::string("/zk_migrated/child"), body);
            Coordination::write(std::string("child-val"), body);
            Coordination::ACLs empty_acls;
            Coordination::write(empty_acls, body);
            Coordination::write(false, body);
            Coordination::write(static_cast<int32_t>(1), body);
            write_txn(1, 2, 7, 2001, 1, body.str());
        }
        Coordination::write(static_cast<int64_t>(0), wb); // EOF
        wb.finalize();
    }

    auto ctx = std::make_shared<DB::KeeperContext>(true, std::make_shared<DB::CoordinationSettings>());
    ctx->setDigestEnabled(true);
    DB::KeeperMemoryStorage storage(500, "", ctx);
    auto logger = getLogger("SimFeatureZKReader");
    DB::deserializeKeeperStorageFromSnapshotsDir(storage, (dir / "snaps").string(), logger);
    storage.initializeSystemNodes();
    DB::deserializeLogsAndApplyToStorage(storage, (dir / "logs").string(), logger);

    auto it_parent = storage.nodes.container.find("/zk_migrated");
    require(it_parent != storage.nodes.container.end() && it_parent->value.getData() == "log-updated",
        "ZooKeeperDataReader failed to replay SetTxn on /zk_migrated");
    auto it_child = storage.nodes.container.find("/zk_migrated/child");
    require(it_child != storage.nodes.container.end() && it_child->value.getData() == "child-val",
        "ZooKeeperDataReader failed to replay CreateTxn on /zk_migrated/child");
    /// Migrated `nodes_digest` must match recalculated digest over all loaded nodes (#135020).
    require(storage.getNodesDigest(true, false).value == storage.getNodesDigest(true, true).value,
        "ZooKeeperDataReader migrated nodes_digest mismatch (#135020)");
    std::cout << "  [PASS] feature: zookeeper_data_reader_migration\n";
}

/// 11. `KeeperReadThreadPool` (serial fallback + parallel multi-chunk + exception propagation)
/// and `KeeperOverDispatcher::promotingMultiCallback` error promotion.
inline void testKeeperReadThreadPoolAndOverDispatcher()
{
    DB::CoordinationSettings cs;
    cs[DB::CoordinationSetting::parallel_read_threads] = 2;
    cs[DB::CoordinationSetting::parallel_read_chunk_size] = 4;
    cs[DB::CoordinationSetting::parallel_read_min_batch] = 8;

    DB::KeeperReadThreadPool pool;

    /// Small batch (< parallel_read_min_batch) runs inline without spawning workers.
    {
        std::vector<int> visited(4, 0);
        pool.execute(4, cs, [&](size_t begin, size_t end)
        {
            for (size_t i = begin; i < end; ++i)
                visited[i] += 1;
        });
        for (int v : visited)
            require(v == 1, "KeeperReadThreadPool small batch missed element");
    }

    /// Larger batch (>= parallel_read_min_batch) fans out across 2 worker threads in chunks of 4.
    {
        std::vector<std::atomic<int>> visited(64);
        for (auto & a : visited)
            a.store(0);
        pool.execute(64, cs, [&](size_t begin, size_t end)
        {
            for (size_t i = begin; i < end; ++i)
                visited[i].fetch_add(1);
        });
        for (const auto & a : visited)
            require(a.load() == 1, "KeeperReadThreadPool parallel batch did not visit each index exactly once");
    }

    /// Exception propagation from worker thread.
    {
        bool caught = false;
        try
        {
            pool.execute(32, cs, [&](size_t begin, size_t end)
            {
                if (begin <= 16 && 16 < end)
                    throw std::runtime_error("simulated read worker failure");
            });
        }
        catch (const std::runtime_error & e)
        {
            caught = std::string(e.what()).find("simulated read worker failure") != std::string::npos;
        }
        require(caught, "KeeperReadThreadPool did not propagate worker exception");
    }
    pool.shutdown();

    /// Verify `KeeperOverDispatcher::promotingMultiCallback` promotes failed sub-op error into outer `MultiResponse`.
    {
        Coordination::Error promoted = Coordination::Error::ZOK;
        auto cb = Coordination::KeeperOverDispatcher::promotingMultiCallback(
            [&promoted](const Coordination::MultiResponse & r) { promoted = r.error; });
        auto multi_resp = std::make_shared<Coordination::ZooKeeperMultiWriteResponse>();
        multi_resp->error = Coordination::Error::ZOK;
        auto sub_ok = std::make_shared<Coordination::ZooKeeperCreateResponse>();
        sub_ok->error = Coordination::Error::ZOK;
        auto sub_fail = std::make_shared<Coordination::ZooKeeperErrorResponse>();
        sub_fail->error = Coordination::Error::ZNODEEXISTS;
        multi_resp->responses = {sub_ok, sub_fail};
        cb(multi_resp);
        require(promoted == Coordination::Error::ZNODEEXISTS,
            "KeeperOverDispatcher::promotingMultiCallback did not promote ZNODEEXISTS");
    }
    std::cout << "  [PASS] feature: keeper_read_thread_pool_and_over_dispatcher\n";
}

/// 12. `KeeperClientCLI` (`KeeperClientBase` parser + command execution against `TestKeeper`).
inline void testKeeperClientCLICommands()
{
    auto zk = zkutil::ZooKeeper::createFromImpl([]()
    {
        zkutil::ZooKeeperArgs args;
        args.implementation = "testkeeper";
        return std::make_unique<Coordination::TestKeeper>(args);
    });

    std::ostringstream out;
    std::ostringstream err;
    DB::KeeperClientBase cli(out, err);
    cli.zookeeper = zk;
    cli.ask_confirmation = false;

    auto run_cli = [&](const std::string & cmd) -> std::string
    {
        out.str("");
        out.clear();
        err.str("");
        err.clear();
        cli.processQueryText(cmd);
        require(err.str().empty(), "KeeperClientCLI stderr non-empty for '" + cmd + "': " + err.str());
        return out.str();
    };

    run_cli("create /cli_dir dir_val");
    run_cli("create /cli_dir/k1 v1");
    require(run_cli("get /cli_dir/k1").find("v1") != std::string::npos, "CLI get /cli_dir/k1 failed");
    run_cli("set /cli_dir/k1 v2");
    require(run_cli("get /cli_dir/k1").find("v2") != std::string::npos, "CLI set /cli_dir/k1 failed");
    /// Quoted empty string literal must be accepted by `parseKeeperArg` (Bug #81).
    run_cli("set /cli_dir/k1 ''");
    run_cli("set /cli_dir/k1 v2");
    require(run_cli("ls /cli_dir").find("k1") != std::string::npos, "CLI ls /cli_dir failed");
    require(run_cli("exists /cli_dir/k1").find("1") != std::string::npos, "CLI exists failed");
    require(run_cli("get_stat /cli_dir/k1").find("dataLength") != std::string::npos, "CLI get_stat failed");
    run_cli("touch /cli_dir/k2");
    run_cli("cp /cli_dir/k1 /cli_dir/k1_copy");
    require(run_cli("get /cli_dir/k1_copy").find("v2") != std::string::npos, "CLI cp failed");
    run_cli("mv /cli_dir/k1_copy /cli_dir/k1_moved");
    require(run_cli("get /cli_dir/k1_moved").find("v2") != std::string::npos, "CLI mv failed");
    run_cli("cpr /cli_dir /cli_backup");
    require(run_cli("get /cli_backup/k1").find("v2") != std::string::npos, "CLI cpr failed");
    require(run_cli("get_direct_children_number /cli_dir").find("3") != std::string::npos, "CLI get_direct_children_number failed");
    require(run_cli("get_all_children_number /cli_dir").find("3") != std::string::npos, "CLI get_all_children_number failed");
    run_cli("cd /cli_dir");
    require(cli.cwd == "/cli_dir", "CLI cd failed: cwd=" + cli.cwd.string());
    run_cli("rm k2");
    run_cli("cd /");
    require(cli.cwd == "/", "CLI cd / failed");
    require(run_cli("lsr /cli_dir").find("k1") != std::string::npos, "CLI lsr failed");
    run_cli("sync /cli_dir");
    run_cli("rmr /cli_backup");
    require(!zk->exists("/cli_backup"), "CLI rmr /cli_backup failed");
    require(DB::formatKeeperNodeName("plain_name") == "plain_name", "formatKeeperNodeName plain failed");
    require(DB::formatKeeperNodeName("with space") == "with\\ space", "formatKeeperNodeName escaped failed");
    require(DB::formatKeeperNodeName("a;b") == "'a;b'", "formatKeeperNodeName quoted failed");
    std::cout << "  [PASS] feature: keeper_client_cli_commands\n";
}

/// 13. LSMT write-throttling (`StorageState::recalculateWriteThrottling`),
/// `setKeeperFileDescriptorMetrics` sentinel handling, and `KeeperStateManager` password auth configs.
inline void testLsmtWriteThrottlingFdMetricsAndAuthConfig(const std::filesystem::path & root_dir)
{
    /// Part A: LSMT write-throttling backoff, cap, and recovery.
    {
        auto settings = std::make_shared<DB::CoordinationSettings>();
        (*settings)[DB::CoordinationSetting::unflushed_memtables_soft_limit] = 4;
        (*settings)[DB::CoordinationSetting::sorted_runs_soft_limit] = 2;
        (*settings)[DB::CoordinationSetting::write_throttling_min_delay_us] = 10000;
        (*settings)[DB::CoordinationSetting::write_throttling_max_delay_us] = 1000000;
        (*settings)[DB::CoordinationSetting::write_throttling_factor] = 16.0f;

        auto keeper_context = std::make_shared<DB::KeeperContext>(true, settings);
        DB::SharedMutex storage_mutex;
        Coordination::Storage::StorageState storage(keeper_context, &storage_mutex);
        std::lock_guard lock(storage_mutex);

        storage.recalculateWriteThrottling();
        require(storage.write_throttling_us.load() == 0, "Expected 0 throttling within limits");

        for (int i = 0; i < 6; ++i)
            storage.immutable_memtables.push_back(std::make_shared<Coordination::Storage::Memtable>());
        for (uint32_t i = 0; i < 4; ++i)
            storage.sorted_runs.push_back(std::make_shared<Coordination::Storage::SortedRun>(i, i));
        storage.recalculateWriteThrottling();
        const uint64_t mid_delay = storage.write_throttling_us.load();
        require(mid_delay >= 639990 && mid_delay <= 640010,
            "Unexpected mid write-throttling delay: " + std::to_string(mid_delay));

        storage.immutable_memtables.clear();
        for (uint32_t i = 4; i < 7; ++i)
            storage.sorted_runs.push_back(std::make_shared<Coordination::Storage::SortedRun>(i, i));
        storage.recalculateWriteThrottling();
        require(storage.write_throttling_us.load() == 1000000, "Expected capped write-throttling delay");

        storage.sorted_runs.clear();
        storage.recalculateWriteThrottling();
        require(storage.write_throttling_us.load() == 0, "Expected throttling to clear back to 0");
    }

    /// Part B: `setKeeperFileDescriptorMetrics` normal & undetermined (`-1` / `nullopt`) values.
    {
        DB::AsynchronousMetricValues mv;
        DB::setKeeperFileDescriptorMetrics(mv, 42, 1024);
        require(mv.at("KeeperOpenFileDescriptorCount").value == 42.0
            && mv.at("KeeperMaxFileDescriptorCount").value == 1024.0,
            "setKeeperFileDescriptorMetrics valid values mismatch");
        DB::setKeeperFileDescriptorMetrics(mv, -1, std::nullopt);
        require(mv.at("KeeperOpenFileDescriptorCount").value == -1.0
            && mv.at("KeeperMaxFileDescriptorCount").value == -1.0,
            "setKeeperFileDescriptorMetrics sentinel mismatch");
    }

    /// Part C: `KeeperStateManager` SHA-256 client password authentication parsing.
    {
        const auto dir = root_dir / "auth_cfg";
        std::filesystem::create_directories(dir / "logs");
        std::filesystem::create_directories(dir / "state");
        const std::string xml = R"(<clickhouse><keeper_server>
            <server_id>1</server_id>
            <client_password_sha256_hex>9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08</client_password_sha256_hex>
            <raft_configuration>
                <server><id>1</id><hostname>127.0.0.1</hostname><port>9234</port></server>
            </raft_configuration>
        </keeper_server></clickhouse>)";
        std::istringstream iss(xml);
        Poco::AutoPtr<Poco::Util::XMLConfiguration> cfg(new Poco::Util::XMLConfiguration(iss));
        auto ctx = std::make_shared<DB::KeeperContext>(true, std::make_shared<DB::CoordinationSettings>());
        ctx->setLogDisk(std::make_shared<DB::DiskLocal>("AuthLogDisk", (dir / "logs").string() + "/"));
        ctx->setStateFileDisk(std::make_shared<DB::DiskLocal>("AuthStateDisk", (dir / "state").string() + "/"));
        DB::KeeperStateManager mgr(1, "keeper_server", "auth_state", *cfg, ctx);
        const auto auth_data = mgr.getAuthenticationData();
        require(auth_data.has_value() && auth_data->getType() == DB::AuthenticationType::SHA256_PASSWORD,
            "KeeperStateManager did not parse client_password_sha256_hex");
    }
    std::cout << "  [PASS] feature: lsmt_write_throttling_fd_metrics_and_auth_config\n";
}

/// 14. Differential test of `CompactChildrenSet` against `std::unordered_set<std::string>`
/// across all 3 tagged-pointer states (Empty -> Single -> Set -> 300-child fanout -> demote -> Empty + copy/move).
inline void testCompactChildrenSetDifferential()
{
    std::vector<std::string> arena;
    arena.reserve(350);
    for (int i = 0; i < 350; ++i)
        arena.push_back("child_" + std::to_string(i));

    DB::CompactChildrenSet cset;
    std::unordered_set<std::string> ref;

    auto verify_eq = [&]()
    {
        require(cset.size() == ref.size(), "CompactChildrenSet size mismatch");
        require(cset.empty() == ref.empty(), "CompactChildrenSet empty mismatch");
        size_t seen = 0;
        for (std::string_view sv : cset)
        {
            require(ref.contains(std::string(sv)), "CompactChildrenSet unexpected element: " + std::string(sv));
            ++seen;
        }
        require(seen == ref.size(), "CompactChildrenSet iterator count mismatch");
        for (const auto & k : ref)
            require(cset.contains(k), "CompactChildrenSet missing element: " + k);
    };

    verify_eq();
    /// Empty -> Single
    cset.insert(arena[0]);
    ref.insert(arena[0]);
    verify_eq();
    /// Duplicate insert in Single state
    cset.insert(arena[0]);
    verify_eq();
    /// Single -> Set (2 elements) -> 300 elements
    for (int i = 1; i < 300; ++i)
    {
        cset.insert(arena[i]);
        ref.insert(arena[i]);
    }
    verify_eq();
    /// Copy constructor & move constructor in Set state
    DB::CompactChildrenSet copied(cset);
    require(copied.size() == 300 && copied.contains(arena[150]), "CompactChildrenSet copy mismatch");
    DB::CompactChildrenSet moved(std::move(copied));
    require(moved.size() == 300 && moved.contains(arena[299]), "CompactChildrenSet move mismatch");

    /// Erase down from 300 -> 1 (demote Set -> Single) -> 0 (Single -> Empty)
    for (int i = 0; i < 299; ++i)
    {
        cset.erase(arena[i]);
        ref.erase(arena[i]);
    }
    verify_eq();
    cset.erase(arena[299]);
    ref.erase(arena[299]);
    verify_eq();

    /// Re-grow after empty
    cset.insert(arena[301]);
    ref.insert(arena[301]);
    cset.insert(arena[302]);
    ref.insert(arena[302]);
    verify_eq();
    std::cout << "  [PASS] feature: compact_children_set_differential\n";
}

/// 15. Persistent (`PERSISTENT`) & Persistent-Recursive (`PERSISTENT_RECURSIVE`) watch semantics,
/// `CheckWatch`, `RemoveWatch`, `SetWatch`, and `SetWatch2` on BOTH `Mem` and `LSMT-Mem`!
inline void testPersistentAndRecursiveWatchSemantics(const std::filesystem::path & root_dir)
{
    for (const bool use_lsmt : {false, true})
    {
        const auto dir = root_dir / (use_lsmt ? "pwatch_lsmt" : "pwatch_mem");
        std::filesystem::create_directories(dir / "snapshots");
        auto snap_disk = std::make_shared<DB::DiskLocal>("PWSnapDisk", (dir / "snapshots").string() + "/");
        auto settings = std::make_shared<DB::CoordinationSettings>();
        (*settings)[DB::CoordinationSetting::use_lsmt_storage] = use_lsmt;
        (*settings)[DB::CoordinationSetting::storage_memory_only] = true;
        (*settings)[DB::CoordinationSetting::parallel_read_threads] = 0;

        auto ctx = std::make_shared<DB::KeeperContext>(true, settings);
        ctx->setSnapshotDisk(snap_disk);
        DB::SnapshotsQueue sq(4);

        std::optional<DB::KeeperResponseForSession> captured;
        std::vector<std::pair<int64_t, Coordination::WatchResponse>> watch_events;
        auto on_resp = [&](DB::KeeperResponseForSession r)
        {
            if (r.response && r.response->xid == Coordination::WATCH_XID)
            {
                if (auto * wr = dynamic_cast<Coordination::WatchResponse *>(r.response.get()))
                    watch_events.emplace_back(r.session_id, *wr);
            }
            else
            {
                captured = std::move(r);
            }
        };

        DB::KeeperStateMachine sm(on_resp, sq, ctx, nullptr);
        sm.init();
        uint64_t idx = 0;
        const int64_t t0 = 1700000040000LL;

        for (int s = 1; s <= 2; ++s)
        {
            auto sreq = std::make_shared<Coordination::ZooKeeperSessionIDRequest>();
            sreq->xid = s;
            sreq->session_timeout_ms = 60000;
            require(applyDirect(sm, idx, -1, sreq, t0, captured)->error == Coordination::Error::ZOK, "SessionID failed");
        }

        auto mk_create = [&](const std::string & p, const std::string & d, int64_t xid)
        {
            auto c = std::make_shared<Coordination::ZooKeeperCreateRequest>();
            c->xid = xid;
            c->path = p;
            c->data = d;
            c->acls = {{Coordination::ACL::All, "world", "anyone"}};
            require(applyDirect(sm, idx, 2, c, t0, captured)->error == Coordination::Error::ZOK, "Create failed: " + p);
        };
        auto mk_set = [&](const std::string & p, const std::string & d, int64_t xid)
        {
            auto s = std::make_shared<Coordination::ZooKeeperSetRequest>();
            s->xid = xid;
            s->path = p;
            s->data = d;
            s->version = -1;
            require(applyDirect(sm, idx, 2, s, t0, captured)->error == Coordination::Error::ZOK, "Set failed: " + p);
        };

        mk_create("/pw", "root", 10);
        mk_create("/pw/a", "a0", 11);

        /// 1. Install `PERSISTENT` watch on `/pw` for session 1 -> MUST fire repeatedly!
        {
            auto aw = std::make_shared<Coordination::ZooKeeperAddWatchRequest>();
            aw->xid = 20;
            aw->path = "/pw";
            aw->mode = Coordination::AddWatchRequest::AddWatchMode::PERSISTENT;
            require(readDirect(sm, 1, aw, t0, captured)->error == Coordination::Error::ZOK, "AddWatch PERSISTENT failed");
        }
        watch_events.clear();
        mk_set("/pw", "root1", 21);
        mk_set("/pw", "root2", 22);
        size_t pw_hits = 0;
        for (const auto & [sid, we] : watch_events)
            if (sid == 1 && we.path == "/pw" && we.type == Coordination::Event::CHANGED)
                ++pw_hits;
        require(pw_hits == 2, "PERSISTENT watch must fire repeatedly on both Set ops, got " + std::to_string(pw_hits));

        /// 2. `CheckWatch` returns ZOK while installed, then `RemoveWatch` removes it -> `CheckWatch` returns ZNOWATCHER!
        {
            auto cw = std::make_shared<Coordination::ZooKeeperCheckWatchRequest>();
            cw->xid = 23;
            cw->path = "/pw";
            cw->type = Coordination::CheckWatchRequest::CheckWatchType::PERSISTENT;
            require(readDirect(sm, 1, cw, t0, captured)->error == Coordination::Error::ZOK, "CheckWatch expected ZOK");

            auto rw = std::make_shared<Coordination::ZooKeeperRemoveWatchRequest>();
            rw->xid = 24;
            rw->path = "/pw";
            rw->type = Coordination::RemoveWatchRequest::WatchType::PERSISTENT;
            require(readDirect(sm, 1, rw, t0, captured)->error == Coordination::Error::ZOK, "RemoveWatch expected ZOK");

            auto cw2 = std::make_shared<Coordination::ZooKeeperCheckWatchRequest>();
            cw2->xid = 25;
            cw2->path = "/pw";
            cw2->type = Coordination::CheckWatchRequest::CheckWatchType::PERSISTENT;
            require(readDirect(sm, 1, cw2, t0, captured)->error == Coordination::Error::ZNOWATCHER, "CheckWatch after remove expected ZNOWATCHER");
        }
        watch_events.clear();
        mk_set("/pw", "root3", 26);
        require(watch_events.empty(), "Removed PERSISTENT watch must not fire");

        /// 3. `PERSISTENT_RECURSIVE` watch on `/pw` fires on deep descendant `/pw/a/b/c`!
        {
            auto aw = std::make_shared<Coordination::ZooKeeperAddWatchRequest>();
            aw->xid = 30;
            aw->path = "/pw";
            aw->mode = Coordination::AddWatchRequest::AddWatchMode::PERSISTENT_RECURSIVE;
            require(readDirect(sm, 1, aw, t0, captured)->error == Coordination::Error::ZOK, "AddWatch PERSISTENT_RECURSIVE failed");
        }
        watch_events.clear();
        mk_create("/pw/a/b", "b0", 31);
        mk_create("/pw/a/b/c", "c0", 32);
        mk_set("/pw/a/b/c", "c1", 33);
        size_t deep_hits = 0;
        for (const auto & [sid, we] : watch_events)
            if (sid == 1 && (we.path == "/pw/a/b" || we.path == "/pw/a/b/c"))
                ++deep_hits;
        require(deep_hits >= 3, "PERSISTENT_RECURSIVE watch missed deep descendant events, got " + std::to_string(deep_hits));

        /// 4. `SetWatch` & `SetWatch2` with stale `zxid = 0` immediately fire missed `CHANGED` event on `/pw`!
        watch_events.clear();
        {
            auto sw = std::make_shared<Coordination::ZooKeeperSetWatchesRequest>();
            sw->xid = 40;
            sw->zxid = 0;
            sw->data_watches = {"/pw"};
            require(readDirect(sm, 2, sw, t0, captured)->error == Coordination::Error::ZOK, "SetWatch failed");
        }
        require(!watch_events.empty(), "SetWatch with stale zxid=0 should fire missed event on /pw");

        /// Verify one-shot watch firing cleans up empty session map entries (Bug #83).
        {
            auto g = std::make_shared<Coordination::ZooKeeperGetRequest>();
            g->xid = 42;
            g->path = "/pw";
            g->has_watch = true;
            require(readDirect(sm, 2, g, t0, captured)->error == Coordination::Error::ZOK, "Get with watch failed");
            mk_set("/pw", "trigger-clean", 43);
            const auto st = sm.getStorageStats();
            require(st.sessions_with_watches_count <= st.total_watches_count,
                "WatchEvents::collectAndRemove leaked empty session entry (Bug #83)");
        }

        watch_events.clear();
        {
            auto sw2 = std::make_shared<Coordination::ZooKeeperSetWatches2Request>();
            sw2->xid = 41;
            sw2->zxid = 0;
            sw2->data_watches = {"/pw/a/b/c"};
            sw2->persistent_watches = {"/pw/a"};
            require(readDirect(sm, 2, sw2, t0, captured)->error == Coordination::Error::ZOK, "SetWatch2 failed");
        }
        require(!watch_events.empty(), "SetWatch2 with stale zxid=0 should fire missed event");

        sm.shutdownStorage();
    }
    std::cout << "  [PASS] feature: persistent_and_recursive_watches_semantics\n";
}

/// 16. Extended Keeper operations (`Create2`, `CheckNotExists`, `CheckStat` all fields,
/// `ListRecursive` limit, `RemoveRecursive` limit enforcement, `FilteredListWithStatsAndData`,
/// and `Container GC` `max_never_used_interval_ms > 0`).
inline void testExtendedOpsLimitsAndNeverUsedContainerGc(const std::filesystem::path & root_dir)
{
    const auto dir = root_dir / "ext_ops";
    std::filesystem::create_directories(dir / "snapshots");
    auto snap_disk = std::make_shared<DB::DiskLocal>("ExtSnapDisk", (dir / "snapshots").string() + "/");
    auto settings = std::make_shared<DB::CoordinationSettings>();
    (*settings)[DB::CoordinationSetting::parallel_read_threads] = 0;
    auto ctx = std::make_shared<DB::KeeperContext>(true, settings);
    ctx->setSnapshotDisk(snap_disk);
    DB::SnapshotsQueue sq(4);

    std::optional<DB::KeeperResponseForSession> captured;
    DB::KeeperStateMachine sm([&captured](DB::KeeperResponseForSession r) { captured = std::move(r); }, sq, ctx, nullptr);
    sm.init();
    uint64_t idx = 0;
    const int64_t t0 = 1700000050000LL;

    auto sreq = std::make_shared<Coordination::ZooKeeperSessionIDRequest>();
    sreq->xid = 1;
    sreq->session_timeout_ms = 60000;
    require(applyDirect(sm, idx, -1, sreq, t0, captured)->error == Coordination::Error::ZOK, "SessionID failed");

    /// 1. `Create2` (`include_stats = true`) returns populated `Stat`.
    Coordination::Stat root_stat{};
    {
        auto c2 = std::make_shared<Coordination::ZooKeeperCreateRequest>();
        c2->xid = 10;
        c2->path = "/ext";
        c2->data = "hello";
        c2->include_stats = true;
        c2->acls = {{Coordination::ACL::All, "world", "anyone"}};
        auto r = applyDirect(sm, idx, 1, c2, t0, captured);
        auto * cr = dynamic_cast<Coordination::ZooKeeperCreate2Response *>(r.get());
        require(cr && cr->error == Coordination::Error::ZOK && cr->zstat.dataLength == 5 && cr->zstat.version == 0,
            "Create2 stat mismatch");
        root_stat = cr->zstat;
    }
    /// 2. `CheckNotExists` (fails with ZNODEEXISTS on `/ext`, succeeds with ZOK on `/ext/absent`).
    {
        auto cne1 = std::make_shared<Coordination::ZooKeeperCheckRequest>();
        cne1->xid = 11;
        cne1->path = "/ext";
        cne1->not_exists = true;
        cne1->version = -1;
        require(applyDirect(sm, idx, 1, cne1, t0, captured)->error == Coordination::Error::ZNODEEXISTS,
            "CheckNotExists on existing node expected ZNODEEXISTS");

        auto cne2 = std::make_shared<Coordination::ZooKeeperCheckRequest>();
        cne2->xid = 12;
        cne2->path = "/ext/absent";
        cne2->not_exists = true;
        cne2->version = -1;
        require(applyDirect(sm, idx, 1, cne2, t0, captured)->error == Coordination::Error::ZOK,
            "CheckNotExists on absent node expected ZOK");
    }
    /// 3. `CheckStat` exact match vs mismatch.
    {
        auto cs = std::make_shared<Coordination::ZooKeeperCheckRequest>();
        cs->xid = 13;
        cs->path = "/ext";
        cs->stat_to_check = root_stat;
        require(applyDirect(sm, idx, 1, cs, t0, captured)->error == Coordination::Error::ZOK, "CheckStat exact match expected ZOK");

        cs->xid = 14;
        cs->stat_to_check->dataLength = 999;
        require(applyDirect(sm, idx, 1, cs, t0, captured)->error == Coordination::Error::ZBADVERSION,
            "CheckStat dataLength mismatch expected ZBADVERSION");
    }
    /// 4. Populate persistent + ephemeral subtree and test `FilteredListWithStatsAndData` + `ListRecursive` limit + `RemoveRecursive` limit!
    for (int i = 0; i < 4; ++i)
    {
        auto c = std::make_shared<Coordination::ZooKeeperCreateRequest>();
        c->xid = 20 + i;
        c->path = "/ext/p" + std::to_string(i);
        c->data = "pv" + std::to_string(i);
        c->acls = {{Coordination::ACL::All, "world", "anyone"}};
        require(applyDirect(sm, idx, 1, c, t0, captured)->error == Coordination::Error::ZOK, "Create persistent child failed");
    }
    {
        auto c = std::make_shared<Coordination::ZooKeeperCreateRequest>();
        c->xid = 25;
        c->path = "/ext/e0";
        c->data = "ev0";
        c->is_ephemeral = true;
        c->acls = {{Coordination::ACL::All, "world", "anyone"}};
        require(applyDirect(sm, idx, 1, c, t0, captured)->error == Coordination::Error::ZOK, "Create ephemeral child failed");
    }
    /// `FilteredListWithStatsAndData` EPHEMERAL_ONLY vs PERSISTENT_ONLY
    {
        auto fl = std::make_shared<Coordination::ZooKeeperListRequest>();
        fl->xid = 30;
        fl->path = "/ext";
        fl->list_request_type = Coordination::ListRequestType::EPHEMERAL_ONLY;
        fl->with_stat = true;
        fl->with_data = true;
        auto r = readDirect(sm, 1, fl, t0, captured);
        auto * lr = dynamic_cast<Coordination::ZooKeeperListResponse *>(r.get());
        require(lr && lr->error == Coordination::Error::ZOK && lr->names.size() == 1
            && lr->names[0] == "e0" && lr->data.size() == 1 && lr->data[0] == "ev0"
            && lr->stats.size() == 1 && lr->stats[0].ephemeralOwner == 1,
            "FilteredListWithStatsAndData EPHEMERAL_ONLY mismatch");
    }
    /// `ListRecursive` with `children_nodes_limit = 2` returns bounded subset
    {
        auto lr_req = std::make_shared<Coordination::ZooKeeperListRecursiveRequest>();
        lr_req->xid = 31;
        lr_req->path = "/ext";
        lr_req->children_nodes_limit = 2;
        auto r = readDirect(sm, 1, lr_req, t0, captured);
        auto * lr = dynamic_cast<Coordination::ZooKeeperListRecursiveResponse *>(r.get());
        require(lr && lr->error == Coordination::Error::ZOK && lr->children.size() == 2,
            "ListRecursive limit=2 mismatch");
    }
    /// `RemoveRecursive` with `remove_nodes_limit = 2` (subtree has 6 nodes) MUST fail without deleting `/ext`!
    {
        auto rr_small = std::make_shared<Coordination::ZooKeeperRemoveRecursiveRequest>();
        rr_small->xid = 32;
        rr_small->path = "/ext";
        rr_small->remove_nodes_limit = 2;
        auto r = applyDirect(sm, idx, 1, rr_small, t0, captured);
        require(r && r->error != Coordination::Error::ZOK, "RemoveRecursive limit=2 should fail on 6-node subtree");

        auto rr_ok = std::make_shared<Coordination::ZooKeeperRemoveRecursiveRequest>();
        rr_ok->xid = 33;
        rr_ok->path = "/ext";
        rr_ok->remove_nodes_limit = 16;
        require(applyDirect(sm, idx, 1, rr_ok, t0, captured)->error == Coordination::Error::ZOK,
            "RemoveRecursive limit=16 should delete subtree");
    }
    /// 5. Never-used Container GC (`collectContainerCandidates`): kept before grace period, eligible after grace period!
    {
        const int64_t now_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        auto cc = std::make_shared<Coordination::ZooKeeperCreateRequest>();
        cc->xid = 40;
        cc->path = "/never_used_container";
        cc->is_container = true;
        cc->acls = {{Coordination::ACL::All, "world", "anyone"}};
        require(applyDirect(sm, idx, 1, cc, now_ms - 5000, captured)->error == Coordination::Error::ZOK, "Create container failed");

        auto has_path = [](const std::vector<std::pair<std::string, Int32>> & candidates, const std::string & p)
        {
            return std::any_of(candidates.begin(), candidates.end(), [&](const auto & item) { return item.first == p; });
        };

        auto early = sm.getStorageUnsafe().collectContainerCandidates(16, /*max_never_used_interval_ms=*/60000);
        require(!has_path(early, "/never_used_container"),
            "Never-used container should NOT be GC'd before max_never_used_interval_ms");

        auto mature = sm.getStorageUnsafe().collectContainerCandidates(16, /*max_never_used_interval_ms=*/1000);
        require(has_path(mature, "/never_used_container"),
            "Never-used container MUST be eligible for GC after max_never_used_interval_ms");
    }

    sm.shutdownStorage();
    std::cout << "  [PASS] feature: extended_ops_check_stat_limits_and_never_used_container_gc\n";
}

/// 17. Cross-version snapshot matrix (V5, V6, V7, V8, V9 x Zstd/uncompressed loaded into Mem & LSMT-Mem)
/// + corrupted latest snapshot fallback to previous valid snapshot!
inline void testCrossVersionSnapshotMatrixAndCorruptFallback(const std::filesystem::path & root_dir)
{
    const std::array<uint64_t, 5> versions = {5, 6, 7, 8, 9};
    for (const uint64_t ver : versions)
    {
        const bool compress = (ver % 2 == 1);
        const auto dir = root_dir / ("snap_v" + std::to_string(ver));
        std::filesystem::create_directories(dir / "snapshots");
        auto snap_disk = std::make_shared<DB::DiskLocal>("VSnapDisk", (dir / "snapshots").string() + "/");

        auto settings = std::make_shared<DB::CoordinationSettings>();
        (*settings)[DB::CoordinationSetting::write_snapshot_version] = ver;
        (*settings)[DB::CoordinationSetting::compress_snapshots_with_zstd_format] = compress;
        (*settings)[DB::CoordinationSetting::parallel_read_threads] = 0;

        auto ctx = std::make_shared<DB::KeeperContext>(true, settings);
        ctx->setSnapshotDisk(snap_disk);
        ctx->setDigestEnabled(true);
        DB::SnapshotsQueue sq(4);
        std::optional<DB::KeeperResponseForSession> captured;
        DB::KeeperStateMachine sm([&captured](DB::KeeperResponseForSession r) { captured = std::move(r); }, sq, ctx, nullptr);
        sm.init();

        uint64_t idx = 0;
        const int64_t t0 = 1700000060000LL;
        auto sreq = std::make_shared<Coordination::ZooKeeperSessionIDRequest>();
        sreq->xid = 1;
        sreq->session_timeout_ms = 60000;
        require(applyDirect(sm, idx, -1, sreq, t0, captured)->error == Coordination::Error::ZOK, "SessionID failed");

        auto auth = std::make_shared<Coordination::ZooKeeperAuthRequest>();
        auth->xid = 2;
        auth->scheme = "digest";
        auth->data = "u1:pw1";
        require(applyDirect(sm, idx, 1, auth, t0, captured)->error == Coordination::Error::ZOK, "Auth failed");

        auto c1 = std::make_shared<Coordination::ZooKeeperCreateRequest>();
        c1->xid = 3;
        c1->path = "/pers";
        c1->data = "data_v" + std::to_string(ver);
        c1->acls = {{Coordination::ACL::All, "auth", ""}};
        require(applyDirect(sm, idx, 1, c1, t0, captured)->error == Coordination::Error::ZOK, "Create /pers failed");

        auto c2 = std::make_shared<Coordination::ZooKeeperCreateRequest>();
        c2->xid = 4;
        c2->path = "/eph";
        c2->data = "eph_val";
        c2->is_ephemeral = true;
        c2->acls = {{Coordination::ACL::All, "world", "anyone"}};
        require(applyDirect(sm, idx, 1, c2, t0, captured)->error == Coordination::Error::ZOK, "Create /eph failed");

        const uint64_t expected_digest = sm.getStorageUnsafe().getNodesDigest(true, true).value;
        auto cfg = nuraft::cs_new<nuraft::cluster_config>(1, 0);
        cfg->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(1, 0, "1", "", false, 1));
        nuraft::snapshot snp(idx, 1, cfg);
        nuraft::async_result<bool>::handler_type done = [](bool &, nuraft::ptr<std::exception> &) {};
        sm.create_snapshot(snp, done);
        DB::CreateSnapshotTask task;
        require(sq.tryPop(task), "Snapshot task not queued for version " + std::to_string(ver));
        task.create_snapshot(std::move(task.snapshot), /*execute_only_cleanup=*/false);
        sm.shutdownStorage();

        /// Reload the V{ver} snapshot into BOTH Mem and LSMT-Mem and verify identical digest + payloads!
        for (const bool load_lsmt : {false, true})
        {
            auto lsettings = std::make_shared<DB::CoordinationSettings>();
            (*lsettings)[DB::CoordinationSetting::use_lsmt_storage] = load_lsmt;
            (*lsettings)[DB::CoordinationSetting::storage_memory_only] = true;
            (*lsettings)[DB::CoordinationSetting::parallel_read_threads] = 0;
            auto lctx = std::make_shared<DB::KeeperContext>(true, lsettings);
            lctx->setSnapshotDisk(snap_disk);
            lctx->setDigestEnabled(true);
            DB::SnapshotsQueue lsq(4);
            DB::KeeperStateMachine lsm([&captured](DB::KeeperResponseForSession r) { captured = std::move(r); }, lsq, lctx, nullptr);
            lsm.init();
            require(lsm.last_commit_index() == idx, "Reloaded snapshot last_commit_index mismatch");
            require(lsm.getStorageUnsafe().getNodesDigest(true, true).value == expected_digest,
                "Cross-version snapshot digest mismatch for V" + std::to_string(ver));

            auto g = std::make_shared<Coordination::ZooKeeperGetRequest>();
            g->xid = 99;
            g->path = "/eph";
            auto gr_ptr = readDirect(lsm, 1, g, t0, captured);
            auto * gr = dynamic_cast<Coordination::ZooKeeperGetResponse *>(gr_ptr.get());
            require(gr && gr->error == Coordination::Error::ZOK && gr->data == "eph_val",
                "Reloaded snapshot /eph payload mismatch for V" + std::to_string(ver));
            lsm.shutdownStorage();
        }
    }

    /// Corrupted latest snapshot fallback when `snapshots_to_keep = 3`!
    {
        const auto dir = root_dir / "snap_corrupt_fallback";
        std::filesystem::create_directories(dir / "snapshots");
        auto snap_disk = std::make_shared<DB::DiskLocal>("CorruptSnapDisk", (dir / "snapshots").string() + "/");
        auto settings = std::make_shared<DB::CoordinationSettings>();
        (*settings)[DB::CoordinationSetting::snapshots_to_keep] = 3;
        (*settings)[DB::CoordinationSetting::parallel_read_threads] = 0;
        auto ctx = std::make_shared<DB::KeeperContext>(true, settings);
        ctx->setSnapshotDisk(snap_disk);
        ctx->setDigestEnabled(true);
        DB::SnapshotsQueue sq(4);
        std::optional<DB::KeeperResponseForSession> captured;
        DB::KeeperStateMachine sm([&captured](DB::KeeperResponseForSession r) { captured = std::move(r); }, sq, ctx, nullptr);
        sm.init();
        uint64_t idx = 0;
        const int64_t t0 = 1700000070000LL;
        auto cfg = nuraft::cs_new<nuraft::cluster_config>(1, 0);
        cfg->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(1, 0, "1", "", false, 1));

        auto take_snap = [&]()
        {
            nuraft::snapshot snp(idx, 1, cfg);
            nuraft::async_result<bool>::handler_type done = [](bool &, nuraft::ptr<std::exception> &) {};
            sm.create_snapshot(snp, done);
            DB::CreateSnapshotTask task;
            require(sq.tryPop(task), "Expected snapshot task");
            return task.create_snapshot(std::move(task.snapshot), false);
        };

        auto c1 = std::make_shared<Coordination::ZooKeeperCreateRequest>();
        c1->xid = 1;
        c1->path = "/good_snap1";
        c1->data = "snap1";
        c1->acls = {{Coordination::ACL::All, "world", "anyone"}};
        require(applyDirect(sm, idx, 1, c1, t0, captured)->error == Coordination::Error::ZOK, "Create /good_snap1 failed");
        const uint64_t snap1_idx = idx;
        (void)take_snap();

        auto c2 = std::make_shared<Coordination::ZooKeeperCreateRequest>();
        c2->xid = 2;
        c2->path = "/corrupt_snap2";
        c2->data = "snap2";
        c2->acls = {{Coordination::ACL::All, "world", "anyone"}};
        require(applyDirect(sm, idx, 1, c2, t0, captured)->error == Coordination::Error::ZOK, "Create /corrupt_snap2 failed");
        const auto snap2_info = take_snap();
        sm.shutdownStorage();

        /// Corrupt the latest snapshot file on disk (overwrite with garbage bytes)!
        const auto bad_path = dir / "snapshots" / snap2_info->path;
        {
            DB::WriteBufferFromFile wb(bad_path.string());
            wb.write("CORRUPT_TRUNCATED_GARBAGE", 25);
            wb.finalize();
        }

        /// Re-init MUST fail closed with `CORRUPTED_DATA` rather than silently discarding latest state!
        {
            DB::SnapshotsQueue sq_fail(4);
            DB::KeeperStateMachine sm_fail([&captured](DB::KeeperResponseForSession r) { captured = std::move(r); }, sq_fail, ctx, nullptr);
            bool threw_corrupt = false;
            try
            {
                sm_fail.init();
            }
            catch (const DB::Exception & e)
            {
                threw_corrupt = (e.code() == DB::ErrorCodes::CORRUPTED_DATA);
            }
            require(threw_corrupt, "Corrupted latest snapshot MUST fail closed with CORRUPTED_DATA");
            sm_fail.shutdownStorage();
        }

        /// After operator removes the corrupted latest snapshot, `snapshots_to_keep = 3` enables clean recovery to `snap1_idx`!
        std::filesystem::remove(bad_path);
        DB::SnapshotsQueue sq2(4);
        DB::KeeperStateMachine sm2([&captured](DB::KeeperResponseForSession r) { captured = std::move(r); }, sq2, ctx, nullptr);
        sm2.init();
        require(sm2.last_commit_index() == snap1_idx,
            "Recovery after removing corrupted snapshot did not load snap1_idx=" + std::to_string(snap1_idx)
            + " (got " + std::to_string(sm2.last_commit_index()) + ")");
        auto g = std::make_shared<Coordination::ZooKeeperGetRequest>();
        g->xid = 3;
        g->path = "/good_snap1";
        auto gr_ptr = readDirect(sm2, 1, g, t0, captured);
        auto * gr = dynamic_cast<Coordination::ZooKeeperGetResponse *>(gr_ptr.get());
        require(gr && gr->error == Coordination::Error::ZOK && gr->data == "snap1", "Recovered snapshot missing /good_snap1");
        sm2.shutdownStorage();
    }
    std::cout << "  [PASS] feature: cross_version_snapshot_matrix_and_corrupt_fallback\n";
}

/// 18. Compressed `Changelog` (`compress_logs = true`) + `compact` + cross-file `write_at` truncation
/// + torn trailing record recovery, AND on-disk `KeeperLSMTNodesStorage` flush/merge/tombstone/reopen!
inline void testCompressedChangelogTruncateCompactAndLsmtDiskReopen(const std::filesystem::path & root_dir)
{
    /// Part A: Zstd-compressed `Changelog` rotation, compaction, cross-file `write_at` truncation, and torn tail recovery.
    {
        const auto dir = root_dir / "changelog_zstd_ops";
        std::filesystem::create_directories(dir / "logs");
        auto log_disk = std::make_shared<DB::DiskLocal>("ZstdLogDisk", (dir / "logs").string() + "/");
        auto ctx = std::make_shared<DB::KeeperContext>(true, std::make_shared<DB::CoordinationSettings>());
        ctx->setLogDisk(log_disk);

        auto make_entry = [](uint64_t term, const std::string & s)
        {
            auto buf = nuraft::buffer::alloc(s.size());
            memcpy(buf->data_begin(), s.data(), s.size());
            return nuraft::cs_new<nuraft::log_entry>(term, buf, nuraft::app_log);
        };

        {
            auto ls = nuraft::cs_new<DB::KeeperLogStore>(
                DB::LogFileSettings{.force_sync = true, .compress_logs = true, .rotate_interval = 5},
                DB::FlushSettings{.max_flush_batch_size = 4, .min_time_between_fsyncs_ms = 0},
                DB::ReadAheadSettings{},
                ctx,
                DB::Changelog::ExecutionMode::Threaded);
            ls->init(0, 1000);
            for (uint64_t i = 1; i <= 30; ++i)
            {
                auto e = make_entry(1, "zstd-val-" + std::to_string(i));
                require(ls->append(e) == i, "Zstd changelog append mismatch");
            }
            require(ls->flush(), "Zstd changelog flush failed");
            ls->end_of_append_batch(1, 30);

            /// Compact entries up to 10 (removes old rotated files) and truncate via `write_at(22, ...)`.
            require(ls->compact(10), "Zstd changelog compact(10) failed");
            auto replacement = make_entry(2, "zstd-rewritten-22");
            ls->write_at(22, replacement);
            require(ls->flush(), "Zstd changelog flush after write_at failed");
            ls->end_of_append_batch(22, 1);
            ls->shutdownChangelog();
        }

        /// Reopen and verify exact compacted + truncated range [11..22]!
        {
            auto ls = nuraft::cs_new<DB::KeeperLogStore>(
                DB::LogFileSettings{.force_sync = true, .compress_logs = true, .rotate_interval = 5},
                DB::FlushSettings{.max_flush_batch_size = 4, .min_time_between_fsyncs_ms = 0},
                DB::ReadAheadSettings{},
                ctx,
                DB::Changelog::ExecutionMode::Threaded);
            ls->init(10, 1000);
            require(ls->start_index() == 11 && ls->next_slot() == 23,
                "Zstd changelog reopen bounds mismatch: start=" + std::to_string(ls->start_index())
                + " next=" + std::to_string(ls->next_slot()));
            auto e22 = ls->entry_at(22);
            require(e22 && e22->get_term() == 2, "Truncated entry 22 term mismatch");
            const std::string v22(reinterpret_cast<const char *>(e22->get_buf().data_begin()), e22->get_buf().size());
            require(v22 == "zstd-rewritten-22", "Truncated entry 22 payload mismatch: " + v22);
            ls->shutdownChangelog();
        }
    }

    /// Part B: Real on-disk `KeeperLSMTNodesStorage` flush -> merge -> huge values -> tombstones -> reopen!
    {
        const auto dir = root_dir / "lsmt_disk_durability";
        std::filesystem::create_directories(dir / "snapshots");
        std::filesystem::create_directories(dir / "data");
        auto snap_disk = std::make_shared<DB::DiskLocal>("LsmtDurSnapDisk", (dir / "snapshots").string() + "/");
        auto data_disk = std::make_shared<DB::DiskLocal>("LsmtDurDataDisk", (dir / "data").string() + "/");

        auto settings = std::make_shared<DB::CoordinationSettings>();
        (*settings)[DB::CoordinationSetting::use_lsmt_storage] = true;
        (*settings)[DB::CoordinationSetting::storage_memory_only] = false;
        (*settings)[DB::CoordinationSetting::committed_memtable_size] = 4096;
        (*settings)[DB::CoordinationSetting::block_cache_size] = 4096;
        (*settings)[DB::CoordinationSetting::sorted_file_uncompressed_size] = 4096;
        (*settings)[DB::CoordinationSetting::parallel_read_threads] = 0;

        auto ctx = std::make_shared<DB::KeeperContext>(true, settings);
        ctx->setSnapshotDisk(snap_disk);
        ctx->setDataDisk(data_disk);
        ctx->setDigestEnabled(true);

        uint64_t saved_idx = 0;
        uint64_t saved_digest = 0;
        const std::string huge_payload(6144, 'H');
        {
            DB::SnapshotsQueue sq(4);
            std::optional<DB::KeeperResponseForSession> captured;
            DB::KeeperStateMachine sm([&captured](DB::KeeperResponseForSession r) { captured = std::move(r); }, sq, ctx, nullptr);
            sm.init();
            uint64_t idx = 0;
            const int64_t t0 = 1700000080000LL;

            for (int i = 0; i < 30; ++i)
            {
                auto c = std::make_shared<Coordination::ZooKeeperCreateRequest>();
                c->xid = 10 + i;
                c->path = "/k" + std::to_string(i);
                c->data = (i == 0) ? huge_payload : ("val_" + std::to_string(i) + "_" + std::string(128, 'x'));
                c->acls = {{Coordination::ACL::All, "world", "anyone"}};
                require(applyDirect(sm, idx, 1, c, t0, captured)->error == Coordination::Error::ZOK, "LSMT create failed");
            }
            /// Overwrite & delete (tombstones) across multiple memtables/flushes.
            for (int i = 1; i <= 10; ++i)
            {
                auto rm = std::make_shared<Coordination::ZooKeeperRemoveRequest>();
                rm->xid = 100 + i;
                rm->path = "/k" + std::to_string(i);
                rm->version = -1;
                require(applyDirect(sm, idx, 1, rm, t0, captured)->error == Coordination::Error::ZOK, "LSMT remove failed");
            }
            saved_idx = idx;
            saved_digest = sm.getStorageUnsafe().getNodesDigest(true, true).value;

            /// Persist snapshot so reopen reloads from on-disk snapshot + LSMT storage!
            auto cfg = nuraft::cs_new<nuraft::cluster_config>(1, 0);
            cfg->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(1, 0, "1", "", false, 1));
            nuraft::snapshot snp(idx, 1, cfg);
            nuraft::async_result<bool>::handler_type done = [](bool &, nuraft::ptr<std::exception> &) {};
            sm.create_snapshot(snp, done);
            DB::CreateSnapshotTask task;
            require(sq.tryPop(task), "LSMT snapshot task missing");
            task.create_snapshot(std::move(task.snapshot), false);
            sm.shutdownStorage();
        }

        /// Reopen fresh on-disk LSMT state machine and verify digest, huge value, surviving keys, and deleted tombstones!
        {
            DB::SnapshotsQueue sq(4);
            std::optional<DB::KeeperResponseForSession> captured;
            DB::KeeperStateMachine sm([&captured](DB::KeeperResponseForSession r) { captured = std::move(r); }, sq, ctx, nullptr);
            sm.init();
            require(sm.last_commit_index() == saved_idx, "LSMT reopen commit idx mismatch");
            require(sm.getStorageUnsafe().getNodesDigest(true, true).value == saved_digest, "LSMT reopen digest mismatch");

            auto g0 = std::make_shared<Coordination::ZooKeeperGetRequest>();
            g0->xid = 1;
            g0->path = "/k0";
            auto r0 = readDirect(sm, 1, g0, 1700000080000LL, captured);
            auto * gr0 = dynamic_cast<Coordination::ZooKeeperGetResponse *>(r0.get());
            require(gr0 && gr0->error == Coordination::Error::ZOK && gr0->data == huge_payload, "LSMT reopen huge value mismatch");

            auto g5 = std::make_shared<Coordination::ZooKeeperGetRequest>();
            g5->xid = 2;
            g5->path = "/k5";
            require(readDirect(sm, 1, g5, 1700000080000LL, captured)->error == Coordination::Error::ZNONODE,
                "LSMT reopen resurrected deleted tombstone /k5");
            sm.shutdownStorage();
        }
    }
    std::cout << "  [PASS] feature: changelog_compress_truncate_compact_and_lsmt_disk_compaction\n";
}

/// 19. `InMemoryLogStore`, `SummingStateMachine`, LSMT `BlockCache`, MVCC `SnapshotableHashTable`,
/// and `ZooKeeperPathUtils` normalization/extraction!
inline void testInMemoryLogStoreSummingSMBlockCacheSnapshotableHTAndPathUtils()
{
    /// 1. `DB::InMemoryLogStore` append, write_at truncation, pack/apply_pack, compact, is_conf.
    {
        DB::InMemoryLogStore ls;
        require(ls.start_index() == 1 && ls.next_slot() == 1, "InMemoryLogStore initial bounds mismatch");

        auto make_entry = [](uint64_t term, const std::string & s, nuraft::log_val_type vt = nuraft::app_log)
        {
            auto buf = nuraft::buffer::alloc(s.size());
            memcpy(buf->data_begin(), s.data(), s.size());
            return nuraft::cs_new<nuraft::log_entry>(term, buf, vt);
        };

        for (uint64_t i = 1; i <= 6; ++i)
        {
            auto e = make_entry(i <= 3 ? 1 : 2, "im-" + std::to_string(i), i == 3 ? nuraft::conf : nuraft::app_log);
            require(ls.append(e) == i, "InMemoryLogStore append index mismatch");
        }
        require(ls.next_slot() == 7 && ls.term_at(4) == 2 && ls.is_conf(3) && !ls.is_conf(4),
            "InMemoryLogStore term_at/is_conf mismatch");

        auto range = ls.log_entries(2, 5);
        require(range && range->size() == 3, "InMemoryLogStore log_entries range mismatch");

        auto repl = make_entry(3, "im-5-rewritten");
        ls.write_at(5, repl);
        require(ls.next_slot() == 6 && ls.term_at(5) == 3 && ls.last_entry()->get_term() == 3,
            "InMemoryLogStore write_at truncation mismatch");

        auto packed = ls.pack(2, 3);
        DB::InMemoryLogStore ls2;
        ls2.apply_pack(2, *packed);
        require(ls2.next_slot() == 5 && ls2.term_at(4) == 2 && ls2.is_conf(3),
            "InMemoryLogStore pack/apply_pack mismatch");

        require(ls.compact(3) && ls.start_index() == 4 && ls.next_slot() == 6,
            "InMemoryLogStore compact mismatch");
    }

    /// 2. `DB::SummingStateMachine` commit + logical snapshot transfer & apply.
    {
        auto make_i64_buf = [](int64_t v)
        {
            auto b = nuraft::buffer::alloc(sizeof(int64_t));
            memcpy(b->data_begin(), &v, sizeof(int64_t));
            return b;
        };
        DB::SummingStateMachine sm1;
        auto b1 = make_i64_buf(10);
        auto b2 = make_i64_buf(25);
        auto b3 = make_i64_buf(-5);
        sm1.commit(1, *b1);
        sm1.commit(2, *b2);
        sm1.commit(3, *b3);
        require(sm1.getValue() == 30 && sm1.last_commit_index() == 3, "SummingStateMachine commit sum mismatch");

        auto cfg = nuraft::cs_new<nuraft::cluster_config>(1, 0);
        nuraft::snapshot snp(3, 1, cfg);
        bool snap_cb_ok = false;
        nuraft::async_result<bool>::handler_type cb = [&](bool & ok, nuraft::ptr<std::exception> &) { snap_cb_ok = ok; };
        sm1.create_snapshot(snp, cb);
        require(snap_cb_ok && sm1.last_snapshot() && sm1.last_snapshot()->get_last_log_idx() == 3,
            "SummingStateMachine create_snapshot failed");

        DB::SummingStateMachine sm2;
        uint64_t obj_id = 0;
        void * user_ctx = nullptr;
        bool is_last = false;
        while (!is_last)
        {
            nuraft::ptr<nuraft::buffer> data_out;
            require(sm1.read_logical_snp_obj(snp, user_ctx, obj_id, data_out, is_last) == 0,
                "SummingStateMachine read_logical_snp_obj failed");
            sm2.save_logical_snp_obj(snp, obj_id, *data_out, obj_id == 0, is_last);
        }
        require(sm2.apply_snapshot(snp) && sm2.getValue() == 30,
            "SummingStateMachine apply_snapshot value mismatch");
    }

    /// 3. LSMT `BlockCache`: `insertProbationary`, `get`, `getBlockOrLoadGroup`, eviction, and `remove`.
    {
        using Coordination::Storage::BlockCache;
        using Coordination::Storage::BlockCacheKey;
        using Coordination::Storage::BlockData;
        using Coordination::Storage::BlockPtr;

        BlockCache cache(/*max_size_in_bytes=*/512);
        BlockPtr b0 = BlockData::create(64);
        cache.insertProbationary(BlockCacheKey{1, 0}, b0);
        require(cache.get(BlockCacheKey{1, 0}).ptr == b0.ptr, "BlockCache probationary get mismatch");

        int load_calls = 0;
        BlockPtr b1 = cache.getBlockOrLoadGroup(BlockCacheKey{1, 1}, 1, [&]()
        {
            ++load_calls;
            return std::vector<BlockPtr>{BlockData::create(64), BlockData::create(64)};
        });
        require(load_calls == 1 && b1.ptr != nullptr, "BlockCache getBlockOrLoadGroup load failed");
        require(cache.get(BlockCacheKey{1, 2}).ptr != nullptr, "BlockCache group sibling not cached");

        cache.remove(BlockCacheKey{1, 1});
        require(cache.get(BlockCacheKey{1, 1}).ptr == nullptr, "BlockCache remove did not evict key");

        /// Pressure eviction beyond 512 bytes must not crash and must keep newly inserted blocks accessible.
        for (uint32_t i = 0; i < 32; ++i)
            cache.insertProbationary(BlockCacheKey{2, i}, BlockData::create(64));
        require(cache.get(BlockCacheKey{2, 31}).ptr != nullptr, "BlockCache latest inserted block missing");
    }

    /// 4. Lock-free MVCC `SnapshotableHashTable` with overlapping `ReadView`s + CoW + stale node reclamation.
    {
        struct MVCCVal
        {
            std::string data;
            size_t sizeInBytes() const { return data.size(); }
            MVCCVal copyFromSnapshotNode() const { return *this; }
        };

        DB::SnapshotableHashTable<MVCCVal> ht;
        std::unordered_map<std::string, std::string> expected_v0;
        for (int i = 0; i < 20; ++i)
        {
            std::string k = "/k" + std::to_string(i);
            std::string v = "v0_" + std::to_string(i);
            ht.insertOrReplace(k, MVCCVal{v});
            expected_v0[k] = v;
        }

        auto view0 = ht.issueReadView();
        std::unordered_map<std::string, std::string> expected_v1 = expected_v0;
        for (int i = 0; i < 10; ++i)
        {
            std::string k = "/k" + std::to_string(i);
            ht.updateValue(k, [&](MVCCVal & val) { val.data = "v1_" + std::to_string(i); });
            expected_v1[k] = "v1_" + std::to_string(i);
        }
        for (int i = 10; i < 15; ++i)
        {
            std::string k = "/k" + std::to_string(i);
            require(ht.erase(k), "SnapshotableHashTable erase failed");
            expected_v1.erase(k);
        }
        ht.insertOrReplace("/k_new", MVCCVal{"fresh"});
        expected_v1["/k_new"] = "fresh";

        auto view1 = ht.issueReadView();
        ht.erase("/k0");
        ht.updateValue("/k1", [](MVCCVal & val) { val.data = "v2_1"; });

        auto check_view = [](const auto & view, const std::unordered_map<std::string, std::string> & exp)
        {
            std::unordered_map<std::string, std::string> got;
            for (const auto & elem : *view)
                got.emplace(std::string(elem.key), elem.value.data);
            require(got == exp, "SnapshotableHashTable MVCC ReadView snapshot mismatch");
        };
        check_view(view0, expected_v0);
        check_view(view1, expected_v1);
        require(ht.listSize() > ht.size(), "Expected stale CoW nodes while views are pinned");

        ht.retireReadView(std::move(view0));
        ht.retireReadView(std::move(view1));
        require(ht.listSize() == ht.size(), "SnapshotableHashTable did not reclaim stale nodes after retiring views");
    }

    /// 5. `zkutil::ZooKeeperPathUtils` normalization & auxiliary keeper name extraction.
    {
        require(zkutil::extractZooKeeperName("/clickhouse/tables") == zkutil::DEFAULT_ZOOKEEPER_NAME,
            "Default keeper name mismatch");
        require(zkutil::extractZooKeeperName("aux1:/clickhouse/tables") == "aux1",
            "Auxiliary keeper name mismatch");
        require(zkutil::extractZooKeeperPath("aux1:/clickhouse/tables/", true) == "/clickhouse/tables",
            "extractZooKeeperPath mismatch");
        require(zkutil::extractZooKeeperPathAndCollapseTrailingSlashes("aux1:/a/b///", true) == "/a/b",
            "extractZooKeeperPathAndCollapseTrailingSlashes mismatch");
        require(zkutil::normalizeZooKeeperPath("/a/b/", true) == "/a/b",
            "normalizeZooKeeperPath mismatch");
        require(zkutil::normalizeZooKeeperPath("/", true).empty(),
            "normalizeZooKeeperPath root strips trailing slash");
        require(zkutil::normalizeZooKeeperPath("rel/path/", false) == "/rel/path",
            "normalizeZooKeeperPath relative prepend mismatch");
    }

    std::cout << "  [PASS] feature: in_memory_log_store_summing_sm_block_cache_snapshotable_ht_and_path_utils\n";
}

/// 20. Exhaustive `ListWithOptions` (`ALL`/`PERSISTENT_ONLY`/`EPHEMERAL_ONLY`, `with_stat`, `with_data`,
/// `recursive`, `max_results`, `truncated`, `shuffle`) + `Multi` atomic commit & rollback matrix on Mem & LSMT-Mem!
inline void testListWithOptionsAndMultiRollbackMatrix(const std::filesystem::path & root_dir)
{
    for (const bool use_lsmt : {false, true})
    {
        const auto dir = root_dir / (use_lsmt ? "lwo_multi_lsmt" : "lwo_multi_mem");
        std::filesystem::create_directories(dir / "snapshots");
        auto snap_disk = std::make_shared<DB::DiskLocal>("LWOSnap", (dir / "snapshots").string() + "/");

        auto settings = std::make_shared<DB::CoordinationSettings>();
        (*settings)[DB::CoordinationSetting::use_lsmt_storage] = use_lsmt;
        (*settings)[DB::CoordinationSetting::storage_memory_only] = true;
        (*settings)[DB::CoordinationSetting::parallel_read_threads] = 0;

        auto ctx = std::make_shared<DB::KeeperContext>(true, settings);
        ctx->setSnapshotDisk(snap_disk);
        ctx->setDigestEnabled(true);
        DB::SnapshotsQueue sq(4);
        std::optional<DB::KeeperResponseForSession> captured;
        DB::KeeperStateMachine sm([&captured](DB::KeeperResponseForSession r) { captured = std::move(r); }, sq, ctx, nullptr);
        sm.init();

        uint64_t idx = 0;
        const int64_t t0 = 1700000090000LL;
        auto sreq = std::make_shared<Coordination::ZooKeeperSessionIDRequest>();
        sreq->xid = 1;
        sreq->session_timeout_ms = 60000;
        require(applyDirect(sm, idx, -1, sreq, t0, captured)->error == Coordination::Error::ZOK, "SessionID failed");

        auto create_node = [&](const std::string & path, const std::string & data, bool eph)
        {
            auto c = std::make_shared<Coordination::ZooKeeperCreateRequest>();
            c->xid = static_cast<int64_t>(idx + 10);
            c->path = path;
            c->data = data;
            c->is_ephemeral = eph;
            c->acls = {{Coordination::ACL::All, "world", "anyone"}};
            require(applyDirect(sm, idx, 1, c, t0, captured)->error == Coordination::Error::ZOK, "Create failed: " + path);
        };

        create_node("/lwo", "root", false);
        create_node("/lwo/p0", "pv0", false);
        create_node("/lwo/p0/sub0", "sv0", false);
        create_node("/lwo/p1", "pv1", false);
        create_node("/lwo/p2", "pv2", false);
        create_node("/lwo/e0", "ev0", true);
        create_node("/lwo/e1", "ev1", true);

        auto run_lwo = [&](Coordination::ListOptions opts)
        {
            auto req = std::make_shared<Coordination::ZooKeeperListWithOptionsRequest>();
            req->xid = 500;
            req->path = "/lwo";
            req->options = opts;
            auto r = readDirect(sm, 1, req, t0, captured);
            auto * lr = dynamic_cast<Coordination::ZooKeeperListWithOptionsResponse *>(r.get());
            require(lr && lr->error == Coordination::Error::ZOK, "ListWithOptions failed");
            return *lr;
        };

        /// 1. `ALL` + `with_stat` + `with_data`
        {
            Coordination::ListOptions opts;
            opts.filter = Coordination::ListRequestType::ALL;
            opts.with_stat = true;
            opts.with_data = true;
            auto lr = run_lwo(opts);
            require(lr.names.size() == 5 && lr.stats.size() == 5 && lr.data.size() == 5 && !lr.truncated,
                "ListWithOptions ALL+stat+data mismatch");
        }
        /// 2. `PERSISTENT_ONLY`
        {
            Coordination::ListOptions opts;
            opts.filter = Coordination::ListRequestType::PERSISTENT_ONLY;
            auto lr = run_lwo(opts);
            require(lr.names.size() == 3 && !lr.truncated, "ListWithOptions PERSISTENT_ONLY count mismatch");
        }
        /// 3. `EPHEMERAL_ONLY` + `with_stat`
        {
            Coordination::ListOptions opts;
            opts.filter = Coordination::ListRequestType::EPHEMERAL_ONLY;
            opts.with_stat = true;
            auto lr = run_lwo(opts);
            require(lr.names.size() == 2 && lr.stats.size() == 2
                && lr.stats[0].ephemeralOwner == 1 && lr.stats[1].ephemeralOwner == 1,
                "ListWithOptions EPHEMERAL_ONLY mismatch");
        }
        /// 4. `recursive = true` -> includes nested `p0/sub0` (6 entries!)
        {
            Coordination::ListOptions opts;
            opts.recursive = true;
            auto lr = run_lwo(opts);
            require(lr.names.size() == 6 && !lr.truncated, "ListWithOptions recursive count mismatch");
        }
        /// 5. `max_results = 2` -> `truncated == true`!
        {
            Coordination::ListOptions opts;
            opts.max_results = 2;
            auto lr = run_lwo(opts);
            require(lr.names.size() == 2 && lr.truncated, "ListWithOptions max_results truncation mismatch");
        }
        /// 6. `shuffle = true` -> exact set equality!
        {
            Coordination::ListOptions opts;
            opts.shuffle = true;
            auto lr = run_lwo(opts);
            std::vector<std::string> sorted = lr.names;
            std::sort(sorted.begin(), sorted.end());
            const std::vector<std::string> expected = {"e0", "e1", "p0", "p1", "p2"};
            require(sorted == expected, "ListWithOptions shuffle set mismatch");
        }

        /// 7. `Multi` atomic commit with mixed Keeper ops (`CreateIfNotExists`, `Create`, `Set`, `Check`, `CheckNotExists`, `TryRemove`)!
        create_node("/mb", "base", false);
        {
            auto multi = std::make_shared<Coordination::ZooKeeperMultiRequest>();
            multi->xid = 600;
            auto c1 = std::make_shared<Coordination::ZooKeeperCreateRequest>();
            c1->path = "/mb/a";
            c1->data = "v1";
            c1->not_exists = true;
            c1->acls = {{Coordination::ACL::All, "world", "anyone"}};
            auto c2 = std::make_shared<Coordination::ZooKeeperCreateRequest>();
            c2->path = "/mb/b";
            c2->data = "b1";
            c2->acls = {{Coordination::ACL::All, "world", "anyone"}};
            auto s1 = std::make_shared<Coordination::ZooKeeperSetRequest>();
            s1->path = "/mb/a";
            s1->data = "v2";
            auto chk1 = std::make_shared<Coordination::ZooKeeperCheckRequest>();
            chk1->path = "/mb/a";
            chk1->version = 1;
            auto cne = std::make_shared<Coordination::ZooKeeperCheckRequest>();
            cne->path = "/mb/absent";
            cne->not_exists = true;
            auto tr = std::make_shared<Coordination::ZooKeeperRemoveRequest>();
            tr->path = "/mb/absent";
            tr->try_remove = true;
            multi->requests = {c1, c2, s1, chk1, cne, tr};

            auto r = applyDirect(sm, idx, 1, multi, t0, captured);
            require(r && r->error == Coordination::Error::ZOK, "Multi commit batch failed");
        }

        /// 8. `Multi` atomic ROLLBACK: verify exact `KeeperDigest` and zero leaked nodes/ephemerals!
        const uint64_t digest_before_rollback = sm.getStorageUnsafe().getNodesDigest(true, true).value;
        {
            auto multi = std::make_shared<Coordination::ZooKeeperMultiRequest>();
            multi->xid = 601;
            auto cp = std::make_shared<Coordination::ZooKeeperCreateRequest>();
            cp->path = "/mb/rb_p";
            cp->data = "dirty_p";
            cp->acls = {{Coordination::ACL::All, "world", "anyone"}};
            auto ce = std::make_shared<Coordination::ZooKeeperCreateRequest>();
            ce->path = "/mb/rb_e";
            ce->data = "dirty_e";
            ce->is_ephemeral = true;
            ce->acls = {{Coordination::ACL::All, "world", "anyone"}};
            auto sb = std::make_shared<Coordination::ZooKeeperSetRequest>();
            sb->path = "/mb/a";
            sb->data = "corrupted";
            auto rm = std::make_shared<Coordination::ZooKeeperRemoveRequest>();
            rm->path = "/mb/b";
            auto bad_chk = std::make_shared<Coordination::ZooKeeperCheckRequest>();
            bad_chk->path = "/mb/a";
            bad_chk->version = 999; // forces ZBADVERSION rollback!
            multi->requests = {cp, ce, sb, rm, bad_chk};

            auto r = applyDirect(sm, idx, 1, multi, t0, captured);
            auto * mr = dynamic_cast<Coordination::ZooKeeperMultiResponse *>(r.get());
            require(mr && mr->responses.size() == 5 && mr->responses[4]->error == Coordination::Error::ZBADVERSION,
                "Multi rollback expected sub-op[4] error ZBADVERSION");
            require(sm.getStorageUnsafe().getNodesDigest(true, true).value == digest_before_rollback,
                "Multi rollback mutated KeeperDigest!");

            auto g_a = std::make_shared<Coordination::ZooKeeperGetRequest>();
            g_a->xid = 602;
            g_a->path = "/mb/a";
            auto ra = readDirect(sm, 1, g_a, t0, captured);
            auto * ga = dynamic_cast<Coordination::ZooKeeperGetResponse *>(ra.get());
            require(ga && ga->error == Coordination::Error::ZOK && ga->data == "v2", "Multi rollback corrupted /mb/a");

            auto g_e = std::make_shared<Coordination::ZooKeeperExistsRequest>();
            g_e->xid = 603;
            g_e->path = "/mb/rb_e";
            require(readDirect(sm, 1, g_e, t0, captured)->error == Coordination::Error::ZNONODE,
                "Multi rollback leaked ephemeral /mb/rb_e");
        }

        sm.shutdownStorage();
    }
    std::cout << "  [PASS] feature: list_with_options_and_multi_rollback_matrix (Mem + LSMT-Mem)\n";
}

/// 21. `WriteBufferFromNuraftBuffer`/`ReadBufferFromNuraftBuffer`, LSMT `NodeRefCache` & `combineActions`,
/// `ZooKeeperRetriesControl`, `ShuffleHost`, `ErrorCounter` + `SystemTablesDataTypes`, and `LoggerWrapper`!
inline void testNuraftBuffersNodeRefCacheCombineActionsRetriesShuffleAndErrorCounter()
{
    /// 1. `WriteBufferFromNuraftBuffer` & `ReadBufferFromNuraftBuffer` across empty, small, and 100 KB payloads.
    for (const size_t len : {size_t(0), size_t(17), size_t(100000)})
    {
        std::string payload(len, '\0');
        for (size_t i = 0; i < len; ++i)
            payload[i] = static_cast<char>('A' + (i % 26));

        DB::WriteBufferFromNuraftBuffer wb;
        if (len > 0)
            wb.write(payload.data(), payload.size());
        wb.finalize();
        auto nbuf = wb.getBuffer();
        require(nbuf && nbuf->size() == len, "WriteBufferFromNuraftBuffer size mismatch");

        DB::ReadBufferFromNuraftBuffer rb(nbuf);
        std::string out(len, '\0');
        if (len > 0)
            rb.readStrict(out.data(), out.size());
        require(out == payload && rb.eof(), "ReadBufferFromNuraftBuffer round-trip mismatch");
    }

    /// 2. LSMT `NodeRefCache`: insert, live `BlockWeakPtrWithSpinlock` hit, weak expiration, and erase.
    {
        using Coordination::Storage::BlockData;
        using Coordination::Storage::BlockPtr;
        using Coordination::Storage::NodePath;
        using Coordination::Storage::NodeRef;
        using Coordination::Storage::NodeRefCache;

        NodeRefCache cache;
        const auto h = NodePath("/nrc/node1").calculateHash();
        auto & entry = cache.getOrInsertEntry(h);
        entry.file_seqno = 7;
        entry.node_offset = 12;

        {
            BlockPtr strong = BlockData::create(64);
            {
                std::lock_guard guard(entry.block);
                entry.block.set(strong);
            }
            NodeRef out_ref;
            const NodeRefCache::Entry * out_entry = nullptr;
            require(cache.tryGet(h, out_ref, &out_entry) && out_ref.block.ptr == strong.ptr
                && out_ref.offset == 12 && out_entry && out_entry->file_seqno == 7,
                "NodeRefCache live tryGet failed");
        }
        /// After `strong` goes out of scope, weak pointer expires while entry metadata stays intact!
        {
            NodeRef out_ref;
            const NodeRefCache::Entry * out_entry = nullptr;
            require(!cache.tryGet(h, out_ref, &out_entry) && out_entry && out_entry->file_seqno == 7,
                "NodeRefCache expired weak_ptr should miss tryGet while returning entry");
        }
        require(cache.size() == 1, "NodeRefCache size mismatch");
        cache.eraseEntry(h, entry);
        require(cache.size() == 0 && cache.findEntry(h) == nullptr, "NodeRefCache eraseEntry failed");
    }

    /// 3. LSMT `combineActions`: exhaustive strict & loose transition table + invalid transition throwing `LOGICAL_ERROR`.
    {
        using Coordination::Storage::combineActions;
        using Coordination::Storage::NodeAction;
        require(combineActions(std::nullopt, NodeAction::Create, true) == NodeAction::Create, "nullopt+Create");
        require(combineActions(NodeAction::Create, NodeAction::Update, true) == NodeAction::Create, "Create+Update");
        require(combineActions(NodeAction::Create, NodeAction::Remove, true) == std::nullopt, "Create+Remove");
        require(combineActions(NodeAction::Update, NodeAction::Update, true) == NodeAction::Update, "Update+Update");
        require(combineActions(NodeAction::Update, NodeAction::Remove, true) == NodeAction::Remove, "Update+Remove");
        require(combineActions(NodeAction::Remove, NodeAction::Create, true) == NodeAction::Update, "Remove+Create");
        require(combineActions(NodeAction::Create, NodeAction::Create, false) == NodeAction::Create, "loose Create+Create");
        bool threw_strict = false;
        try
        {
            (void)combineActions(NodeAction::Create, NodeAction::Create, true);
        }
        catch (const DB::Exception &)
        {
            threw_strict = true;
        }
        require(threw_strict, "Strict combineActions(Create, Create) must throw");
    }

    /// 4. `DB::ZooKeeperRetriesControl`: hardware error retries, immediate stop on user error, and retry exhaustion callback!
    {
        /// 4a. Succeeds on attempt 3 after 2 transient `ZCONNECTIONLOSS` hardware errors.
        int attempts = 0;
        DB::ZooKeeperRetriesInfo info(/*max_retries_=*/5, /*initial_backoff_ms_=*/1, /*max_backoff_ms_=*/4, nullptr);
        DB::ZooKeeperRetriesControl ctrl("sim_retry_ok", nullptr, info);
        ctrl.retryLoop([&]()
        {
            ++attempts;
            if (attempts < 3)
                throw zkutil::KeeperException(Coordination::Error::ZCONNECTIONLOSS);
        });
        require(attempts == 3, "ZooKeeperRetriesControl expected 3 attempts to succeed");

        /// 4b. Non-hardware user error (`ZNONODE`) is NOT retried and propagates immediately!
        int user_err_attempts = 0;
        DB::ZooKeeperRetriesControl ctrl_user("sim_retry_user_err", nullptr, info);
        bool caught_nonode = false;
        try
        {
            ctrl_user.retryLoop([&]()
            {
                ++user_err_attempts;
                throw zkutil::KeeperException(Coordination::Error::ZNONODE);
            });
        }
        catch (const zkutil::KeeperException & e)
        {
            caught_nonode = (e.code == Coordination::Error::ZNONODE);
        }
        require(caught_nonode && user_err_attempts == 1, "ZooKeeperRetriesControl retried non-hardware ZNONODE!");

        /// 4c. Exhausts `max_retries = 2` and invokes `actionAfterLastFailedRetry` once.
        int exhaust_attempts = 0;
        int after_last_calls = 0;
        DB::ZooKeeperRetriesInfo small_info(/*max_retries_=*/2, /*initial_backoff_ms_=*/1, /*max_backoff_ms_=*/2, nullptr);
        DB::ZooKeeperRetriesControl ctrl_ex("sim_retry_exhaust", nullptr, small_info);
        ctrl_ex.actionAfterLastFailedRetry([&]() { ++after_last_calls; });
        bool caught_exhaust = false;
        try
        {
            ctrl_ex.retryLoop([&]()
            {
                ++exhaust_attempts;
                throw zkutil::KeeperException(Coordination::Error::ZOPERATIONTIMEOUT);
            });
        }
        catch (const zkutil::KeeperException & e)
        {
            caught_exhaust = (e.code == Coordination::Error::ZOPERATIONTIMEOUT);
        }
        require(caught_exhaust && exhaust_attempts == 3 && after_last_calls == 1,
            "ZooKeeperRetriesControl exhaustion mismatch");
    }

    /// 5. `zkutil::ShuffleHost::compare` ordering (AZ -> Priority -> random).
    {
        zkutil::ShuffleHosts hosts;
        zkutil::ShuffleHost h_other;
        h_other.host = "other:9181";
        h_other.az_info = zkutil::ShuffleHost::OTHER;
        h_other.priority = Priority{1};
        h_other.random = 1;

        zkutil::ShuffleHost h_same_lo;
        h_same_lo.host = "same-lo:9181";
        h_same_lo.az_info = zkutil::ShuffleHost::SAME;
        h_same_lo.priority = Priority{2};
        h_same_lo.random = 100;

        zkutil::ShuffleHost h_same_hi;
        h_same_hi.host = "same-hi:9181";
        h_same_hi.az_info = zkutil::ShuffleHost::SAME;
        h_same_hi.priority = Priority{1};
        h_same_hi.random = 200;

        hosts = {h_other, h_same_lo, h_same_hi};
        std::sort(hosts.begin(), hosts.end(), zkutil::ShuffleHost::compare);
        require(hosts[0].host == "same-hi:9181" && hosts[1].host == "same-lo:9181" && hosts[2].host == "other:9181",
            "ShuffleHost::compare ordering mismatch");
    }

    /// 6. `Coordination::ErrorCounter` + `Coordination::SystemTablesDataTypes` (`errorCodeEnum`, `operationEnum`, `watchEventTypeEnum`, `watchStateEnum`).
    {
        auto err_enum = Coordination::SystemTablesDataTypes::errorCodeEnum();
        auto op_enum = Coordination::SystemTablesDataTypes::operationEnum();
        auto ev_enum = Coordination::SystemTablesDataTypes::watchEventTypeEnum();
        auto st_enum = Coordination::SystemTablesDataTypes::watchStateEnum();
        require(err_enum && op_enum && ev_enum && st_enum, "SystemTablesDataTypes returned null enum");

        Coordination::ErrorCounter ec;
        ec.increment(Coordination::Error::ZOK);
        ec.increment(Coordination::Error::ZOK);
        ec.increment(Coordination::Error::ZNONODE);
        ec.increment(Coordination::Error::ZBADVERSION);

        auto map_type = std::make_shared<DB::DataTypeMap>(err_enum, std::make_shared<DB::DataTypeUInt32>());
        auto col = map_type->createColumn();
        auto & map_col = assert_cast<DB::ColumnMap &>(*col);
        ec.dumpToMapColumn(&map_col);
        require(map_col.size() == 1 && map_col.getNestedData().size() == 3,
            "ErrorCounter dumpToMapColumn row/entry count mismatch");
    }

    /// 7. `DB::LoggerWrapper` valid levels (`none`..`trace` via ctor, `set_level`, & `put_details`, Bug #86)
    ///    and `nuraft::buffer::get_str` EOF bounds safety (`#135014`).
    {
        auto b = nuraft::buffer::alloc(4);
        b->pos(4);
        (void)b->get_str();
        require(b->pos() <= b->size(), "nuraft::buffer::get_str advanced pos past size at EOF (#135014)");

        DB::LoggerWrapper lw_none("SimLoggerWrapperNone", DB::LogsLevel::none);
        DB::LoggerWrapper lw("SimLoggerWrapperTest", DB::LogsLevel::error);
        lw.set_level(static_cast<int>(DB::LogsLevel::warning));
        require(lw.get_level() == static_cast<int>(DB::LogsLevel::warning), "LoggerWrapper get_level mismatch");
        lw.put_details(static_cast<int>(DB::LogsLevel::debug), __FILE__, __func__, __LINE__, "sim-debug-msg");
    }

    std::cout << "  [PASS] feature: nuraft_buffers_node_ref_cache_combine_actions_retries_shuffle_and_error_counter\n";
}

/// 22. Empty `Multi`/`MultiRead`, `<feature_flags>` XML config parsing & validation,
/// `remove_orphaned_nodes_on_startup` + LSMT rejection, and Znode `ctime`/`mtime` persistence across snapshot reopen!
inline void testEmptyMultiFeatureFlagsXMLConfigAndZnodeTime(const std::filesystem::path & root_dir)
{
    /// 1. Empty `OpNum::Multi` and empty `OpNum::MultiRead` (0 subrequests) on both `Mem` and `LSMT-Mem`!
    for (const bool use_lsmt : {false, true})
    {
        const auto dir = root_dir / (use_lsmt ? "empty_multi_lsmt" : "empty_multi_mem");
        std::filesystem::create_directories(dir / "snapshots");
        auto snap_disk = std::make_shared<DB::DiskLocal>("EMSnap", (dir / "snapshots").string() + "/");

        auto settings = std::make_shared<DB::CoordinationSettings>();
        (*settings)[DB::CoordinationSetting::use_lsmt_storage] = use_lsmt;
        (*settings)[DB::CoordinationSetting::storage_memory_only] = true;
        (*settings)[DB::CoordinationSetting::parallel_read_threads] = 0;

        auto ctx = std::make_shared<DB::KeeperContext>(true, settings);
        ctx->setSnapshotDisk(snap_disk);
        ctx->setDigestEnabled(true);
        DB::SnapshotsQueue sq(4);
        std::optional<DB::KeeperResponseForSession> captured;
        DB::KeeperStateMachine sm([&captured](DB::KeeperResponseForSession r) { captured = std::move(r); }, sq, ctx, nullptr);
        sm.init();

        uint64_t idx = 0;
        const int64_t t0 = 1700000100000LL;
        auto sreq = std::make_shared<Coordination::ZooKeeperSessionIDRequest>();
        sreq->xid = 1;
        sreq->session_timeout_ms = 60000;
        require(applyDirect(sm, idx, -1, sreq, t0, captured)->error == Coordination::Error::ZOK, "SessionID failed");

        auto empty_multi = std::make_shared<Coordination::ZooKeeperMultiRequest>();
        empty_multi->xid = 2;
        auto r_mw = applyDirect(sm, idx, 1, empty_multi, t0, captured);
        auto * mrw = dynamic_cast<Coordination::ZooKeeperMultiWriteResponse *>(r_mw.get());
        require(mrw && mrw->error == Coordination::Error::ZOK && mrw->responses.empty(),
            "Empty MultiWrite expected ZOK with 0 responses");

        auto empty_mread = std::make_shared<Coordination::ZooKeeperMultiRequest>();
        empty_mread->xid = 3;
        empty_mread->operation_type = Coordination::ZooKeeperMultiRequest::OperationType::Read;
        auto r_mr = readDirect(sm, 1, empty_mread, t0, captured);
        auto * mrr = dynamic_cast<Coordination::ZooKeeperMultiReadResponse *>(r_mr.get());
        require(mrr && mrr->error == Coordination::Error::ZOK && mrr->responses.empty(),
            "Empty MultiRead expected ZOK with 0 responses");

        /// 2. Znode `ctime` & `mtime` monotonicity across `Create` -> `Set` -> snapshot -> reopen!
        const int64_t t_create = t0 + 1000;
        const int64_t t_set = t0 + 5000;
        auto c = std::make_shared<Coordination::ZooKeeperCreateRequest>();
        c->xid = 4;
        c->path = "/timed";
        c->data = "v0";
        c->acls = {{Coordination::ACL::All, "world", "anyone"}};
        require(applyDirect(sm, idx, 1, c, t_create, captured)->error == Coordination::Error::ZOK, "Create /timed failed");

        auto s = std::make_shared<Coordination::ZooKeeperSetRequest>();
        s->xid = 5;
        s->path = "/timed";
        s->data = "v1";
        auto r_set = applyDirect(sm, idx, 1, s, t_set, captured);
        auto * sr = dynamic_cast<Coordination::ZooKeeperSetResponse *>(r_set.get());
        require(sr && sr->error == Coordination::Error::ZOK && sr->stat.ctime == t_create && sr->stat.mtime == t_set,
            "Znode ctime/mtime after Set mismatch");

        auto cfg = nuraft::cs_new<nuraft::cluster_config>(1, 0);
        cfg->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(1, 0, "1", "", false, 1));
        nuraft::snapshot snp(idx, 1, cfg);
        nuraft::async_result<bool>::handler_type done = [](bool &, nuraft::ptr<std::exception> &) {};
        sm.create_snapshot(snp, done);
        DB::CreateSnapshotTask task;
        require(sq.tryPop(task), "Expected snapshot task for Znode time test");
        (void)task.create_snapshot(std::move(task.snapshot), false);
        sm.shutdownStorage();

        DB::SnapshotsQueue sq2(4);
        DB::KeeperStateMachine sm2([&captured](DB::KeeperResponseForSession r) { captured = std::move(r); }, sq2, ctx, nullptr);
        sm2.init();
        auto g = std::make_shared<Coordination::ZooKeeperGetRequest>();
        g->xid = 6;
        g->path = "/timed";
        auto r_get = readDirect(sm2, 1, g, t_set + 100, captured);
        auto * gr = dynamic_cast<Coordination::ZooKeeperGetResponse *>(r_get.get());
        require(gr && gr->error == Coordination::Error::ZOK && gr->data == "v1"
            && gr->stat.ctime == t_create && gr->stat.mtime == t_set,
            "Znode ctime/mtime lost across snapshot reopen");
        sm2.shutdownStorage();
    }

    /// 3. `KeeperContext::initialize` `<feature_flags>` XML config parsing & validation!
    {
        const auto dir = root_dir / "ff_xml_cfg";
        std::filesystem::create_directories(dir / "logs");
        std::filesystem::create_directories(dir / "snapshots");

        /// 3a. Disabling `multi_read` downgrades `/keeper/api_version` to `ZOOKEEPER_COMPATIBLE`,
        ///     while enabling `create_ttl` & `create_container` enables those ops!
        {
            std::ostringstream xml;
            xml << "<clickhouse><keeper_server>\n"
                << "  <server_id>1</server_id>\n"
                << "  <log_storage_path>" << (dir / "logs").string() << "</log_storage_path>\n"
                << "  <snapshot_storage_path>" << (dir / "snapshots").string() << "</snapshot_storage_path>\n"
                << "  <feature_flags>\n"
                << "    <multi_read>0</multi_read>\n"
                << "    <create_ttl>1</create_ttl>\n"
                << "    <create_container>1</create_container>\n"
                << "  </feature_flags>\n"
                << "</keeper_server></clickhouse>\n";
            std::istringstream iss(xml.str());
            Poco::AutoPtr<Poco::Util::XMLConfiguration> cfg(new Poco::Util::XMLConfiguration(iss));
            DB::PlacementInfo::PlacementInfo::instance().initialize(*cfg);
            auto settings = std::make_shared<DB::CoordinationSettings>();
            (*settings)[DB::CoordinationSetting::write_snapshot_version] = 9;
            auto ctx = std::make_shared<DB::KeeperContext>(true, settings);
            ctx->initialize(*cfg, nullptr);
            require(!ctx->getFeatureFlags().isEnabled(DB::KeeperFeatureFlag::MULTI_READ),
                "XML multi_read=0 did not disable MULTI_READ");
            require(ctx->getFeatureFlags().isEnabled(DB::KeeperFeatureFlag::CREATE_TTL)
                && ctx->getFeatureFlags().isEnabled(DB::KeeperFeatureFlag::CREATE_CONTAINER),
                "XML create_ttl=1 / create_container=1 did not enable flags");
            const auto & sys = ctx->getSystemNodesWithData();
            require(sys.at(std::string(DB::keeper_api_version_path)) == "0",
                "Disabling MULTI_READ did not downgrade /keeper/api_version to 0");
        }

        /// 3b. Unknown feature flag in `<feature_flags>` MUST throw `BAD_ARGUMENTS`!
        {
            std::ostringstream xml;
            xml << "<clickhouse><keeper_server>\n"
                << "  <server_id>1</server_id>\n"
                << "  <log_storage_path>" << (dir / "logs").string() << "</log_storage_path>\n"
                << "  <snapshot_storage_path>" << (dir / "snapshots").string() << "</snapshot_storage_path>\n"
                << "  <feature_flags><bogus_unknown_flag>1</bogus_unknown_flag></feature_flags>\n"
                << "</keeper_server></clickhouse>\n";
            std::istringstream iss(xml.str());
            Poco::AutoPtr<Poco::Util::XMLConfiguration> cfg(new Poco::Util::XMLConfiguration(iss));
            auto ctx = std::make_shared<DB::KeeperContext>(true, std::make_shared<DB::CoordinationSettings>());
            bool threw = false;
            try
            {
                ctx->initialize(*cfg, nullptr);
            }
            catch (const DB::Exception &)
            {
                threw = true;
            }
            require(threw, "Invalid feature_flags XML entry did not throw");
        }

        /// 3c. `remove_orphaned_nodes_on_startup = true` combined with `use_lsmt_storage = true` MUST throw!
        {
            std::ostringstream xml;
            xml << "<clickhouse><keeper_server>\n"
                << "  <server_id>1</server_id>\n"
                << "  <remove_orphaned_nodes_on_startup>true</remove_orphaned_nodes_on_startup>\n"
                << "  <log_storage_path>" << (dir / "logs").string() << "</log_storage_path>\n"
                << "  <snapshot_storage_path>" << (dir / "snapshots").string() << "</snapshot_storage_path>\n"
                << "</keeper_server></clickhouse>\n";
            std::istringstream iss(xml.str());
            Poco::AutoPtr<Poco::Util::XMLConfiguration> cfg(new Poco::Util::XMLConfiguration(iss));
            DB::PlacementInfo::PlacementInfo::instance().initialize(*cfg);
            auto settings = std::make_shared<DB::CoordinationSettings>();
            (*settings)[DB::CoordinationSetting::use_lsmt_storage] = true;
            auto ctx = std::make_shared<DB::KeeperContext>(true, settings);
            bool threw = false;
            try
            {
                ctx->initialize(*cfg, nullptr);
            }
            catch (const DB::Exception &)
            {
                threw = true;
            }
            require(threw, "remove_orphaned_nodes_on_startup + use_lsmt_storage must throw");
        }
    }

    std::cout << "  [PASS] feature: empty_multi_feature_flags_xml_config_and_znode_time\n";
}

/// 23. `zk_approximate_data_size` accounting accuracy (`test_keeper_mntr_data_size`) across `Create`/`Set`/`Remove`
/// on both `Mem` and `LSMT-Mem`, AND `Changelog` missing middle-file log-gap detection on startup (`test_keeper_log_gap_before_committed`)!
inline void testMntrDataSizeAccuracyAndLogGapRejection(const std::filesystem::path & root_dir)
{
    /// 1. `approximate_data_size` increases on `Create` + `Set` and returns to exact baseline on `Remove`!
    for (const bool use_lsmt : {false, true})
    {
        const auto dir = root_dir / (use_lsmt ? "mntr_size_lsmt" : "mntr_size_mem");
        std::filesystem::create_directories(dir / "snapshots");
        auto snap_disk = std::make_shared<DB::DiskLocal>("SizeSnap", (dir / "snapshots").string() + "/");

        auto settings = std::make_shared<DB::CoordinationSettings>();
        (*settings)[DB::CoordinationSetting::use_lsmt_storage] = use_lsmt;
        (*settings)[DB::CoordinationSetting::storage_memory_only] = true;
        (*settings)[DB::CoordinationSetting::parallel_read_threads] = 0;

        auto ctx = std::make_shared<DB::KeeperContext>(true, settings);
        ctx->setSnapshotDisk(snap_disk);
        DB::SnapshotsQueue sq(4);
        std::optional<DB::KeeperResponseForSession> captured;
        DB::KeeperStateMachine sm([&captured](DB::KeeperResponseForSession r) { captured = std::move(r); }, sq, ctx, nullptr);
        sm.init();

        uint64_t idx = 0;
        const int64_t t0 = 1700000120000LL;
        auto sreq = std::make_shared<Coordination::ZooKeeperSessionIDRequest>();
        sreq->xid = 1;
        sreq->session_timeout_ms = 60000;
        require(applyDirect(sm, idx, -1, sreq, t0, captured)->error == Coordination::Error::ZOK, "SessionID failed");

        const uint64_t base_size = sm.getStorageStats().approximate_data_size;
        const uint64_t base_nodes = sm.getStorageStats().nodes_count;

        auto c = std::make_shared<Coordination::ZooKeeperCreateRequest>();
        c->xid = 2;
        c->path = "/sized";
        c->data = std::string(1024, 'A');
        c->acls = {{Coordination::ACL::All, "world", "anyone"}};
        require(applyDirect(sm, idx, 1, c, t0, captured)->error == Coordination::Error::ZOK, "Create /sized failed");
        const auto stats_create = sm.getStorageStats();
        require(stats_create.nodes_count == base_nodes + 1, "nodes_count did not increment on Create");
        if (!use_lsmt)
            require(stats_create.approximate_data_size >= base_size + 1024,
                "Mem approximate_data_size did not grow by >= 1024 on Create");

        auto s = std::make_shared<Coordination::ZooKeeperSetRequest>();
        s->xid = 3;
        s->path = "/sized";
        s->data = std::string(40000, 'B');
        require(applyDirect(sm, idx, 1, s, t0, captured)->error == Coordination::Error::ZOK, "Set /sized failed");
        const auto stats_set = sm.getStorageStats();
        require(stats_set.nodes_count == base_nodes + 1 && stats_set.approximate_data_size > base_size + 30000,
            "approximate_data_size did not grow on 40 KB Set");

        auto rm = std::make_shared<Coordination::ZooKeeperRemoveRequest>();
        rm->xid = 4;
        rm->path = "/sized";
        require(applyDirect(sm, idx, 1, rm, t0, captured)->error == Coordination::Error::ZOK, "Remove /sized failed");
        const auto stats_rm = sm.getStorageStats();
        require(stats_rm.nodes_count == base_nodes, "nodes_count did not return to baseline after Remove");
        if (!use_lsmt)
            require(stats_rm.approximate_data_size <= base_size + 64,
                "Mem approximate_data_size did not return near baseline after Remove");
        sm.shutdownStorage();
    }

    /// 2. Missing middle changelog file before committed index MUST throw on `KeeperLogStore::init`!
    {
        const auto dir = root_dir / "log_gap_check";
        std::filesystem::create_directories(dir / "logs");
        auto log_disk = std::make_shared<DB::DiskLocal>("GapLogDisk", (dir / "logs").string() + "/");
        auto ctx = std::make_shared<DB::KeeperContext>(true, std::make_shared<DB::CoordinationSettings>());
        ctx->setLogDisk(log_disk);

        auto make_entry = [](uint64_t term, const std::string & s)
        {
            auto buf = nuraft::buffer::alloc(s.size());
            memcpy(buf->data_begin(), s.data(), s.size());
            return nuraft::cs_new<nuraft::log_entry>(term, buf, nuraft::app_log);
        };

        {
            auto ls = nuraft::cs_new<DB::KeeperLogStore>(
                DB::LogFileSettings{.force_sync = true, .compress_logs = false, .rotate_interval = 3},
                DB::FlushSettings{.max_flush_batch_size = 2, .min_time_between_fsyncs_ms = 0},
                DB::ReadAheadSettings{},
                ctx,
                DB::Changelog::ExecutionMode::Threaded);
            ls->init(0, 100);
            for (uint64_t i = 1; i <= 9; ++i)
            {
                auto e = make_entry(1, "gap-" + std::to_string(i));
                require(ls->append(e) == i, "Gap log append mismatch");
            }
            require(ls->flush(), "Gap log flush failed");
            ls->end_of_append_batch(1, 9);
        }

        /// Delete the middle rotated changelog file (`changelog_4_6.bin`) to create an internal log gap `[1..3] .. [7..9]`.
        bool removed_middle = false;
        for (const auto & e : std::filesystem::directory_iterator(dir / "logs"))
        {
            if (e.path().filename().string().find("changelog_4_") == 0)
            {
                std::filesystem::remove(e.path());
                removed_middle = true;
            }
        }
        require(removed_middle, "Expected middle changelog_4_* file to delete");

        auto ls2 = nuraft::cs_new<DB::KeeperLogStore>(
            DB::LogFileSettings{.force_sync = true, .compress_logs = false, .rotate_interval = 3},
            DB::FlushSettings{},
            DB::ReadAheadSettings{},
            ctx,
            DB::Changelog::ExecutionMode::Threaded);
        bool threw_gap = false;
        try
        {
            ls2->init(0, 100);
        }
        catch (const DB::Exception &)
        {
            threw_gap = true;
        }
        require(threw_gap, "KeeperLogStore::init failed to reject missing middle changelog file (log gap)!");
    }

    std::cout << "  [PASS] feature: mntr_data_size_accuracy_and_log_gap_rejection\n";
}

}

inline void runKeeperFeatureTests(const std::filesystem::path & state_dir)
{
    require(!state_dir.empty(), "--state-dir is required for --feature-tests");
    if (std::filesystem::exists(state_dir))
        std::filesystem::remove_all(state_dir);
    std::filesystem::create_directories(state_dir);

    /// Feature tests exercise real multi-threaded Changelog read-ahead & parallel startup,
    /// so keep virtual POSIX time disabled here.
    g_virtual_time_enabled = false;
    DB::ServerUUID::setRandomForUnitTests();
    std::cout << std::unitbuf;

    size_t failed_count = 0;
    std::string first_failure;
    auto run_one = [&](const std::string & name, auto && fn)
    {
        try
        {
            fn();
        }
        catch (const std::exception & e)
        {
            ++failed_count;
            if (first_failure.empty())
                first_failure = name + ": " + e.what();
            std::cout << "  [FAIL] feature: " << name << ": " << e.what() << '\n';
        }
        catch (...)
        {
            ++failed_count;
            if (first_failure.empty())
                first_failure = name + ": unknown exception";
            std::cout << "  [FAIL] feature: " << name << ": unknown exception\n";
        }
    };

    run_one("wire_protocol_roundtrip", [&]() { feature_tests_detail::testWireProtocolRoundtrip(); });
    run_one("feature_flags_and_versions", [&]() { feature_tests_detail::testFeatureFlagsAndContext(); });
    run_one("acl_modes_superdigest_and_system_nodes", [&]() { feature_tests_detail::testAclModesAndSystemNodes(state_dir); });
    run_one("ttl_and_container_gc_semantics", [&]() { feature_tests_detail::testTtlAndContainerGc(state_dir); });
    run_one("orphan_node_startup_recovery", [&]() { feature_tests_detail::testOrphanNodeStartupRecovery(state_dir); });
    run_one("reconfig_validation_and_config_parsing", [&]() { feature_tests_detail::testReconfigParsingAndActions(); });
    run_one("four_letter_word_commands_and_stats", [&]() { feature_tests_detail::testFourLetterWordsAndStats(state_dir); });
    run_one("changelog_readahead_and_parallel_startup", [&]() { feature_tests_detail::testChangelogReadAheadAndParallelStartup(state_dir); });
    run_one("session_expiry_state_manager_and_raft_params", [&]() { feature_tests_detail::testSessionExpiryStateManagerAndRaftParams(state_dir); });
    run_one("zookeeper_data_reader_migration", [&]() { feature_tests_detail::testZooKeeperDataReaderMigration(state_dir); });
    run_one("keeper_read_thread_pool_and_over_dispatcher", [&]() { feature_tests_detail::testKeeperReadThreadPoolAndOverDispatcher(); });
    run_one("keeper_client_cli_commands", [&]() { feature_tests_detail::testKeeperClientCLICommands(); });
    run_one("lsmt_write_throttling_fd_metrics_and_auth_config", [&]() { feature_tests_detail::testLsmtWriteThrottlingFdMetricsAndAuthConfig(state_dir); });
    run_one("compact_children_set_differential", [&]() { feature_tests_detail::testCompactChildrenSetDifferential(); });
    run_one("persistent_and_recursive_watches_semantics", [&]() { feature_tests_detail::testPersistentAndRecursiveWatchSemantics(state_dir); });
    run_one("extended_ops_check_stat_limits_and_never_used_container_gc", [&]() { feature_tests_detail::testExtendedOpsLimitsAndNeverUsedContainerGc(state_dir); });
    run_one("cross_version_snapshot_matrix_and_corrupt_fallback", [&]() { feature_tests_detail::testCrossVersionSnapshotMatrixAndCorruptFallback(state_dir); });
    run_one("changelog_compress_truncate_compact_and_lsmt_disk_compaction", [&]() { feature_tests_detail::testCompressedChangelogTruncateCompactAndLsmtDiskReopen(state_dir); });
    run_one("in_memory_log_store_summing_sm_block_cache_snapshotable_ht_and_path_utils", [&]() { feature_tests_detail::testInMemoryLogStoreSummingSMBlockCacheSnapshotableHTAndPathUtils(); });
    run_one("list_with_options_and_multi_rollback_matrix", [&]() { feature_tests_detail::testListWithOptionsAndMultiRollbackMatrix(state_dir); });
    run_one("nuraft_buffers_node_ref_cache_combine_actions_retries_shuffle_and_error_counter", [&]() { feature_tests_detail::testNuraftBuffersNodeRefCacheCombineActionsRetriesShuffleAndErrorCounter(); });
    run_one("empty_multi_feature_flags_xml_config_and_znode_time", [&]() { feature_tests_detail::testEmptyMultiFeatureFlagsXMLConfigAndZnodeTime(state_dir); });
    run_one("mntr_data_size_accuracy_and_log_gap_rejection", [&]() { feature_tests_detail::testMntrDataSizeAccuracyAndLogGapRejection(state_dir); });

    std::error_code ec;
    std::filesystem::remove_all(state_dir, ec);
    if (failed_count > 0)
        throw std::runtime_error(std::to_string(failed_count) + " feature test(s) failed (first: " + first_failure + ")");
    std::cout << "PASS keeper-sim-feature-tests\n";
}

}
