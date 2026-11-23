#pragma once

#include <SimCrashDisk.h>
#include <SimEngine.h>
#include <SimNetwork.h>
#include <SimOracle.h>

#include <Coordination/KeeperLSMTNodesStorage.h>
#include <Coordination/Storage/BackgroundWork.h>
#include <Coordination/KeeperLogStore.h>
#include <Coordination/KeeperStateMachine.h>
#include <Coordination/KeeperStateManager.h>
#include <Coordination/KeeperStorage.h>
#include <Common/OpenTelemetryTraceContext.h>
#include <Disks/DiskEncrypted.h>
#include <Disks/DiskLocal.h>
#include <IO/FileEncryptionCommon.h>
#include <IO/WriteBuffer.h>
#include <IO/WriteHelpers.h>
#include <libnuraft/client_req_stream.hxx>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace DB::CoordinationSetting
{
    extern const CoordinationSettingsBool use_lsmt_storage;
    extern const CoordinationSettingsBool storage_memory_only;
    extern const CoordinationSettingsBool compress_snapshots_with_zstd_format;
    extern const CoordinationSettingsMilliseconds dead_session_check_period_ms;
    extern const CoordinationSettingsUInt64 parallel_read_threads;
    extern const CoordinationSettingsUInt64 committed_memtable_size;
    extern const CoordinationSettingsUInt64 memtable_block_size;
    extern const CoordinationSettingsUInt64 uncommitted_memtable_size;
    extern const CoordinationSettingsUInt64 min_files_to_merge;
    extern const CoordinationSettingsUInt64 max_files_to_merge;
    extern const CoordinationSettingsUInt64 block_cache_size;
    extern const CoordinationSettingsUInt64 file_block_size;
    extern const CoordinationSettingsUInt64 file_block_group_compressed_size;
    extern const CoordinationSettingsUInt64 sorted_file_uncompressed_size;
    extern const CoordinationSettingsUInt64 write_snapshot_version;
    extern const CoordinationSettingsUInt64 snapshot_transfer_chunk_size;
    extern const CoordinationSettingsUInt64 snapshots_to_keep;
    extern const CoordinationSettingsBool snapshot_transfer_remote_disk_streaming;
    extern const CoordinationSettingsUInt64 min_request_size_for_cache;
    extern const CoordinationSettingsBool use_xid_64;
    extern const CoordinationSettingsBool quorum_reads;
}

namespace keeper_sim
{

enum class KillMode
{
    Graceful,
    PowerLoss,
    WipeAndRejoin,
};

struct ClusterConfig
{
    int num_nodes = 3;
    bool use_lsmt = false;
    bool lsmt_on_disk = false;
    bool mixed_storage = false; // Heterogeneous cluster: Mem + LSMT-Mem + LSMT-Disk (differential fuzzer!)
    bool unsafe_quorum_control = false;
    bool strict_raft_commit = false;
    int nuraft_log_level = 0; // 0 = silent, 1=Fatal..6=Trace
    std::filesystem::path root_dir;

    bool nodeUsesLsmt(int id) const
    {
        if (mixed_storage)
            return (id == 2 || id == 3 || id == 5);
        return use_lsmt;
    }

    bool nodeUsesLsmtDisk(int id) const
    {
        if (mixed_storage)
            return (id == 3 || id == 5);
        return use_lsmt && lsmt_on_disk;
    }
};

class SimNuRaftLogger : public nuraft::logger
{
public:
    SimNuRaftLogger(int node_id_, int level_)
        : node_id(node_id_)
        , level(level_)
    {
    }

    int get_level() override { return level; }
    void set_level(int l) override { level = l; }

    void put_details(
        int lvl,
        const char * source_file,
        const char * func_name,
        size_t line_number,
        const std::string & log_line) override
    {
        if (lvl > level)
            return;
        std::cerr << "[" << simTimeMs() << "ms N" << node_id << " L" << lvl << " "
                  << source_file << ":" << line_number << " " << func_name << "] " << log_line << '\n';
    }

private:
    int node_id;
    int level;
};

/// Subclass of `KeeperStateManager` that turns NuRaft `system_exit` into a clean
/// single-node crash + supervisor restart inside the simulator instead of `std::abort`.
class SimKeeperStateManager : public DB::KeeperStateManager
{
public:
    SimKeeperStateManager(
        int server_id_,
        const std::string & host_,
        int port_,
        DB::KeeperContextPtr keeper_context_,
        nuraft::ptr<DB::KeeperLogStore> log_store_)
        : DB::KeeperStateManager(server_id_, host_, port_, std::move(keeper_context_), std::move(log_store_))
        , sim_node_id(server_id_)
    {
    }

    void system_exit(const int exit_code) override
    {
        throw SimNodeSystemExit(sim_node_id, exit_code);
    }

private:
    int sim_node_id;
};

/// Subclass of `KeeperStateMachine` that observes real `rollback` and `apply_snapshot`
/// transitions directly for continuous Raft & Keeper oracle verification and coverage.
class SimKeeperStateMachine : public DB::KeeperStateMachine
{
public:
    SimKeeperStateMachine(
        int node_id_,
        RaftSafetyOracle & raft_oracle_,
        KeeperSemanticOracle & keeper_oracle_,
        CoverageTracker & coverage_,
        DB::KeeperResponseCallback response_callback_,
        DB::SnapshotsQueue & snapshots_queue_,
        const DB::KeeperContextPtr & keeper_context_,
        DB::KeeperSnapshotManagerS3 * snapshot_manager_s3_,
        CommitCallback commit_callback_ = {},
        const std::string & superdigest_ = "")
        : DB::KeeperStateMachine(
            std::move(response_callback_),
            snapshots_queue_,
            keeper_context_,
            snapshot_manager_s3_,
            std::move(commit_callback_),
            superdigest_)
        , node_id(node_id_)
        , raft_oracle(raft_oracle_)
        , keeper_oracle(keeper_oracle_)
        , coverage(coverage_)
    {
    }

    nuraft::ptr<nuraft::buffer> commit(const uint64_t log_idx, nuraft::buffer & data) override
    {
        auto res = DB::KeeperStateMachine::commit(log_idx, data);
        if (log_idx > 0)
        {
            const uint64_t digest = getStorageUnsafe().getNodesDigest(/*committed=*/true, /*lock_transaction_mutex=*/true).value;
            keeper_oracle.verifyDigest(node_id, log_idx, digest);
        }
        return res;
    }

    void rollback(uint64_t log_idx, nuraft::buffer & data) override
    {
        uint64_t entry_term = 0;
        if (auto * ls = getLogStore(); ls && log_idx >= ls->start_index() && log_idx < ls->next_slot())
            entry_term = ls->term_at(log_idx);
        raft_oracle.onRollback(node_id, log_idx, entry_term, last_commit_index());
        DB::KeeperStateMachine::rollback(log_idx, data);
    }

    bool apply_snapshot(nuraft::snapshot & s) override
    {
        const bool ok = DB::KeeperStateMachine::apply_snapshot(s);
        if (ok)
        {
            coverage.hit("snapshot_applied_on_follower");
            const uint64_t idx = last_commit_index();
            if (idx > 0)
            {
                const uint64_t digest = getStorageUnsafe().getNodesDigest(/*committed=*/true, /*lock_transaction_mutex=*/true).value;
                keeper_oracle.verifyDigest(node_id, idx, digest);
            }
        }
        return ok;
    }

private:
    int node_id;
    RaftSafetyOracle & raft_oracle;
    KeeperSemanticOracle & keeper_oracle;
    CoverageTracker & coverage;
};

/// Manages an N-node (3 or 5) real ClickHouse Keeper cluster inside a single deterministic process.
/// Every node has:
///   - Independent `KeeperContext` (with node-local virtual clock + optional clock skew & manual LSMT)
///   - Independent crash-consistent `SimCrashDisk`s (`LogDisk`, `SnapshotDisk`, `StateDisk`, `DataDisk`)
///   - Independent `KeeperStateMachine`, `KeeperLogStore` (`ExecutionMode::Manual`), `KeeperStateManager`, `SimRaftServer`
///   - Production `PreAppendLogLeader` / `PreAppendLogFollower` / `AppendLogFailed` callbacks (real zxid + digest checking!)
///   - Real automatic snapshots, log compaction, and multi-chunk network snapshot installation!
class SimCluster
{
public:
    struct NodeRuntime
    {
        int id = 0;
        uint64_t incarnation = 0;
        bool online = false;
        bool is_dynamic_joiner = false;
        int64_t clock_skew_ms = 0;

        /// Persistent simulated disks survive across crashes (unless WipeAndRejoin).
        std::shared_ptr<SimCrashDisk> log_disk;
        std::shared_ptr<SimCrashDisk> snapshot_disk;
        std::shared_ptr<SimCrashDisk> latest_snapshot_disk;
        std::shared_ptr<SimCrashDisk> state_disk;
        std::shared_ptr<SimCrashDisk> data_disk;
        DB::DiskPtr local_log_disk;
        DB::DiskPtr local_snapshot_disk;
        std::filesystem::path local_log_dir;
        std::filesystem::path local_snapshot_dir;
        nuraft::ptr<nuraft::client_req_stream> client_stream;

        /// Volatile runtime objects destroyed and recreated on every crash/reboot.
        DB::KeeperContextPtr context;
        nuraft::ptr<DB::SnapshotsQueue> snapshots_queue;
        nuraft::ptr<DB::KeeperStateMachine> machine;
        nuraft::ptr<DB::KeeperLogStore> log_store;
        nuraft::ptr<DB::KeeperStateManager> state_manager;
        nuraft::ptr<SimTimers> timers;
        nuraft::ptr<SimRaftServer> server;

        /// Pending response capture for the commit currently executing on this node.
        std::optional<DB::KeeperResponseForSession> last_commit_response;
        std::vector<std::tuple<int64_t, std::string, int32_t>> recent_watches;
        uint64_t last_commit_log_idx = 0;
        uint32_t next_commit_sub_idx = 0;

        /// Track queued background task deduplication per node.
        std::set<DB::Changelog::BackgroundTask> queued_log_tasks;
        bool snapshot_task_queued = false;
        bool lsmt_flush_queued = false;
        bool lsmt_merge_queued = false;
        bool lsmt_delete_queued = false;
    };

    SimCluster(SimEngine & engine_, const ClusterConfig & cfg_)
        : engine(engine_)
        , cfg(cfg_)
        , network(engine_)
        , rpc_factory(nuraft::cs_new<SimRpcFactory>(network, engine_.buggify.client_stream && engine_.rng.chance(0.50)))
        , raft_oracle(cfg_.num_nodes, cfg_.strict_raft_commit, engine_.coverage)
        , keeper_oracle(engine_.coverage)
    {
        require(cfg.num_nodes == 3 || cfg.num_nodes == 5, "SimCluster supports 3 or 5 nodes");
        std::filesystem::create_directories(cfg.root_dir);

        for (int id = 1; id <= cfg.num_nodes; ++id)
            initial_raft_config.get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(id, std::to_string(id)));
        if (engine.buggify.async_replication)
        {
            initial_raft_config.set_async_replication(true);
            engine.coverage.hit("async_replication_enabled");
        }

        buildRaftParams();

        network.on_rpc_response = [this](const SimNetwork::RpcObservation & obs)
        {
            raft_oracle.onRpcResponse(obs);
        };

        /// Poll manual Changelog, Snapshot, and LSMT background queues before every simulation step.
        engine.on_before_step = [this]()
        {
            pollBackgroundWork();
        };

        /// Run continuous inline Raft safety checks after every step.
        engine.on_after_step = [this](const std::string &, int)
        {
            checkRaftSafetyAllNodes();
        };

        /// Surface fatal NuRaft `system_exit` calls immediately (except expected `N22_unrecoverable_isolation`
        /// when `leadership_expiry_ms > 0` is explicitly enabled, or `N12`/`N13` when transient disk faults are active).
        engine.on_node_system_exit = [this](int node_id, int exit_code)
        {
            engine.coverage.hit("node_system_exit_" + std::to_string(exit_code));
            engine.trace("NODE_SYSTEM_EXIT n=" + std::to_string(node_id) + " code=" + std::to_string(exit_code));
            const bool expected_isolation = (exit_code == nuraft::raft_err::N22_unrecoverable_isolation && engine.buggify.leadership_expiry_ms > 0);
            const bool expected_disk_snap_err = ((exit_code == nuraft::raft_err::N12_apply_snapshot_failed || exit_code == nuraft::raft_err::N13_snapshot_install_failed)
                && engine.buggify.isActive("transient_disk_io_errors"));
            if (!expected_isolation && !expected_disk_snap_err)
            {
                require(false,
                    "FATAL_NURAFT_SYSTEM_EXIT on node " + std::to_string(node_id)
                    + " exit_code=" + std::to_string(exit_code));
            }
            engine.coverage.hit("node_system_exit_recovered");
            if (isOnline(node_id))
                crashNode(node_id, KillMode::PowerLoss);
            const int64_t restart_delay = engine.rng.rangeInt(300, 900);
            engine.scheduleAfterMs(
                restart_delay,
                "node-supervisor-restart",
                [this, node_id]()
                {
                    if (!isOnline(node_id))
                        startNode(node_id);
                });
        };

        for (int id = 1; id <= cfg.num_nodes; ++id)
        {
            auto & node = nodes[id];
            node.id = id;
            if (engine.buggify.isActive("clock_skew"))
                node.clock_skew_ms = engine.rng.rangeInt(-45, 45);

            const auto node_dir = cfg.root_dir / ("node_" + std::to_string(id));
            if (engine.buggify.local_disk_cache)
            {
                node.local_log_dir = node_dir / "local_logs";
                node.local_snapshot_dir = node_dir / "local_snapshots";
                std::filesystem::create_directories(node.local_log_dir);
                std::filesystem::create_directories(node.local_snapshot_dir);
                node.local_log_disk = std::make_shared<DB::DiskLocal>("LocalLogDisk_" + std::to_string(id), node.local_log_dir.string() + "/");
                node.local_snapshot_disk = std::make_shared<DB::DiskLocal>("LocalSnapDisk_" + std::to_string(id), node.local_snapshot_dir.string() + "/");
            }
            node.log_disk = std::make_shared<SimCrashDisk>("LogDisk_" + std::to_string(id),
                node_dir / "live/logs", node_dir / "durable/logs", &engine.coverage);
            node.snapshot_disk = std::make_shared<SimCrashDisk>("SnapshotDisk_" + std::to_string(id),
                node_dir / "live/snapshots", node_dir / "durable/snapshots", &engine.coverage);
            if (engine.buggify.dual_snapshot_disks)
            {
                node.latest_snapshot_disk = std::make_shared<SimCrashDisk>("LatestSnapDisk_" + std::to_string(id),
                    node_dir / "live/latest_snapshots", node_dir / "durable/latest_snapshots", &engine.coverage);
            }
            node.state_disk = std::make_shared<SimCrashDisk>("StateDisk_" + std::to_string(id),
                node_dir / "live/state", node_dir / "durable/state", &engine.coverage);
            if (cfg.nodeUsesLsmtDisk(id))
            {
                node.data_disk = std::make_shared<SimCrashDisk>("DataDisk_" + std::to_string(id),
                    node_dir / "live/data", node_dir / "durable/data", &engine.coverage);
            }
        }

        network.healAll();
    }

    ~SimCluster()
    {
        try
        {
            shutdownAll();
        }
        catch (...)
        {
        }
    }

    void startAll()
    {
        for (int id = 1; id <= cfg.num_nodes; ++id)
            startNode(id);
        scheduleDeadSessionSweep();
    }

    void startNode(int id)
    {
        auto & node = nodes.at(id);
        require(!node.online, "Node " + std::to_string(id) + " is already online");
        ++node.incarnation;
        const uint64_t inc = node.incarnation;

        /// Reset LSMT ephemeral data disk on reboot (matching KeeperContext::initializeDataDisk).
        if (node.data_disk)
            node.data_disk->simulateDiskWipe();

        const bool node_lsmt = cfg.nodeUsesLsmt(id);
        const bool node_lsmt_disk = cfg.nodeUsesLsmtDisk(id);

        auto settings = std::make_shared<DB::CoordinationSettings>();
        (*settings)[DB::CoordinationSetting::use_lsmt_storage] = node_lsmt;
        (*settings)[DB::CoordinationSetting::storage_memory_only] = !node_lsmt_disk;
        (*settings)[DB::CoordinationSetting::compress_snapshots_with_zstd_format] = engine.buggify.compress_snapshots_zstd;
        const uint64_t node_snap_ver = engine.buggify.mixed_versions ? ((id % 2 == 1) ? 8ULL : 9ULL) : engine.buggify.write_snapshot_version;
        (*settings)[DB::CoordinationSetting::write_snapshot_version] = node_snap_ver;
        (*settings)[DB::CoordinationSetting::snapshot_transfer_chunk_size] = engine.buggify.snapshot_transfer_chunk_size;
        (*settings)[DB::CoordinationSetting::snapshots_to_keep] = engine.buggify.snapshots_to_keep;
        (*settings)[DB::CoordinationSetting::snapshot_transfer_remote_disk_streaming] = engine.buggify.remote_snapshot_streaming;
        (*settings)[DB::CoordinationSetting::min_request_size_for_cache] = engine.buggify.min_request_size_for_cache;
        (*settings)[DB::CoordinationSetting::use_xid_64] = engine.buggify.quorum_reads;
        (*settings)[DB::CoordinationSetting::quorum_reads] = engine.buggify.quorum_reads;
        (*settings)[DB::CoordinationSetting::dead_session_check_period_ms] = 500;
        (*settings)[DB::CoordinationSetting::parallel_read_threads] = 0;
        if (engine.buggify.latest_logs_cache_entry_count_threshold > 0)
            engine.coverage.hit("bounded_log_cache_active");
        if (engine.buggify.latest_logs_cache_size_threshold > 0)
            engine.coverage.hit("byte_bounded_log_cache_active");
        if (engine.buggify.log_max_size > 0)
            engine.coverage.hit("log_max_size_active");
        if (engine.buggify.min_request_size_for_cache <= 64)
            engine.coverage.hit("parsed_batch_cache_active");
        if (engine.buggify.snapshot_transfer_chunk_size > 0)
            engine.coverage.hit("chunked_snapshot_transfer_active");
        if (node_lsmt)
        {
            (*settings)[DB::CoordinationSetting::committed_memtable_size] = engine.buggify.lsmt_memtable_size;
            (*settings)[DB::CoordinationSetting::memtable_block_size] = engine.buggify.lsmt_block_size;
            (*settings)[DB::CoordinationSetting::uncommitted_memtable_size] = engine.buggify.lsmt_memtable_size;
            (*settings)[DB::CoordinationSetting::min_files_to_merge] = engine.buggify.lsmt_min_files_to_merge;
            (*settings)[DB::CoordinationSetting::max_files_to_merge] = 4;
            (*settings)[DB::CoordinationSetting::file_block_size] = engine.buggify.lsmt_file_block_size;
            (*settings)[DB::CoordinationSetting::file_block_group_compressed_size] = engine.buggify.lsmt_block_group_compressed_size;
            (*settings)[DB::CoordinationSetting::sorted_file_uncompressed_size] = engine.buggify.lsmt_sorted_file_uncompressed_size;
            if (node_lsmt_disk)
                (*settings)[DB::CoordinationSetting::block_cache_size] = engine.buggify.lsmt_block_cache_size;
        }

        const int64_t skew = node.clock_skew_ms;
        node.context = std::make_shared<DB::KeeperContext>(
            /*standalone_keeper=*/true,
            settings,
            [skew]() -> int64_t
            {
                return std::max<int64_t>(0, simTimeMs() + skew);
            });
        auto wrap_encrypted = [](const std::string & name, DB::DiskPtr inner) -> DB::DiskPtr
        {
            auto s = std::make_unique<DB::DiskEncryptedSettings>();
            s->wrapped_disk = std::move(inner);
            s->disk_path = "";
            s->current_key = DB::SecretString("0123456789abcdef");
            s->current_algorithm = DB::FileEncryption::Algorithm::AES_128_CTR;
            s->current_key_fingerprint = DB::UInt128{0x123456789abcdef0ULL, 0xfedcba9876543210ULL};
            s->all_keys[s->current_key_fingerprint] = s->current_key;
            return std::make_shared<DB::DiskEncrypted>(name, std::move(s));
        };

        node.context->setManualLSMTBackgroundWork(true);
        node.context->setDigestEnabled(true);
        if (engine.buggify.local_disk_cache && node.local_snapshot_disk)
        {
            node.context->setSnapshotDisk(node.local_snapshot_disk);
            engine.coverage.hit("local_disk_streaming_snapshot_active");
        }
        else if (engine.buggify.remote_snapshots)
        {
            node.context->setSnapshotDisk(wrap_encrypted("EncSnapDisk_" + std::to_string(id), node.snapshot_disk));
            engine.coverage.hit("remote_snapshot_disk_active");
            if (node.latest_snapshot_disk)
            {
                node.context->setLatestSnapshotDisk(node.latest_snapshot_disk);
                engine.coverage.hit("dual_snapshot_disk_migration_active");
            }
            if (engine.buggify.remote_snapshot_streaming)
                engine.coverage.hit("remote_snapshot_streaming_active");
        }
        else
        {
            node.context->setSnapshotDisk(node.snapshot_disk);
        }
        if (engine.buggify.local_disk_cache && node.local_log_disk)
        {
            node.context->setLogDisk(node.local_log_disk);
            engine.coverage.hit("local_disk_log_active");
        }
        else if (engine.buggify.encrypted_logs)
        {
            node.context->setLogDisk(wrap_encrypted("EncLogDisk_" + std::to_string(id), node.log_disk));
            engine.coverage.hit("encrypted_log_disk_active");
        }
        else
        {
            node.context->setLogDisk(node.log_disk);
        }
        node.context->setStateFileDisk(node.state_disk);
        if (node.data_disk)
            node.context->setDataDisk(node.data_disk);

        node.snapshots_queue = nuraft::cs_new<DB::SnapshotsQueue>(8);
        node.last_commit_response.reset();
        node.last_commit_log_idx = 0;
        node.next_commit_sub_idx = 0;

        node.machine = nuraft::cs_new<SimKeeperStateMachine>(
            id,
            raft_oracle,
            keeper_oracle,
            engine.coverage,
            [this, id](DB::KeeperResponseForSession resp)
            {
                if (!resp.response)
                    return;
                if (resp.response->xid == Coordination::WATCH_XID)
                {
                    std::string watch_path;
                    int32_t watch_type = 0;
                    if (auto wr = std::dynamic_pointer_cast<Coordination::ZooKeeperWatchResponse>(resp.response))
                    {
                        watch_path = wr->path;
                        watch_type = wr->type;
                    }
                    auto it_w = nodes.find(id);
                    if (it_w != nodes.end() && it_w->second.online)
                        it_w->second.recent_watches.emplace_back(resp.session_id, watch_path, watch_type);
                    keeper_oracle.recordWatchDelivered(id, resp.session_id, watch_path);
                    return;
                }
                auto it = nodes.find(id);
                if (it != nodes.end() && it->second.online)
                    it->second.last_commit_response = std::move(resp);
            },
            *node.snapshots_queue,
            node.context,
            /*snapshot_manager_s3=*/nullptr,
            [this, id](uint64_t log_idx, const DB::KeeperRequestForSession & req, int32_t /*dispatcher_server_id*/)
            {
                auto it = nodes.find(id);
                if (it == nodes.end() || !it->second.online)
                    return;
                auto & n = it->second;
                Coordination::ZooKeeperResponsePtr resp_ptr;
                if (n.last_commit_response)
                {
                    resp_ptr = n.last_commit_response->response;
                    n.last_commit_response.reset();
                }

                if (n.last_commit_log_idx != log_idx)
                {
                    n.last_commit_log_idx = log_idx;
                    n.next_commit_sub_idx = 0;
                }
                const uint32_t sub_idx = n.next_commit_sub_idx++;

                /// Verify Raft State Machine Safety (entry bytes at log_idx).
                if (sub_idx == 0 && n.log_store && log_idx >= n.log_store->start_index() && log_idx < n.log_store->next_slot())
                {
                    if (auto entry = n.log_store->entry_at(log_idx))
                    {
                        auto buf = entry->serialize();
                        std::string bytes(reinterpret_cast<const char *>(buf->data_begin()), buf->size());
                        raft_oracle.verifyCommittedEntry(id, log_idx, entry->get_term(), bytes);
                    }
                }

                const uint64_t digest = n.machine->getStorageUnsafe().getNodesDigest(/*committed=*/true, /*lock_transaction_mutex=*/true).value;
                keeper_oracle.onKeeperCommit(id, log_idx, sub_idx, /*is_last_in_batch=*/false, req, resp_ptr, digest);
            });

        node.machine->init();
        const uint64_t snapshot_idx = node.machine->last_commit_index();
        node.context->setLastCommitIndex(snapshot_idx);
        if (snapshot_idx > 0)
            keeper_oracle.verifyDigest(id, snapshot_idx, node.machine->getStorageUnsafe().getNodesDigest(/*committed=*/true, /*lock_transaction_mutex=*/true).value);

        node.log_store = nuraft::cs_new<DB::KeeperLogStore>(
            DB::LogFileSettings{
                .force_sync = engine.buggify.force_sync_logs,
                .compress_logs = engine.buggify.compress_logs,
                .rotate_interval = engine.buggify.log_rotate_interval,
                .max_size = engine.buggify.log_max_size,
                .overallocate_size = engine.buggify.log_overallocate_size,
                .latest_logs_cache_size_threshold = engine.buggify.latest_logs_cache_size_threshold,
                .latest_logs_cache_entry_count_threshold = engine.buggify.latest_logs_cache_entry_count_threshold,
                .startup_read_max_streams = 1,
            },
            DB::FlushSettings{.max_flush_batch_size = engine.buggify.max_flush_batch_size, .min_time_between_fsyncs_ms = 0},
            DB::ReadAheadSettings{},
            node.context,
            DB::Changelog::ExecutionMode::Manual);

        node.state_manager = nuraft::cs_new<SimKeeperStateManager>(id, std::to_string(id), 0, node.context, node.log_store);
        node.state_manager->loadLogStore(snapshot_idx, std::max<uint64_t>(200, snapshot_idx));
        node.machine->setLogStore(node.log_store.get());
        require(node.log_store->flush(), "SimCluster: startup log flush failed on node " + std::to_string(id));

        /// Preprocess any persisted uncommitted log entries above the snapshot (exact KeeperServer::startup semantics).
        if (node.log_store->next_slot() > snapshot_idx + 1)
            node.machine->preprocessUncommittedLogEntries(snapshot_idx + 1, node.log_store->next_slot(), /*lock_mutex=*/true);
        node.context->setLocalLogsPreprocessed();

        /// Recover latest cluster configuration from snapshot or log (matching KeeperServer::loadLatestConfig).
        auto snap_cfg = node.machine->getClusterConfig();
        auto log_cfg = node.state_manager->getLatestConfigFromLogStore();
        if (snap_cfg && (!log_cfg || snap_cfg->get_log_idx() > log_cfg->get_log_idx()))
            node.state_manager->save_config(*snap_cfg);
        else if (log_cfg)
            node.state_manager->save_config(*log_cfg);
        else if (node.is_dynamic_joiner)
        {
            nuraft::cluster_config joiner_cfg;
            joiner_cfg.get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(id, 0, std::to_string(id), "", /*learner=*/true, /*priority=*/0));
            node.state_manager->save_config(joiner_cfg);
        }
        else
            node.state_manager->save_config(initial_raft_config);

        node.timers = nuraft::cs_new<SimTimers>(id, inc, engine);

        /// Wire production PreAppendLogLeader / PreAppendLogFollower / AppendLogFailed / CommitConfig callbacks!
        nuraft::cb_func::func_type cb = [this, id](nuraft::cb_func::Type type, nuraft::cb_func::Param * param) -> nuraft::cb_func::ReturnCode
        {
            return handleRaftCallback(id, type, param);
        };

        nuraft::ptr<nuraft::logger> nr_logger;
        if (cfg.nuraft_log_level > 0)
            nr_logger = nuraft::cs_new<SimNuRaftLogger>(id, cfg.nuraft_log_level);

        auto * ctx = new nuraft::context(
            node.state_manager,
            node.machine,
            {},
            nr_logger,
            rpc_factory,
            node.timers,
            raft_params_cfg,
            &network);
        ctx->set_cb_func(cb);

        node.server = nuraft::cs_new<SimRaftServer>(ctx);
        node.online = true;
        network.registerServer(id, node.server, inc);
        node.log_store->setRaftServer(node.server);
        node.server->start_server(false);

        require(g_thread_attempts.load() == 0 && liveThreadCount() == 1,
            "SimCluster::startNode spawned uncontrolled OS thread on node " + std::to_string(id));
        engine.trace("NODE_START n=" + std::to_string(id) + " inc=" + std::to_string(inc)
            + " snap_idx=" + std::to_string(snapshot_idx)
            + " log_end=" + std::to_string(node.log_store->next_slot() - 1));
    }

    void crashNode(int id, KillMode mode)
    {
        auto & node = nodes.at(id);
        if (!node.online)
            return;

        engine.trace("NODE_CRASH n=" + std::to_string(id) + " mode=" + std::to_string(static_cast<int>(mode)));

        std::weak_ptr<SimRaftServer> weak_server = node.server;
        std::weak_ptr<DB::KeeperStateMachine> weak_machine = node.machine;
        std::weak_ptr<DB::KeeperLogStore> weak_log = node.log_store;
        std::weak_ptr<DB::KeeperStateManager> weak_mgr = node.state_manager;
        std::weak_ptr<DB::KeeperContext> weak_ctx = node.context;

        node.online = false;
        network.unregisterServer(id);

        if (node.timers)
            node.timers->cancelAll();
        if (node.server)
            node.server->shutdown();
        if (mode == KillMode::Graceful && engine.buggify.snapshot_on_exit && node.server && node.context)
        {
            node.context->setServerState(DB::KeeperContext::Phase::SHUTDOWN);
            nuraft::raft_server::create_snapshot_options opts;
            opts.serialize_commit_ = true;
            const uint64_t exit_snp_idx = node.server->create_snapshot(opts);
            if (exit_snp_idx > 0)
            {
                engine.coverage.hit("snapshot_on_exit_created");
                engine.trace("SNAPSHOT_ON_EXIT n=" + std::to_string(id) + " idx=" + std::to_string(exit_snp_idx));
            }
        }
        if (node.log_store)
            node.log_store->setRaftServer(nullptr);
        if (node.machine)
            node.machine->setLogStore(nullptr);
        engine.purgeNodeEvents(id);

        if (mode == KillMode::PowerLoss && node.log_store)
        {
            const size_t dropped = node.log_store->discardUnexecutedWritesForTesting();
            if (dropped > 0)
                engine.coverage.hit("power_loss_discarded_dirty_log");
        }

        node.queued_log_tasks.clear();
        node.snapshot_task_queued = false;
        node.lsmt_flush_queued = false;
        node.lsmt_merge_queued = false;
        node.lsmt_delete_queued = false;
        node.last_commit_response.reset();
        node.last_commit_log_idx = 0;
        node.next_commit_sub_idx = 0;

        node.client_stream.reset();
        node.server.reset();
        node.machine.reset();
        node.state_manager.reset();
        node.log_store.reset();
        node.snapshots_queue.reset();
        node.timers.reset();
        node.context.reset();

        /// Verify zero leaked ownership of crashed node runtime (FDB strict reboot isolation).
        require(weak_server.expired() && weak_machine.expired() && weak_log.expired()
            && weak_mgr.expired() && weak_ctx.expired(),
            "SimCluster::crashNode retained stale runtime object for node " + std::to_string(id)
            + " (server=" + std::to_string(weak_server.use_count())
            + " machine=" + std::to_string(weak_machine.use_count())
            + " log=" + std::to_string(weak_log.use_count())
            + " mgr=" + std::to_string(weak_mgr.use_count())
            + " ctx=" + std::to_string(weak_ctx.use_count()) + ")");

        if (mode == KillMode::Graceful)
        {
            node.log_disk->simulateGracefulStop();
            node.snapshot_disk->simulateGracefulStop();
            if (node.latest_snapshot_disk)
                node.latest_snapshot_disk->simulateGracefulStop();
            node.state_disk->simulateGracefulStop();
            engine.coverage.hit("graceful_restart");
        }
        else if (mode == KillMode::PowerLoss)
        {
            const bool allow_torn = engine.buggify.isActive("torn_writes_on_power_loss");
            node.log_disk->simulatePowerLoss(engine.rng, allow_torn);
            node.snapshot_disk->simulatePowerLoss(engine.rng, allow_torn);
            if (node.latest_snapshot_disk)
                node.latest_snapshot_disk->simulatePowerLoss(engine.rng, allow_torn);
            node.state_disk->simulatePowerLoss(engine.rng, allow_torn);
            engine.coverage.hit("power_loss_crash");
        }
        else if (mode == KillMode::WipeAndRejoin)
        {
            /// Wipe logs + snapshots while preserving `state_disk` (persisted term & voted_for)
            /// so snapshot-install recovery is tested without violating Raft's persisted-vote safety invariant.
            if (node.local_log_disk)
            {
                std::filesystem::remove_all(node.local_log_dir);
                std::filesystem::create_directories(node.local_log_dir);
                node.local_log_disk = std::make_shared<DB::DiskLocal>("LocalLogDisk_" + std::to_string(id), node.local_log_dir.string() + "/");
            }
            if (node.local_snapshot_disk)
            {
                std::filesystem::remove_all(node.local_snapshot_dir);
                std::filesystem::create_directories(node.local_snapshot_dir);
                node.local_snapshot_disk = std::make_shared<DB::DiskLocal>("LocalSnapDisk_" + std::to_string(id), node.local_snapshot_dir.string() + "/");
            }
            node.log_disk->simulateDiskWipe();
            node.snapshot_disk->simulateDiskWipe();
            if (node.latest_snapshot_disk)
                node.latest_snapshot_disk->simulateDiskWipe();
            node.state_disk->simulateGracefulStop();
            raft_oracle.onNodeDiskWiped(id, /*state_wiped=*/false);
            engine.coverage.hit("snapshot_wipe_recovery");
        }
    }

    int findLeader() const
    {
        for (const auto & [id, node] : nodes)
        {
            if (node.online && node.server && node.server->is_leader())
                return id;
        }
        return 0;
    }

    std::vector<int> onlineNodes() const
    {
        std::vector<int> out;
        for (const auto & [id, node] : nodes)
        {
            if (node.online)
                out.push_back(id);
        }
        return out;
    }

    bool isOnline(int id) const
    {
        auto it = nodes.find(id);
        return it != nodes.end() && it->second.online;
    }

    nuraft::ptr<nuraft::cmd_result<nuraft::ptr<nuraft::buffer>>> submitRequest(
        int leader_id,
        Coordination::ZooKeeperRequestPtr request,
        int64_t session_id)
    {
        DB::KeeperRequestForSession entry;
        entry.session_id = session_id;
        entry.time = simTimeMs();
        if (engine.buggify.quorum_reads && request)
        {
            if (request->xid >= (1LL << 32) || engine.rng.chance(0.50))
            {
                entry.use_xid_64 = true;
                engine.coverage.hit("xid_64_submitted");
            }
            if (engine.rng.chance(0.30))
            {
                auto tc = std::make_shared<DB::OpenTelemetry::TracingContext>();
                tc->trace_id = DB::UUID{DB::UInt128{engine.rng.nextU64(), engine.rng.nextU64()}};
                tc->span_id = engine.rng.nextU64();
                tc->tracestate = "sim=1";
                tc->trace_flags = 1;
                request->tracing_context = std::move(tc);
                engine.coverage.hit("tracing_context_submitted");
            }
        }
        entry.request = std::move(request);
        const uint64_t log_ver = engine.buggify.mixed_versions ? ((leader_id % 2 == 1) ? 0ULL : 1ULL) : engine.buggify.write_log_entry_version;
        return submitRequestBatch(leader_id, {std::move(entry)}, log_ver);
    }

    nuraft::ptr<nuraft::cmd_result<nuraft::ptr<nuraft::buffer>>> submitRequestBatch(
        int leader_id,
        std::vector<DB::KeeperRequestForSession> entries,
        uint64_t log_entry_version = 1)
    {
        auto & node = nodes.at(leader_id);
        if (!node.online || !node.server || (!engine.buggify.auto_forward && !node.server->is_leader()) || entries.empty())
            return nullptr;

        if (!node.server->is_leader())
            engine.coverage.hit("follower_auto_forwarded_write");

        DB::KeeperRequestBatch batch;
        batch.dispatcher_server_id = -1;
        batch.requests = std::move(entries);
        const size_t nreq = batch.requests.size();
        auto bufs = DB::KeeperStateMachine::serializeRequestBatch(batch, log_entry_version);
        if (nreq > 1 && log_entry_version >= 1)
            engine.coverage.hit("multi_request_batch_submitted");
        return node.server->append_entries(bufs);
    }

    /// Submit a write through NuRaft's `client_req_stream` (`open_client_req_stream`) on ANY node!
    bool submitViaClientStream(
        int node_id,
        Coordination::ZooKeeperRequestPtr request,
        int64_t session_id)
    {
        auto it = nodes.find(node_id);
        if (it == nodes.end() || !it->second.online || !it->second.server)
            return false;
        auto & node = it->second;
        if (!node.client_stream || node.client_stream->is_abandoned())
        {
            node.client_stream = node.server->open_client_req_stream(800);
            if (!node.client_stream)
                return false;
            engine.coverage.hit("client_req_stream_opened");
        }
        if (!node.client_stream->is_ready())
            return false;
        DB::KeeperRequestForSession entry;
        entry.session_id = session_id;
        entry.time = simTimeMs();
        entry.request = std::move(request);
        DB::KeeperRequestBatch batch;
        batch.dispatcher_server_id = -1;
        batch.requests = {std::move(entry)};
        auto bufs = DB::KeeperStateMachine::serializeRequestBatch(batch, engine.buggify.write_log_entry_version);
        node.client_stream->append(std::move(bufs));
        engine.coverage.hit("client_req_stream_appended");
        return true;
    }

    /// Hot-reload NuRaft params, directed leadership transfer (`request_leadership`), and `set_user_ctx`!
    void runHotReconfigAction()
    {
        const int leader = findLeader();
        if (leader == 0)
            return;
        auto & lnode = nodes.at(leader);
        if (!lnode.online || !lnode.server)
            return;
        const int64_t op = engine.rng.rangeInt(0, 2);
        if (op == 0)
        {
            auto online = onlineNodes();
            std::vector<int> successors;
            for (int oid : online)
                if (oid != leader)
                    successors.push_back(oid);
            if (!successors.empty())
            {
                const int succ = engine.rng.pick(successors);
                if (lnode.server->request_leadership(succ))
                {
                    engine.coverage.hit("request_leadership_succeeded");
                    engine.trace("REQUEST_LEADERSHIP from=" + std::to_string(leader) + " to=" + std::to_string(succ));
                }
            }
        }
        else if (op == 1)
        {
            lnode.server->set_user_ctx("sim-ctx-" + std::to_string(engine.rng.nextU32()));
            engine.coverage.hit("set_user_ctx_submitted");
        }
        else
        {
            nuraft::raft_params updated = lnode.server->get_current_params();
            updated.max_append_size_ = static_cast<int32_t>(engine.rng.rangeInt(1, 12));
            updated.snapshot_distance_ = static_cast<int32_t>(engine.rng.rangeInt(8, 30));
            updated.max_uncommitted_log_entries_ = engine.rng.chance(0.5) ? static_cast<uint64_t>(engine.rng.rangeInt(4, 16)) : 0ULL;
            lnode.server->update_params(updated);
            engine.coverage.hit("hot_update_params");
        }
    }

    void yieldLeadership(int leader_id)
    {
        auto it = nodes.find(leader_id);
        if (it == nodes.end() || !it->second.online || !it->second.server || !it->second.server->is_leader())
            return;
        it->second.server->yield_leadership(/*immediate_yield=*/true);
        engine.coverage.hit("leadership_yielded");
        engine.trace("LEADER_YIELD n=" + std::to_string(leader_id));
    }

    /// Toggle NuRaft slow-member backpressure (`bpon`/`bpoff`) via any online server (leader or follower!).
    void toggleSlowMemberBackpressure(int node_id, bool enable)
    {
        auto it = nodes.find(node_id);
        if (it == nodes.end() || !it->second.online || !it->second.server)
            return;
        if (it->second.server->request_slow_member_backpressure(enable))
        {
            engine.coverage.hit(enable ? "slow_member_backpressure_on" : "slow_member_backpressure_off");
            engine.trace("BACKPRESSURE_TOGGLE n=" + std::to_string(node_id) + " enable=" + std::to_string(enable ? 1 : 0));
        }
    }

    /// Submit a live Raft configuration update changing a peer's election priority!
    void setNodePriority(int leader_id, int target_id, int priority)
    {
        auto it = nodes.find(leader_id);
        if (it == nodes.end() || !it->second.online || !it->second.server)
            return;
        auto status = it->second.server->set_priority(target_id, priority);
        if (status == nuraft::raft_server::PrioritySetResult::SET)
        {
            engine.coverage.hit("reconfig_priority_submitted");
            engine.trace("RECONFIG_PRIORITY leader=" + std::to_string(leader_id)
                + " target=" + std::to_string(target_id) + " prio=" + std::to_string(priority));
        }
    }

    /// Wave 12 NuRaft priority & config torture: exercises `set_priority_v2` (leader & forwarded),
    /// `set_priority(0)` stepdown & `broadcast_when_leader_exists`, `flip_learner_flag`, and `update_srv_config`!
    void runPriorityTortureAction()
    {
        const int leader = findLeader();
        if (leader == 0)
            return;
        auto & lnode = nodes.at(leader);
        if (!lnode.online || !lnode.server)
            return;
        auto cur_cfg = lnode.server->get_config();
        if (!cur_cfg)
            return;

        auto online = onlineNodes();
        if (online.empty())
            return;

        std::vector<int> voters;
        std::vector<int> learners;
        int voters_with_pos_prio = 0;
        for (const auto & s : cur_cfg->get_servers())
        {
            if (!s)
                continue;
            if (s->is_learner())
                learners.push_back(s->get_id());
            else
            {
                voters.push_back(s->get_id());
                if (s->get_priority() > 0)
                    ++voters_with_pos_prio;
            }
        }

        const int64_t op = engine.rng.rangeInt(0, 3);
        if (op == 0)
        {
            /// `set_priority_v2` from leader (or forwarded from a follower when `auto_forward` is enabled)!
            const int caller = (engine.buggify.auto_forward && engine.rng.chance(0.55)) ? engine.rng.pick(online) : leader;
            auto cit = nodes.find(caller);
            if (cit == nodes.end() || !cit->second.online || !cit->second.server)
                return;
            const int target = static_cast<int>(engine.rng.rangeInt(1, cfg.num_nodes));
            int new_prio = static_cast<int>(engine.rng.rangeInt(1, 20));
            if (voters_with_pos_prio >= 3 && engine.rng.chance(0.25))
                new_prio = 0;
            if (cit->second.server->set_priority_v2(target, new_prio))
            {
                engine.coverage.hit(caller == leader ? "set_priority_v2_leader" : "set_priority_v2_forwarded");
                engine.trace("SET_PRIO_V2 caller=" + std::to_string(caller)
                    + " target=" + std::to_string(target) + " prio=" + std::to_string(new_prio));
            }
        }
        else if (op == 1)
        {
            /// `set_priority` with `broadcast_when_leader_exists=true` or leader self-stepdown (`priority=0`)!
            const int caller = engine.rng.chance(0.50) ? engine.rng.pick(online) : leader;
            auto cit = nodes.find(caller);
            if (cit == nodes.end() || !cit->second.online || !cit->second.server)
                return;
            const int target = (voters_with_pos_prio >= 3 && engine.rng.chance(0.40))
                ? leader
                : static_cast<int>(engine.rng.rangeInt(1, cfg.num_nodes));
            const int new_prio = (target == leader && voters_with_pos_prio >= 3)
                ? 0
                : static_cast<int>(engine.rng.rangeInt(1, 20));
            const bool broadcast = engine.rng.chance(0.50);
            auto st = cit->second.server->set_priority(target, new_prio, broadcast);
            if (st == nuraft::raft_server::PrioritySetResult::BROADCAST)
                engine.coverage.hit("set_priority_broadcast");
            else if (st == nuraft::raft_server::PrioritySetResult::SET)
                engine.coverage.hit("set_priority_set");
        }
        else if (op == 2)
        {
            /// `flip_learner_flag`: promote any learner back to voter, or demote a non-leader voter if >= 4 voters!
            if (!learners.empty() && engine.rng.chance(0.65))
            {
                const int promote_id = engine.rng.pick(learners);
                auto res = lnode.server->flip_learner_flag(promote_id, /*to=*/false);
                if (res && res->get_result_code() == nuraft::cmd_result_code::OK)
                {
                    engine.coverage.hit("flip_learner_to_voter");
                    engine.trace("FLIP_LEARNER_PROMOTE target=" + std::to_string(promote_id));
                    if (engine.buggify.reconfig_overlap)
                    {
                        /// Immediately issue `set_user_ctx` or `update_srv_config` while `flip_learner_flag` is uncommitted!
                        lnode.server->set_user_ctx("overlap-" + std::to_string(engine.rng.nextU32()));
                        engine.coverage.hit("reconfig_overlap_set_user_ctx_after_flip");
                    }
                }
            }
            else if (voters.size() >= 4)
            {
                std::vector<int> demotable;
                for (int v : voters)
                    if (v != leader)
                        demotable.push_back(v);
                if (!demotable.empty())
                {
                    const int demote_id = engine.rng.pick(demotable);
                    auto res = lnode.server->flip_learner_flag(demote_id, /*to=*/true);
                    if (res && res->get_result_code() == nuraft::cmd_result_code::OK)
                    {
                        engine.coverage.hit("flip_voter_to_learner");
                        engine.trace("FLIP_LEARNER_DEMOTE target=" + std::to_string(demote_id));
                        if (engine.buggify.reconfig_overlap)
                        {
                            lnode.server->set_user_ctx("overlap-" + std::to_string(engine.rng.nextU32()));
                            engine.coverage.hit("reconfig_overlap_set_user_ctx_after_flip");
                        }
                    }
                }
            }
        }
        else
        {
            /// `update_srv_config`: update `dc_id`, `aux`, and `priority` on an existing non-joiner server!
            const int target = static_cast<int>(engine.rng.rangeInt(1, cfg.num_nodes));
            auto srv = lnode.server->get_srv_config(target);
            if (srv && !srv->is_new_joiner())
            {
                const int new_dc = static_cast<int>(engine.rng.rangeInt(0, 3));
                const int new_prio = srv->is_learner() ? 0 : static_cast<int>(engine.rng.rangeInt(1, 20));
                nuraft::srv_config updated(
                    srv->get_id(), new_dc, srv->get_endpoint(),
                    "aux-" + std::to_string(engine.rng.nextU32()),
                    srv->is_learner(), new_prio);
                if (lnode.server->update_srv_config(updated))
                {
                    engine.coverage.hit("update_srv_config_succeeded");
                    engine.trace("UPDATE_SRV_CONFIG target=" + std::to_string(target) + " dc=" + std::to_string(new_dc));
                }
            }
        }
    }

    /// Issue a real local read (Get / List / Exists, optionally with watch) on a live node
    /// and verify its response against the independent Keeper semantic oracle at `last_commit_index()`.
    Coordination::ZooKeeperResponsePtr executeLocalRead(
        int node_id,
        Coordination::ZooKeeperRequestPtr request,
        int64_t session_id,
        bool verify_response = true)
    {
        auto & node = nodes.at(node_id);
        if (!node.online || !node.machine)
            return nullptr;
        node.last_commit_response.reset();
        node.last_commit_log_idx = 0;
        node.next_commit_sub_idx = 0;
        /// Track watch deliveries during this commit to check for duplicate DELETED events on AddWatch(PERSISTENT).
        if (engine.buggify.watch_oracle)
            node.recent_watches.clear();
        const uint64_t commit_idx = node.machine->last_commit_index();
        auto req_copy = request;
        DB::KeeperRequestForSession req;
        req.session_id = session_id;
        req.time = simTimeMs();
        req.request = std::move(request);
        node.machine->processReadRequests({req});
        if (!node.last_commit_response)
            return nullptr;
        auto resp = node.last_commit_response->response;
        node.last_commit_response.reset();
        node.last_commit_log_idx = 0;
        node.next_commit_sub_idx = 0;
        if (engine.buggify.watch_oracle && req_copy && req_copy->getOpNum() == Coordination::OpNum::SetWatch)
        {
            const auto & sw = dynamic_cast<const Coordination::ZooKeeperSetWatchesRequest &>(*req_copy);
            for (const auto & [wsid, wpath, wtype] : node.recent_watches)
            {
                const bool in_list = std::find(sw.child_watches.begin(), sw.child_watches.end(), wpath) != sw.child_watches.end();
                const bool in_data = std::find(sw.data_watches.begin(), sw.data_watches.end(), wpath) != sw.data_watches.end();
                if (in_list && !in_data && wtype == static_cast<int32_t>(Coordination::Event::CHANGED))
                {
                    require(false,
                        "KEEPER_WATCH_ORACLE SetWatches child_watch fired Event::CHANGED(3) instead of Event::CHILD(4) on node "
                        + std::to_string(node_id) + " path=" + wpath);
                }
            }
        }
        if (engine.buggify.watch_oracle && req_copy && resp && req_copy->getOpNum() == Coordination::OpNum::CheckWatch)
        {
            const auto & cw = dynamic_cast<const Coordination::ZooKeeperCheckWatchRequest &>(*req_copy);
            auto & st = node.machine->getStorageUnsafe();
            std::shared_lock lock(st.storage_mutex);
            bool own_watch = false;
            if (auto sit = st.sessions_and_watchers.find(session_id); sit != st.sessions_and_watchers.end())
            {
                for (const auto & wi : sit->second)
                {
                    if (wi.path == cw.path)
                        own_watch = true;
                }
            }
            if (resp->error == Coordination::Error::ZOK && !own_watch)
            {
                require(false,
                    "KEEPER_WATCH_ORACLE CheckWatch returned ZOK for session " + std::to_string(session_id)
                    + " which owns NO watch on path=" + cw.path + " (leaked another session's watch!)");
            }
        }
        if (verify_response && req_copy && resp)
            keeper_oracle.verifyLocalReadResponse(node_id, commit_idx, session_id, *req_copy, *resp);
        return resp;
    }

    /// Trigger an explicit snapshot on node `id` via NuRaft `raft_server::create_snapshot`
    /// (matching production ClickHouse Keeper `csnp` / `create_snapshot_on_exit`).
    bool triggerSnapshot(int id)
    {
        auto & node = nodes.at(id);
        if (!node.online || !node.server || !node.machine || !node.log_store)
            return false;
        const uint64_t idx = node.machine->last_commit_index();
        if (idx < 2)
            return false;
        if (auto last_snp = node.machine->last_snapshot(); last_snp && last_snp->get_last_log_idx() >= idx)
            return false;
        if (idx < node.log_store->start_index() || idx >= node.log_store->next_slot())
            return false;

        nuraft::raft_server::create_snapshot_options opts;
        opts.serialize_commit_ = false;
        const uint64_t created_idx = node.server->create_snapshot(opts);
        return created_idx > 0;
    }

    /// Dynamically add node `cfg.num_nodes + 1` to the live cluster via NuRaft `add_srv` (as voter or learner).
    /// Exercises `handle_join_leave.cxx`, `KeeperLogStore::pack`/`apply_pack` (`sync_log_request`),
    /// snapshot catch-up for fresh joiners, and `KeeperStateMachine::commit_config`.
    bool addDynamicNode(int leader_id, bool as_learner)
    {
        auto lit = nodes.find(leader_id);
        if (lit == nodes.end() || !lit->second.online || !lit->second.server || !lit->second.server->is_leader())
            return false;

        const int dyn_id = cfg.num_nodes + 1;
        auto cur_cfg = lit->second.server->get_config();
        if (cur_cfg && cur_cfg->get_server(dyn_id) != nullptr)
            return false;

        auto & node = nodes[dyn_id];
        if (node.online)
            crashNode(dyn_id, KillMode::Graceful);

        node.id = dyn_id;
        node.is_dynamic_joiner = true;
        if (engine.buggify.isActive("clock_skew"))
            node.clock_skew_ms = engine.rng.rangeInt(-45, 45);

        const auto node_dir = cfg.root_dir / ("node_" + std::to_string(dyn_id) + "_gen_" + std::to_string(node.incarnation + 1));
        node.log_disk = std::make_shared<SimCrashDisk>("LogDisk_" + std::to_string(dyn_id),
            node_dir / "live/logs", node_dir / "durable/logs", &engine.coverage);
        node.snapshot_disk = std::make_shared<SimCrashDisk>("SnapshotDisk_" + std::to_string(dyn_id),
            node_dir / "live/snapshots", node_dir / "durable/snapshots", &engine.coverage);
        node.state_disk = std::make_shared<SimCrashDisk>("StateDisk_" + std::to_string(dyn_id),
            node_dir / "live/state", node_dir / "durable/state", &engine.coverage);
        if (cfg.nodeUsesLsmtDisk(dyn_id))
        {
            node.data_disk = std::make_shared<SimCrashDisk>("DataDisk_" + std::to_string(dyn_id),
                node_dir / "live/data", node_dir / "durable/data", &engine.coverage);
        }
        raft_oracle.onNodeDiskWiped(dyn_id, /*state_wiped=*/true);
        startNode(dyn_id);

        nuraft::srv_config new_srv(dyn_id, 0, std::to_string(dyn_id), "", as_learner, as_learner ? 0 : 1);
        auto res = lit->second.server->add_srv(new_srv);
        if (!res || !res->get_accepted())
            return false;

        engine.coverage.hit(as_learner ? "dynamic_add_srv_learner" : "dynamic_add_srv_voter");
        engine.trace("DYNAMIC_ADD_SRV leader=" + std::to_string(leader_id)
            + " dyn=" + std::to_string(dyn_id) + " learner=" + std::to_string(as_learner ? 1 : 0));
        if (engine.buggify.reconfig_overlap)
        {
            expected_dyn_present = true;
            if (engine.rng.chance(0.50))
            {
                lit->second.server->set_user_ctx("overlap-add-" + std::to_string(engine.rng.nextU32()));
                engine.coverage.hit("reconfig_overlap_set_user_ctx_after_add");
            }
            else if (auto srv = lit->second.server->get_srv_config(leader_id); srv && !srv->is_new_joiner())
            {
                nuraft::srv_config updated(srv->get_id(), srv->get_dc_id() + 1, srv->get_endpoint(), "overlap-aux", srv->is_learner(), srv->get_priority());
                lit->second.server->update_srv_config(updated);
                engine.coverage.hit("reconfig_overlap_update_srv_after_add");
            }
        }
        return true;
    }

    /// Dynamically remove node `cfg.num_nodes + 1` via NuRaft `remove_srv`.
    bool removeDynamicNode(int leader_id)
    {
        auto lit = nodes.find(leader_id);
        if (lit == nodes.end() || !lit->second.online || !lit->second.server || !lit->second.server->is_leader())
            return false;

        const int dyn_id = cfg.num_nodes + 1;
        auto cur_cfg = lit->second.server->get_config();
        if (!cur_cfg || cur_cfg->get_server(dyn_id) == nullptr)
            return false;

        auto res = lit->second.server->remove_srv(dyn_id);
        if (!res || !res->get_accepted())
            return false;

        engine.coverage.hit("dynamic_remove_srv");
        engine.trace("DYNAMIC_REMOVE_SRV leader=" + std::to_string(leader_id) + " dyn=" + std::to_string(dyn_id));
        if (engine.buggify.reconfig_overlap)
        {
            expected_dyn_present = false;
            seen_dyn_committed = false;
        }
        return true;
    }

    /// Core-voter membership rotation among IDs `1..(num_nodes+1)`:
    /// - When cluster has `num_nodes` members: add the missing member as voter (50% wiped, 50% stale old disk!).
    /// - When cluster has `num_nodes + 1` members: either yield leadership so the old leader can be removed,
    ///   or remove ANY non-leader voter (including original core voters 1..num_nodes!).
    bool rotateClusterMember(int leader_id)
    {
        auto lit = nodes.find(leader_id);
        if (lit == nodes.end() || !lit->second.online || !lit->second.server || !lit->second.server->is_leader())
            return false;

        auto cur_members = currentClusterMemberIds();
        const int max_id = cfg.num_nodes + 1;

        if (static_cast<int>(cur_members.size()) <= cfg.num_nodes)
        {
            std::vector<int> missing;
            for (int id = 1; id <= max_id; ++id)
            {
                if (std::find(cur_members.begin(), cur_members.end(), id) == cur_members.end())
                    missing.push_back(id);
            }
            if (missing.empty())
                return false;
            const int add_id = engine.rng.pick(missing);
            auto & node = nodes[add_id];
            const bool fresh_disk = (!node.log_disk || engine.rng.chance(0.55));
            if (node.online)
                crashNode(add_id, KillMode::Graceful);
            node.id = add_id;
            node.is_dynamic_joiner = true;
            if (fresh_disk)
            {
                const auto node_dir = cfg.root_dir / ("node_" + std::to_string(add_id) + "_rot_" + std::to_string(node.incarnation + 1));
                node.log_disk = std::make_shared<SimCrashDisk>("LogDisk_" + std::to_string(add_id),
                    node_dir / "live/logs", node_dir / "durable/logs", &engine.coverage);
                node.snapshot_disk = std::make_shared<SimCrashDisk>("SnapshotDisk_" + std::to_string(add_id),
                    node_dir / "live/snapshots", node_dir / "durable/snapshots", &engine.coverage);
                if (engine.buggify.dual_snapshot_disks)
                {
                    node.latest_snapshot_disk = std::make_shared<SimCrashDisk>("LatestSnapDisk_" + std::to_string(add_id),
                        node_dir / "live/latest_snapshots", node_dir / "durable/latest_snapshots", &engine.coverage);
                }
                node.state_disk = std::make_shared<SimCrashDisk>("StateDisk_" + std::to_string(add_id),
                    node_dir / "live/state", node_dir / "durable/state", &engine.coverage);
                if (cfg.nodeUsesLsmtDisk(add_id))
                {
                    node.data_disk = std::make_shared<SimCrashDisk>("DataDisk_" + std::to_string(add_id),
                        node_dir / "live/data", node_dir / "durable/data", &engine.coverage);
                }
                raft_oracle.onNodeDiskWiped(add_id, /*state_wiped=*/true);
            }
            else
            {
                engine.coverage.hit("member_rotation_stale_disk_rejoin");
            }
            startNode(add_id);

            nuraft::srv_config new_srv(add_id, 0, std::to_string(add_id), "", /*learner=*/false, /*priority=*/1);
            auto res = lit->second.server->add_srv(new_srv);
            if (!res || !res->get_accepted())
                return false;

            engine.coverage.hit("member_rotation_add_voter");
            engine.trace("MEMBER_ROTATE_ADD leader=" + std::to_string(leader_id) + " add=" + std::to_string(add_id));
            return true;
        }
        else
        {
            if (engine.rng.chance(0.35))
            {
                yieldLeadership(leader_id);
                engine.coverage.hit("member_rotation_leader_yield");
                return true;
            }
            std::vector<int> removable;
            for (int id : cur_members)
                if (id != leader_id)
                    removable.push_back(id);
            if (removable.empty())
                return false;
            const int rm_id = engine.rng.pick(removable);
            auto res = lit->second.server->remove_srv(rm_id);
            if (!res || !res->get_accepted())
                return false;

            engine.coverage.hit(rm_id <= cfg.num_nodes ? "member_rotation_remove_core_voter" : "member_rotation_remove_spare");
            engine.trace("MEMBER_ROTATE_RM leader=" + std::to_string(leader_id) + " rm=" + std::to_string(rm_id));
            return true;
        }
    }

    /// Return all server IDs currently in the leader's cluster config (or initial 1..num_nodes if no leader).
    std::vector<int> currentClusterMemberIds() const
    {
        const int leader = findLeader();
        if (leader != 0)
        {
            auto it = nodes.find(leader);
            if (it != nodes.end() && it->second.online && it->second.server)
            {
                if (auto cur_cfg = it->second.server->get_config())
                {
                    std::vector<int> ids;
                    for (const auto & srv : cur_cfg->get_servers())
                    {
                        if (srv && nodes.contains(srv->get_id()))
                            ids.push_back(srv->get_id());
                    }
                    if (!ids.empty())
                        return ids;
                }
            }
        }
        std::vector<int> ids;
        for (int id = 1; id <= cfg.num_nodes; ++id)
            ids.push_back(id);
        return ids;
    }

    const std::vector<int64_t> & getRecentlyClosedSessions() const { return recently_closed_sessions; }

    /// Verify every online node's tree and digest against the independent Keeper semantic oracle.
    void verifyAllOnlineNodes()
    {
        for (auto & [id, node] : nodes)
        {
            if (node.online && node.machine)
                keeper_oracle.verifyNodeTree(id, *node.machine);
        }
    }

    /// Final FDB-style convergence & deep consistency assertion after quieting.
    void verifyFullConvergence()
    {
        const int leader = findLeader();
        require(leader != 0, "CONVERGENCE_FAILURE: no leader elected after quieting");

        uint64_t expected_commit = 0;
        uint64_t expected_digest = 0;
        for (int id : currentClusterMemberIds())
        {
            auto & node = nodes.at(id);
            require(node.online && node.server && node.machine, "CONVERGENCE_FAILURE: node offline: " + std::to_string(id));
            const uint64_t cidx = node.machine->last_commit_index();
            const uint64_t digest = node.machine->getStorageUnsafe().getNodesDigest(/*committed=*/true, /*lock_transaction_mutex=*/true).value;
            if (expected_commit == 0)
            {
                expected_commit = cidx;
                expected_digest = digest;
            }
            else
            {
                require(cidx == expected_commit,
                    "CONVERGENCE_FAILURE: commit index mismatch on node " + std::to_string(id)
                    + " (" + std::to_string(cidx) + " vs " + std::to_string(expected_commit) + ")");
                require(digest == expected_digest,
                    "CONVERGENCE_FAILURE: KeeperDigest mismatch on node " + std::to_string(id)
                    + " (" + std::to_string(digest) + " vs " + std::to_string(expected_digest) + ")");
            }
            keeper_oracle.verifyNodeTree(id, *node.machine);
        }
        if (!engine.buggify.async_replication)
            keeper_oracle.verifyAllAckedCommitted();
    }

    void shutdownAll()
    {
        if (shut_down)
            return;
        shut_down = true;
        engine.on_before_step = {};
        engine.on_after_step = {};
        engine.on_node_system_exit = {};
        for (auto & [id, node] : nodes)
        {
            if (node.online)
                crashNode(id, KillMode::Graceful);
        }
        engine.clearPending();
    }

    SimNetwork & getNetwork() { return network; }
    RaftSafetyOracle & getRaftOracle() { return raft_oracle; }
    KeeperSemanticOracle & getKeeperOracle() { return keeper_oracle; }
    std::map<int, NodeRuntime> & getNodes() { return nodes; }
    const ClusterConfig & getConfig() const { return cfg; }

private:
    void buildRaftParams()
    {
        raft_params_cfg.election_timeout_lower_bound_ = engine.buggify.election_timeout_lower_ms;
        raft_params_cfg.election_timeout_upper_bound_ = engine.buggify.election_timeout_upper_ms;
        raft_params_cfg.heart_beat_interval_ = engine.buggify.heartbeat_interval_ms;
        raft_params_cfg.rpc_failure_backoff_ = 0;
        raft_params_cfg.leadership_expiry_ = engine.buggify.leadership_expiry_ms;
        if (engine.buggify.leadership_expiry_ms > 0)
            engine.coverage.hit("leadership_expiry_enabled");
        raft_params_cfg.return_method_ = nuraft::raft_params::async_handler;
        raft_params_cfg.use_bg_thread_for_urgent_commit_ = true;
        raft_params_cfg.use_bg_thread_for_snapshot_io_ = false;
        raft_params_cfg.snapshot_distance_ = engine.buggify.snapshot_distance;
        raft_params_cfg.reserved_log_items_ = 4;
        raft_params_cfg.max_append_size_ = engine.buggify.max_append_size;
        raft_params_cfg.parallel_log_appending_ = engine.buggify.parallel_log_appending;
        raft_params_cfg.max_log_gap_in_stream_ = engine.buggify.max_log_gap_in_stream;
        raft_params_cfg.log_sync_stop_gap_ = engine.buggify.log_sync_stop_gap;
        raft_params_cfg.use_new_joiner_type_ = engine.buggify.use_new_joiner_type;
        if (engine.buggify.snapshot_torture)
            raft_params_cfg.snapshot_sync_ctx_timeout_ = 300000; // Production ClickHouse Keeper default (5 minutes)
        if (engine.buggify.auto_forward)
        {
            raft_params_cfg.auto_forwarding_ = true;
            raft_params_cfg.auto_forwarding_req_timeout_ = 800;
            engine.coverage.hit("auto_forwarding_enabled");
        }
        if (engine.buggify.full_consensus)
        {
            raft_params_cfg.use_full_consensus_among_healthy_members_ = true;
            raft_params_cfg.track_peers_sm_commit_idx_ = true;
            raft_params_cfg.grace_period_of_lagging_state_machine_ = engine.buggify.grace_period_lagging_sm_ms;
            engine.coverage.hit("full_consensus_enabled");
        }
        if (engine.buggify.enable_backpressure)
        {
            raft_params_cfg.max_uncommitted_log_entries_ = engine.buggify.max_uncommitted_log_entries;
            raft_params_cfg.slow_member_backpressure_max_uncommitted_ = engine.buggify.slow_member_backpressure_max_uncommitted;
            raft_params_cfg.slow_member_backpressure_no_progress_timeout_ = engine.buggify.slow_member_backpressure_no_progress_timeout_ms;
            raft_params_cfg.max_append_size_bytes_ = engine.buggify.max_append_size_bytes;
            raft_params_cfg.append_entries_backward_probe_throttle_threshold_ = engine.buggify.backward_probe_throttle_threshold;
            raft_params_cfg.track_peers_sm_commit_idx_ = engine.buggify.track_peers_sm_commit_idx;
            engine.coverage.hit("backpressure_params_active");
        }
        if (cfg.unsafe_quorum_control)
            raft_params_cfg.custom_commit_quorum_size_ = 1;
    }

    /// Replicates production `KeeperServer::callbackFunc` behavior for `PreAppendLogLeader`,
    /// `PreAppendLogFollower`, `AppendLogFailed`, `CommitConfig`, and `Rollback`.
    nuraft::cb_func::ReturnCode handleRaftCallback(int id, nuraft::cb_func::Type type, nuraft::cb_func::Param * param)
    {
        auto it = nodes.find(id);
        if (it == nodes.end() || !it->second.online || !it->second.machine)
            return nuraft::cb_func::ReturnCode::Ok;
        auto & node = it->second;

        switch (type)
        {
            case nuraft::cb_func::PreAppendLogLeader:
            {
                auto & entry = *static_cast<nuraft::ptr<nuraft::log_entry> *>(param->ctx);
                if (!entry || entry->get_val_type() != nuraft::app_log)
                    return nuraft::cb_func::ReturnCode::Ok;

                auto entry_buf = entry->get_buf_ptr();
                DB::KeeperStateMachine::ZooKeeperLogSerializationVersion ser_ver{};
                size_t patched_fields_offset = 0;
                auto batch = node.machine->parseRequestBatch(*entry_buf, /*final=*/false, &ser_ver, &patched_fields_offset);
                batch->first_zxid = node.machine->getNextZxid();
                auto digest = node.machine->preprocessBatch(*batch, /*lock_mutex=*/true);
                if (!digest)
                    return nuraft::cb_func::ReturnCode::ReturnNull;

                batch->digest = *digest;
                batch->committed_log_idx = static_cast<int64_t>(node.machine->last_commit_index());

                size_t bytes_missing = 0;
                if (ser_ver < DB::KeeperStateMachine::ZooKeeperLogSerializationVersion::WITH_TIME)
                    bytes_missing += sizeof(int64_t);
                if (ser_ver < DB::KeeperStateMachine::ZooKeeperLogSerializationVersion::WITH_ZXID_DIGEST)
                    bytes_missing += sizeof(int64_t) + sizeof(uint8_t) + sizeof(uint64_t);

                if (bytes_missing != 0)
                {
                    auto new_buf = nuraft::buffer::alloc(entry_buf->size() + bytes_missing);
                    memcpy(new_buf->data_begin(), entry_buf->data_begin(), entry_buf->size());
                    entry_buf = std::move(new_buf);
                    entry = nuraft::cs_new<nuraft::log_entry>(entry->get_term(), entry_buf, entry->get_val_type());
                }

                DB::KeeperStateMachine::patchSerializedRequestBatch(*entry_buf, ser_ver, patched_fields_offset, *batch);
                return nuraft::cb_func::ReturnCode::Ok;
            }
            case nuraft::cb_func::PreAppendLogFollower:
            {
                const auto & entry = *static_cast<nuraft::ptr<nuraft::log_entry> *>(param->ctx);
                if (entry && entry->get_val_type() == nuraft::app_log)
                    node.machine->parseRequestBatch(entry->get_buf(), /*final=*/false);
                return nuraft::cb_func::ReturnCode::Ok;
            }
            case nuraft::cb_func::AppendLogFailed:
            {
                auto & entry = *static_cast<nuraft::ptr<nuraft::log_entry> *>(param->ctx);
                if (entry && entry->get_val_type() == nuraft::app_log)
                {
                    auto batch = node.machine->parseRequestBatch(entry->get_buf(), /*final=*/true);
                    node.machine->rollbackRequestBatch(*batch, /*allow_missing=*/true);
                }
                return nuraft::cb_func::ReturnCode::Ok;
            }
            case nuraft::cb_func::StateMachineExecution:
            {
                if (param && param->ctx)
                {
                    const uint64_t idx = *static_cast<uint64_t *>(param->ctx);
                    keeper_oracle.onConfigCommit(idx);
                    if (engine.buggify.reconfig_overlap && node.server)
                    {
                        if (auto cur_cfg = node.server->get_config())
                        {
                            const int dyn_id = cfg.num_nodes + 1;
                            const bool has_dyn = (cur_cfg->get_server(dyn_id) != nullptr);
                            if (expected_dyn_present && has_dyn)
                                seen_dyn_committed = true;
                            else if (expected_dyn_present && seen_dyn_committed && !has_dyn)
                            {
                                require(false,
                                    "NURAFT_CONFIG_ORACLE: committed add_srv(" + std::to_string(dyn_id)
                                    + ") was SILENTLY REVERTED at log_idx=" + std::to_string(idx)
                                    + " on node " + std::to_string(id)
                                    + " by concurrent update_srv_config/set_user_ctx!");
                            }
                        }
                    }
                }
                return nuraft::cb_func::ReturnCode::Ok;
            }
            case nuraft::cb_func::GotAppendEntryReqFromLeader:
            {
                /// Track when a node's conflicting tail will be rolled back.
                if (param && param->ctx && node.log_store)
                {
                    auto & req = *static_cast<nuraft::req_msg *>(param->ctx);
                    if (!req.log_entries().empty() && req.get_last_log_idx() + 1 < node.log_store->next_slot())
                        engine.coverage.hit("leader_stepdown_conflict");
                }
                return nuraft::cb_func::ReturnCode::Ok;
            }
            case nuraft::cb_func::SaveSnapshot:
            {
                engine.coverage.hit("snapshot_applied_on_follower");
                return nuraft::cb_func::ReturnCode::Ok;
            }
            default:
                return nuraft::cb_func::ReturnCode::Ok;
        }
    }

    /// Poll all manual background queues (Changelog, SnapshotsQueue, LSMT BackgroundWork)
    /// and schedule them onto SimEngine with realistic simulated disk delays.
    void pollBackgroundWork()
    {
        for (auto & [id, node] : nodes)
        {
            if (!node.online)
                continue;
            const uint64_t inc = node.incarnation;

            /// 1. Real Keeper Changelog manual tasks (Write, Completion, FileOperation).
            if (node.log_store)
            {
                for (const auto & [task, name] : {
                    std::pair{DB::Changelog::BackgroundTask::Write, "log-write"},
                    std::pair{DB::Changelog::BackgroundTask::Completion, "log-complete"},
                    std::pair{DB::Changelog::BackgroundTask::FileOperation, "log-file"}})
                {
                    if (node.log_store->hasPendingTask(task) && node.queued_log_tasks.emplace(task).second)
                    {
                        int64_t delay_us = engine.rng.rangeInt(100, 1200);
                        if (task == DB::Changelog::BackgroundTask::Write
                            && engine.buggify.fire("slow_disk_flush", engine.rng, 0.25))
                        {
                            delay_us = engine.rng.rangeInt(10'000, 90'000);
                        }
                        auto store_copy = node.log_store;
                        engine.scheduleAfterUs(
                            delay_us,
                            name,
                            [this, id, store_copy, task]()
                            {
                                auto it = nodes.find(id);
                                if (it == nodes.end() || !it->second.online)
                                    return;
                                it->second.queued_log_tasks.erase(task);
                                store_copy->runTask(task);
                            },
                            id,
                            inc);
                    }
                }
            }

            /// 2. Real Keeper Snapshot creation tasks (`CreateSnapshotTask`).
            if (node.snapshots_queue && !node.snapshot_task_queued && !node.snapshots_queue->empty())
            {
                DB::CreateSnapshotTask raw_task;
                if (node.snapshots_queue->tryPop(raw_task))
                {
                    node.snapshot_task_queued = true;
                    auto shared_task = std::make_shared<DB::CreateSnapshotTask>(std::move(raw_task));
                    const int64_t delay_ms = engine.buggify.fire("delayed_snapshot_publish", engine.rng, 0.40)
                        ? engine.rng.rangeInt(15, 80)
                        : engine.rng.rangeInt(1, 10);
                    engine.scheduleAfterMs(
                        delay_ms,
                        "keeper-snapshot-create",
                        [this, id, shared_task]()
                        {
                            auto it = nodes.find(id);
                            if (it == nodes.end() || !it->second.online)
                                return;
                            it->second.snapshot_task_queued = false;
                            auto file = shared_task->create_snapshot(std::move(shared_task->snapshot), false);
                            shared_task->create_snapshot = {};
                            if (file)
                            {
                                engine.coverage.hit("snapshot_created");
                                engine.trace("SNAPSHOT_CREATED n=" + std::to_string(id) + " path=" + file->path);
                            }
                        },
                        id,
                        inc);
                }
            }

            /// 3. Real LSMT BackgroundWork manual tasks (Flush, Merge, FileDelete) when node uses LSMT.
            if (cfg.nodeUsesLsmt(id) && node.machine)
            {
                auto * lsmt = dynamic_cast<DB::KeeperLSMTNodesStorage *>(node.machine->getStorageUnsafe().nodes_storage);
                if (lsmt && lsmt->state.background && lsmt->state.background->isManualMode())
                {
                    auto * bg = lsmt->state.background.get();
                    if (!node.lsmt_flush_queued && bg->hasPendingFlush())
                    {
                        node.lsmt_flush_queued = true;
                        const int64_t delay_ms = engine.rng.rangeInt(1, 15);
                        engine.scheduleAfterMs(
                            delay_ms,
                            "lsmt-flush",
                            [this, id]()
                            {
                                auto it = nodes.find(id);
                                if (it == nodes.end() || !it->second.online)
                                    return;
                                it->second.lsmt_flush_queued = false;
                                auto * st = dynamic_cast<DB::KeeperLSMTNodesStorage *>(
                                    it->second.machine->getStorageUnsafe().nodes_storage);
                                if (st && st->state.background && st->state.background->runFlushStep())
                                    engine.coverage.hit("lsmt_flush_executed");
                            },
                            id,
                            inc);
                    }
                    if (!node.lsmt_merge_queued && bg->hasPendingMerge())
                    {
                        node.lsmt_merge_queued = true;
                        const int64_t delay_ms = engine.rng.rangeInt(2, 25);
                        engine.scheduleAfterMs(
                            delay_ms,
                            "lsmt-merge",
                            [this, id]()
                            {
                                auto it = nodes.find(id);
                                if (it == nodes.end() || !it->second.online)
                                    return;
                                it->second.lsmt_merge_queued = false;
                                auto * st = dynamic_cast<DB::KeeperLSMTNodesStorage *>(
                                    it->second.machine->getStorageUnsafe().nodes_storage);
                                if (st && st->state.background && st->state.background->runMergeStep())
                                    engine.coverage.hit("lsmt_merge_executed");
                            },
                            id,
                            inc);
                    }
                    if (!node.lsmt_delete_queued && bg->hasPendingFileDeletes())
                    {
                        node.lsmt_delete_queued = true;
                        engine.scheduleAfterMs(
                            1,
                            "lsmt-file-delete",
                            [this, id]()
                            {
                                auto it = nodes.find(id);
                                if (it == nodes.end() || !it->second.online)
                                    return;
                                it->second.lsmt_delete_queued = false;
                                auto * st = dynamic_cast<DB::KeeperLSMTNodesStorage *>(
                                    it->second.machine->getStorageUnsafe().nodes_storage);
                                if (st && st->state.background && st->state.background->runFileDeleteStep())
                                    engine.coverage.hit("lsmt_file_deleted");
                            },
                            id,
                            inc);
                    }
                }
            }
        }
    }

    /// Periodic leader dead-session sweep: mirrors production KeeperDispatcher's session expiry thread,
    /// replicating real `ZooKeeperCloseRequest` entries for expired sessions!
    void scheduleDeadSessionSweep()
    {
        engine.scheduleAfterMs(
            500,
            "dead-session-sweep",
            [this]()
            {
                if (shut_down)
                    return;
                const int leader_id = findLeader();
                if (leader_id != 0)
                {
                    auto & leader = nodes.at(leader_id);
                    if (leader.online && leader.machine)
                    {
                        const auto dead = leader.machine->getDeadSessions();
                        for (int64_t sid : dead)
                        {
                            if (in_flight_close_sessions.contains(sid))
                                continue;
                            in_flight_close_sessions.insert(sid);
                            auto close_req = std::make_shared<Coordination::ZooKeeperCloseRequest>();
                            close_req->xid = Coordination::CLOSE_XID;
                            auto res = submitRequest(leader_id, close_req, sid);
                            if (res && res->get_accepted())
                            {
                                engine.trace("SESSION_EXPIRE_SUBMIT sid=" + std::to_string(sid));
                                res->when_ready(
                                    [this, sid](nuraft::cmd_result<nuraft::ptr<nuraft::buffer>> & r, nuraft::ptr<std::exception> & err)
                                    {
                                        in_flight_close_sessions.erase(sid);
                                        if (!err && r.get_result_code() == nuraft::cmd_result_code::OK)
                                            recently_closed_sessions.push_back(sid);
                                    });
                            }
                            else
                            {
                                in_flight_close_sessions.erase(sid);
                            }
                        }

                        /// Also run real TTL & Container GC sweeps on virtual time!
                        const auto expired_ttls = leader.machine->getExpiredTTLPathsForGarbageCollector(8);
                        for (const auto & [path, ver] : expired_ttls)
                        {
                            auto rm = std::make_shared<Coordination::ZooKeeperRemoveRequest>();
                            rm->xid = 700000 + (++gc_xid_counter);
                            rm->path = path;
                            rm->version = ver;
                            if (auto res = submitRequest(leader_id, rm, -1); res && res->get_accepted())
                                engine.coverage.hit("ttl_gc_submitted");
                        }
                        const auto containers = leader.machine->getContainerCandidatesForGarbageCollector(
                            8, engine.buggify.container_gc_max_never_used_ms);
                        for (const auto & [path, ver] : containers)
                        {
                            auto rm = std::make_shared<Coordination::ZooKeeperRemoveRequest>();
                            rm->xid = 700000 + (++gc_xid_counter);
                            rm->path = path;
                            rm->version = ver;
                            if (auto res = submitRequest(leader_id, rm, -1); res && res->get_accepted())
                                engine.coverage.hit("container_gc_submitted");
                        }
                    }
                }
                scheduleDeadSessionSweep();
            });
    }

    void checkRaftSafetyAllNodes()
    {
        for (const auto & [id, node] : nodes)
        {
            if (!node.online || !node.server || !node.log_store || !node.state_manager)
                continue;

            RaftSafetyOracle::NodeSnapshot snap;
            snap.id = id;
            snap.incarnation = node.incarnation;
            snap.online = true;
            snap.term = node.server->get_term();
            snap.is_leader = node.server->is_leader();
            snap.last_log_idx = node.server->get_last_log_idx();
            snap.durable_idx = node.log_store->last_durable_index();
            snap.target_commit_idx = node.server->get_target_committed_log_idx();
            snap.applied_idx = node.server->get_committed_log_idx();

            /// Only read state file on term transitions or occasionally to avoid unnecessary disk reads.
            if (auto it = last_seen_term.find(id); it == last_seen_term.end() || it->second != snap.term)
            {
                if (auto st = node.state_manager->read_state())
                    cached_vote[id] = st->get_voted_for();
                last_seen_term[id] = snap.term;
            }
            snap.voted_for = cached_vote.contains(id) ? cached_vote.at(id) : -1;

            raft_oracle.observeNode(
                snap,
                [&node](uint64_t idx) -> std::optional<std::pair<uint64_t, std::string>>
                {
                    if (idx < node.log_store->start_index() || idx >= node.log_store->next_slot())
                        return std::nullopt;
                    auto entry = node.log_store->entry_at(idx);
                    if (!entry)
                        return std::nullopt;
                    auto buf = entry->serialize();
                    return std::make_pair(
                        entry->get_term(),
                        std::string(reinterpret_cast<const char *>(buf->data_begin()), buf->size()));
                });
        }
    }

    SimEngine & engine;
    ClusterConfig cfg;
    SimNetwork network;
    nuraft::ptr<SimRpcFactory> rpc_factory;
    nuraft::cluster_config initial_raft_config;
    nuraft::raft_params raft_params_cfg;
    RaftSafetyOracle raft_oracle;
    KeeperSemanticOracle keeper_oracle;
    std::map<int, NodeRuntime> nodes;
    std::map<int, uint64_t> last_seen_term;
    std::map<int, int> cached_vote;
    std::set<int64_t> in_flight_close_sessions;
    std::vector<int64_t> recently_closed_sessions;
    int64_t gc_xid_counter = 0;
    bool expected_dyn_present = false;
    bool seen_dyn_committed = false;
    bool shut_down = false;
};

} // namespace keeper_sim
