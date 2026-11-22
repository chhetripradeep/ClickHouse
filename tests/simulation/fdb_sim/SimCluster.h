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
#include <IO/WriteBuffer.h>
#include <IO/WriteHelpers.h>

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
    extern const CoordinationSettingsMilliseconds dead_session_check_period_ms;
    extern const CoordinationSettingsUInt64 parallel_read_threads;
    extern const CoordinationSettingsUInt64 committed_memtable_size;
    extern const CoordinationSettingsUInt64 memtable_block_size;
    extern const CoordinationSettingsUInt64 uncommitted_memtable_size;
    extern const CoordinationSettingsUInt64 min_files_to_merge;
    extern const CoordinationSettingsUInt64 max_files_to_merge;
}

namespace fdb_sim
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
    bool unsafe_quorum_control = false;
    bool strict_raft_commit = false;
    int nuraft_log_level = 0; // 0 = silent, 1=Fatal..6=Trace
    std::filesystem::path root_dir;
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
        int64_t clock_skew_ms = 0;

        /// Persistent simulated disks survive across crashes (unless WipeAndRejoin).
        std::shared_ptr<SimCrashDisk> log_disk;
        std::shared_ptr<SimCrashDisk> snapshot_disk;
        std::shared_ptr<SimCrashDisk> state_disk;
        std::shared_ptr<SimCrashDisk> data_disk;

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
        , rpc_factory(nuraft::cs_new<SimRpcFactory>(network))
        , raft_oracle(cfg_.num_nodes, cfg_.strict_raft_commit, engine_.coverage)
        , keeper_oracle(engine_.coverage)
    {
        require(cfg.num_nodes == 3 || cfg.num_nodes == 5, "SimCluster supports 3 or 5 nodes");
        std::filesystem::create_directories(cfg.root_dir);

        for (int id = 1; id <= cfg.num_nodes; ++id)
            initial_raft_config.get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(id, std::to_string(id)));

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

        /// Simulate single-process fail-fast + supervisor restart when NuRaft calls `system_exit`.
        engine.on_node_system_exit = [this](int node_id, int exit_code)
        {
            engine.coverage.hit("node_system_exit_recovered");
            engine.trace("NODE_SYSTEM_EXIT n=" + std::to_string(node_id) + " code=" + std::to_string(exit_code));
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
            node.log_disk = std::make_shared<SimCrashDisk>("LogDisk_" + std::to_string(id),
                node_dir / "live/logs", node_dir / "durable/logs", &engine.coverage);
            node.snapshot_disk = std::make_shared<SimCrashDisk>("SnapshotDisk_" + std::to_string(id),
                node_dir / "live/snapshots", node_dir / "durable/snapshots", &engine.coverage);
            node.state_disk = std::make_shared<SimCrashDisk>("StateDisk_" + std::to_string(id),
                node_dir / "live/state", node_dir / "durable/state", &engine.coverage);
            if (cfg.use_lsmt && cfg.lsmt_on_disk)
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

        auto settings = std::make_shared<DB::CoordinationSettings>();
        (*settings)[DB::CoordinationSetting::use_lsmt_storage] = cfg.use_lsmt;
        (*settings)[DB::CoordinationSetting::storage_memory_only] = !(cfg.use_lsmt && cfg.lsmt_on_disk);
        (*settings)[DB::CoordinationSetting::dead_session_check_period_ms] = 500;
        (*settings)[DB::CoordinationSetting::parallel_read_threads] = 0;
        if (cfg.use_lsmt)
        {
            (*settings)[DB::CoordinationSetting::committed_memtable_size] = engine.buggify.lsmt_memtable_size;
            (*settings)[DB::CoordinationSetting::memtable_block_size] = engine.buggify.lsmt_block_size;
            (*settings)[DB::CoordinationSetting::uncommitted_memtable_size] = engine.buggify.lsmt_memtable_size;
            (*settings)[DB::CoordinationSetting::min_files_to_merge] = engine.buggify.lsmt_min_files_to_merge;
            (*settings)[DB::CoordinationSetting::max_files_to_merge] = 4;
        }

        const int64_t skew = node.clock_skew_ms;
        node.context = std::make_shared<DB::KeeperContext>(
            /*standalone_keeper=*/true,
            settings,
            [skew]() -> int64_t
            {
                return std::max<int64_t>(0, simTimeMs() + skew);
            });
        node.context->setManualLSMTBackgroundWork(true);
        node.context->setDigestEnabled(true);
        node.context->setSnapshotDisk(node.snapshot_disk);
        node.context->setLogDisk(node.log_disk);
        node.context->setStateFileDisk(node.state_disk);
        if (node.data_disk)
            node.context->setDataDisk(node.data_disk);

        node.snapshots_queue = nuraft::cs_new<DB::SnapshotsQueue>(8);
        node.last_commit_response.reset();

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
                    if (auto wr = std::dynamic_pointer_cast<Coordination::ZooKeeperWatchResponse>(resp.response))
                        watch_path = wr->path;
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

                /// Verify Raft State Machine Safety (entry bytes at log_idx).
                if (n.log_store && log_idx >= n.log_store->start_index() && log_idx < n.log_store->next_slot())
                {
                    if (auto entry = n.log_store->entry_at(log_idx))
                    {
                        auto buf = entry->serialize();
                        std::string bytes(reinterpret_cast<const char *>(buf->data_begin()), buf->size());
                        raft_oracle.verifyCommittedEntry(id, log_idx, entry->get_term(), bytes);
                    }
                }

                const uint64_t digest = n.machine->getStorageUnsafe().getNodesDigest(/*committed=*/true, /*lock_transaction_mutex=*/true).value;
                keeper_oracle.onKeeperCommit(id, log_idx, req, resp_ptr, digest);
            });

        node.machine->init();
        const uint64_t snapshot_idx = node.machine->last_commit_index();
        node.context->setLastCommitIndex(snapshot_idx);
        if (snapshot_idx > 0)
            keeper_oracle.verifyDigest(id, snapshot_idx, node.machine->getStorageUnsafe().getNodesDigest(/*committed=*/true, /*lock_transaction_mutex=*/true).value);

        node.log_store = nuraft::cs_new<DB::KeeperLogStore>(
            DB::LogFileSettings{
                .force_sync = true,
                .compress_logs = false,
                .rotate_interval = engine.buggify.log_rotate_interval,
                .startup_read_max_streams = 1,
            },
            DB::FlushSettings{.max_flush_batch_size = 1, .min_time_between_fsyncs_ms = 0},
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
            node.state_disk->simulateGracefulStop();
            engine.coverage.hit("graceful_restart");
        }
        else if (mode == KillMode::PowerLoss)
        {
            const bool allow_torn = engine.buggify.isActive("torn_writes_on_power_loss");
            node.log_disk->simulatePowerLoss(engine.rng, allow_torn);
            node.snapshot_disk->simulatePowerLoss(engine.rng, allow_torn);
            node.state_disk->simulatePowerLoss(engine.rng, allow_torn);
            engine.coverage.hit("power_loss_crash");
        }
        else if (mode == KillMode::WipeAndRejoin)
        {
            node.log_disk->simulateDiskWipe();
            node.snapshot_disk->simulateDiskWipe();
            node.state_disk->simulateDiskWipe();
            raft_oracle.onNodeDiskWiped(id);
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
        auto & node = nodes.at(leader_id);
        if (!node.online || !node.server || !node.server->is_leader())
            return nullptr;

        DB::KeeperRequestForSession entry;
        entry.session_id = session_id;
        entry.time = simTimeMs();
        entry.request = std::move(request);
        DB::KeeperRequestBatch batch;
        batch.dispatcher_server_id = -1;
        batch.requests.push_back(std::move(entry));
        auto bufs = DB::KeeperStateMachine::serializeRequestBatch(batch, engine.buggify.write_log_entry_version);
        return node.server->append_entries(bufs);
    }

    /// Issue a real local read (Get / List / Exists, optionally with watch) on a live node.
    Coordination::ZooKeeperResponsePtr executeLocalRead(
        int node_id,
        Coordination::ZooKeeperRequestPtr request,
        int64_t session_id)
    {
        auto & node = nodes.at(node_id);
        if (!node.online || !node.machine)
            return nullptr;
        node.last_commit_response.reset();
        DB::KeeperRequestForSession req;
        req.session_id = session_id;
        req.time = simTimeMs();
        req.request = std::move(request);
        node.machine->processReadRequests({req});
        if (!node.last_commit_response)
            return nullptr;
        auto resp = node.last_commit_response->response;
        node.last_commit_response.reset();
        return resp;
    }

    /// Trigger an explicit snapshot on node `id` (in addition to NuRaft's automatic `snapshot_distance_`).
    bool triggerSnapshot(int id)
    {
        auto & node = nodes.at(id);
        if (!node.online || !node.machine || !node.log_store)
            return false;
        const uint64_t idx = node.machine->last_commit_index();
        if (idx < 2)
            return false;
        if (auto last_snp = node.machine->last_snapshot(); last_snp && last_snp->get_last_log_idx() >= idx)
            return false;
        if (idx < node.log_store->start_index() || idx >= node.log_store->next_slot())
            return false;

        nuraft::snapshot meta(idx, node.log_store->term_at(idx), node.machine->getClusterConfig());
        nuraft::async_result<bool>::handler_type cb = [](bool &, nuraft::ptr<std::exception> &) {};
        node.machine->create_snapshot(meta, cb);
        return true;
    }

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
        for (auto & [id, node] : nodes)
        {
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
        raft_params_cfg.leadership_expiry_ = 0;
        raft_params_cfg.return_method_ = nuraft::raft_params::async_handler;
        raft_params_cfg.use_bg_thread_for_urgent_commit_ = true;
        raft_params_cfg.use_bg_thread_for_snapshot_io_ = false;
        raft_params_cfg.snapshot_distance_ = engine.buggify.snapshot_distance;
        raft_params_cfg.reserved_log_items_ = 4;
        raft_params_cfg.max_append_size_ = engine.buggify.max_append_size;
        raft_params_cfg.parallel_log_appending_ = engine.buggify.parallel_log_appending;
        raft_params_cfg.max_log_gap_in_stream_ = engine.buggify.max_log_gap_in_stream;
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

            /// 3. Real LSMT BackgroundWork manual tasks (Flush, Merge, FileDelete) when LSMT is enabled.
            if (cfg.use_lsmt && node.machine)
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
                                    [this, sid](nuraft::cmd_result<nuraft::ptr<nuraft::buffer>> &, nuraft::ptr<std::exception> &)
                                    {
                                        in_flight_close_sessions.erase(sid);
                                    });
                            }
                            else
                            {
                                in_flight_close_sessions.erase(sid);
                            }
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
    bool shut_down = false;
};

} // namespace fdb_sim
