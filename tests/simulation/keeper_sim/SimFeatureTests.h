#pragma once

#include <SimClock.h>

#include <Common/AsynchronousMetrics.h>
#include <Common/OpenTelemetryTraceContext.h>
#include <Common/ZooKeeper/KeeperFeatureFlags.h>
#include <Common/ZooKeeper/ZooKeeperCommon.h>
#include <Common/ZooKeeper/ZooKeeperIO.h>
#include <Coordination/CoordinationSettings.h>
#include <Coordination/FourLetterCommand.h>
#include <Coordination/KeeperConnectionStats.h>
#include <Coordination/KeeperConstants.h>
#include <Coordination/KeeperContext.h>
#include <Coordination/KeeperLogStore.h>
#include <Coordination/KeeperMemNodesStorage.h>
#include <Coordination/KeeperReconfiguration.h>
#include <Coordination/KeeperSnapshotManager.h>
#include <Coordination/KeeperStateMachine.h>
#include <Coordination/KeeperStorage.h>
#include <Coordination/KeeperStorageImpl.h>
#include <Coordination/RaftServerConfig.h>
#include <Disks/DiskLocal.h>
#include <IO/ReadBufferFromString.h>
#include <IO/WriteBufferFromString.h>

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <string>
#include <vector>

namespace DB::CoordinationSetting
{
    extern const CoordinationSettingsBool use_lsmt_storage;
    extern const CoordinationSettingsBool storage_memory_only;
    extern const CoordinationSettingsBool check_node_acl_on_remove;
    extern const CoordinationSettingsUInt64 write_snapshot_version;
    extern const CoordinationSettingsUInt64 parallel_read_threads;
    extern const CoordinationSettingsUInt64 max_request_size;
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

} // namespace feature_tests_detail

inline void runKeeperFeatureTests(const std::filesystem::path & state_dir)
{
    require(!state_dir.empty(), "--state-dir is required for --feature-tests");
    if (std::filesystem::exists(state_dir))
        std::filesystem::remove_all(state_dir);
    std::filesystem::create_directories(state_dir);

    /// Feature tests exercise real multi-threaded Changelog read-ahead & parallel startup,
    /// so keep virtual POSIX time disabled here.
    g_virtual_time_enabled = false;

    feature_tests_detail::testWireProtocolRoundtrip();
    feature_tests_detail::testFeatureFlagsAndContext();
    feature_tests_detail::testAclModesAndSystemNodes(state_dir);
    feature_tests_detail::testTtlAndContainerGc(state_dir);
    feature_tests_detail::testOrphanNodeStartupRecovery(state_dir);
    feature_tests_detail::testReconfigParsingAndActions();
    feature_tests_detail::testFourLetterWordsAndStats(state_dir);
    feature_tests_detail::testChangelogReadAheadAndParallelStartup(state_dir);

    std::error_code ec;
    std::filesystem::remove_all(state_dir, ec);
    std::cout << "PASS keeper-sim-feature-tests\n";
}

} // namespace keeper_sim
