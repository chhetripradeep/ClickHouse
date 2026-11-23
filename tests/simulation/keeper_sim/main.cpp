#include <SimClock.h>
#include <SimCluster.h>
#include <SimEngine.h>
#include <SimSwarm.h>
#include <SimWorkloads.h>

#include <Common/Stopwatch.h>
#include <Common/ThreadPool.h>
#include <Common/scope_guard_safe.h>
#include <Common/tests/gtest_global_context.h>
#include <IO/SharedThreadPools.h>
#include <Poco/ConsoleChannel.h>
#include <Poco/FormattingChannel.h>
#include <Poco/Logger.h>
#include <Poco/PatternFormatter.h>
#include <Poco/ThreadPool.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>

namespace
{

std::string jsonEscape(const std::string & s)
{
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s)
    {
        if (c == '"')
            out += "\\\"";
        else if (c == '\\')
            out += "\\\\";
        else if (c == '\n')
            out += "\\n";
        else
            out += c;
    }
    return out;
}

void runInstrumentationControl()
{
    using namespace keeper_sim;
    g_virtual_time_enabled = true;
    g_sim_time_ns = 5'000'000'000LL;
    const auto steady_before = std::chrono::steady_clock::now();
    const auto sys_before = std::chrono::system_clock::now();
    Stopwatch sw;
    g_sim_time_ns += 23'000'000LL;
    require(std::chrono::steady_clock::now() - steady_before == std::chrono::milliseconds(23),
        "Virtual steady_clock did not advance by 23ms");
    require(std::chrono::system_clock::now() - sys_before == std::chrono::milliseconds(23),
        "Virtual system_clock did not advance by 23ms");
    require(sw.elapsedMilliseconds() == 23, "Virtual Stopwatch did not advance by 23ms");

    const size_t host_before = g_host_clock_reads.load();
    timespec ts{};
    require(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts) == 0, "Host clock control failed");
    require(g_host_clock_reads.load() == host_before + 1, "Missed delegated host clock call");

    const size_t threads_before = g_thread_attempts.load();
    std::thread t([]() {});
    t.join();
    require(g_thread_attempts.load() == threads_before + 1, "Missed pthread_create interception");
    std::cout << "PASS keeper-sim-instrumentation-control\n";
}

} // namespace

int main(int argc, char ** argv)
{
    SCOPE_EXIT_SAFE({
        getMutableContext().destroy();
        Poco::ThreadPool::defaultPool().stopAll();
        DB::StaticThreadPool::shutdownAll();
        GlobalThreadPool::shutdown();
    });

    try
    {
        Poco::Logger::root().setLevel("none");
        getContext();

        uint64_t seed = 1;
        int num_nodes = 3;
        int64_t duration_ms = 20000;
        int64_t quiet_ms = 12000;
        std::string profile = "balanced";
        bool use_lsmt = false;
        bool lsmt_on_disk = false;
        bool mixed_storage = false;
        bool enable_buggify = true;
        bool no_streaming = false;
        bool force_compress_logs = false;
        bool no_bounded_cache = false;
        bool dynamic_membership = false;
        bool late_session_race = false;
        bool snapshot_torture = false;
        bool mixed_versions = false;
        bool async_replication = false;
        bool leadership_expiry = false;
        bool force_new_joiner = false;
        bool no_disk_faults = false;
        bool backpressure = false;
        bool large_snap_dist = false;
        bool acl_torture = false;
        bool torn_snapshots = false;
        bool auto_forward = false;
        bool full_consensus = false;
        bool rollback_torture = false;
        bool watch_oracle = false;
        bool remote_snapshots = false;
        bool encrypted_logs = false;
        bool client_stream = false;
        bool hot_reconfig = false;
        bool snapshot_on_exit = false;
        bool member_rotation = false;
        bool remote_snap_faults = false;
        bool deep_lsmt_stress = false;
        bool quorum_reads = false;
        bool log_size_cache = false;
        bool priority_torture = false;
        bool batch_cache_retry = false;
        bool local_disk_cache = false;
        bool reconfig_race = false;
        bool batch_cache_snapshot = false;
        bool reconfig_overlap = false;
        bool broken_tail_writeat = false;
        bool strict_raft_commit = false;
        bool unsafe_quorum_control = false;
        bool instrumentation_control = false;
        bool json_summary = false;
        int host_delay_ms = 0;
        std::filesystem::path state_dir;
        std::string trace_file;
        std::string keeper_log_level = "none";
        int nuraft_log_level = 0;

        bool swarm_mode = false;
        std::string swarm_suite = "quick";
        size_t swarm_workers = 4;
        std::filesystem::path swarm_output;

        for (int i = 1; i < argc; ++i)
        {
            const std::string opt = argv[i];
            if (opt == "--swarm")
                swarm_mode = true;
            else if (opt == "--suite" && i + 1 < argc)
                swarm_suite = argv[++i];
            else if (opt == "--workers" && i + 1 < argc)
                swarm_workers = std::stoul(argv[++i]);
            else if (opt == "--output" && i + 1 < argc)
                swarm_output = argv[++i];
            else if (opt == "--seed" && i + 1 < argc)
                seed = std::stoull(argv[++i]);
            else if (opt == "--nodes" && i + 1 < argc)
                num_nodes = std::stoi(argv[++i]);
            else if (opt == "--duration-ms" && i + 1 < argc)
                duration_ms = std::stoll(argv[++i]);
            else if (opt == "--quiet-ms" && i + 1 < argc)
                quiet_ms = std::stoll(argv[++i]);
            else if (opt == "--profile" && i + 1 < argc)
                profile = argv[++i];
            else if (opt == "--lsmt")
                use_lsmt = true;
            else if (opt == "--lsmt-disk")
            {
                use_lsmt = true;
                lsmt_on_disk = true;
            }
            else if (opt == "--mixed-storage")
                mixed_storage = true;
            else if (opt == "--no-streaming")
                no_streaming = true;
            else if (opt == "--compress-logs")
                force_compress_logs = true;
            else if (opt == "--no-bounded-cache")
                no_bounded_cache = true;
            else if (opt == "--dynamic-membership")
                dynamic_membership = true;
            else if (opt == "--late-session-race")
                late_session_race = true;
            else if (opt == "--snapshot-torture")
                snapshot_torture = true;
            else if (opt == "--mixed-versions")
                mixed_versions = true;
            else if (opt == "--async-replication")
                async_replication = true;
            else if (opt == "--leadership-expiry")
                leadership_expiry = true;
            else if (opt == "--force-new-joiner")
                force_new_joiner = true;
            else if (opt == "--no-disk-faults")
                no_disk_faults = true;
            else if (opt == "--backpressure")
                backpressure = true;
            else if (opt == "--large-snap-dist")
                large_snap_dist = true;
            else if (opt == "--acl-torture")
                acl_torture = true;
            else if (opt == "--torn-snapshots")
                torn_snapshots = true;
            else if (opt == "--auto-forward")
                auto_forward = true;
            else if (opt == "--full-consensus")
                full_consensus = true;
            else if (opt == "--rollback-torture")
                rollback_torture = true;
            else if (opt == "--watch-oracle")
                watch_oracle = true;
            else if (opt == "--remote-snapshots")
                remote_snapshots = true;
            else if (opt == "--encrypted-logs")
                encrypted_logs = true;
            else if (opt == "--client-stream")
                client_stream = true;
            else if (opt == "--hot-reconfig")
                hot_reconfig = true;
            else if (opt == "--snapshot-on-exit")
                snapshot_on_exit = true;
            else if (opt == "--member-rotation")
                member_rotation = true;
            else if (opt == "--remote-snap-faults")
                remote_snap_faults = true;
            else if (opt == "--deep-lsmt-stress")
                deep_lsmt_stress = true;
            else if (opt == "--quorum-reads")
                quorum_reads = true;
            else if (opt == "--log-size-cache")
                log_size_cache = true;
            else if (opt == "--priority-torture")
                priority_torture = true;
            else if (opt == "--batch-cache-retry")
                batch_cache_retry = true;
            else if (opt == "--local-disk-cache")
                local_disk_cache = true;
            else if (opt == "--reconfig-race")
                reconfig_race = true;
            else if (opt == "--batch-cache-snapshot")
                batch_cache_snapshot = true;
            else if (opt == "--reconfig-overlap")
                reconfig_overlap = true;
            else if (opt == "--broken-tail-writeat")
                broken_tail_writeat = true;
            else if (opt == "--buggify")
                enable_buggify = true;
            else if (opt == "--no-buggify")
                enable_buggify = false;
            else if (opt == "--strict-raft-commit")
                strict_raft_commit = true;
            else if (opt == "--unsafe-quorum-control")
                unsafe_quorum_control = true;
            else if (opt == "--instrumentation-control")
                instrumentation_control = true;
            else if (opt == "--host-delay")
                host_delay_ms = 1;
            else if (opt == "--state-dir" && i + 1 < argc)
                state_dir = argv[++i];
            else if (opt == "--trace-file" && i + 1 < argc)
                trace_file = argv[++i];
            else if (opt == "--keeper-log-level" && i + 1 < argc)
                keeper_log_level = argv[++i];
            else if (opt == "--nuraft-log-level" && i + 1 < argc)
                nuraft_log_level = std::stoi(argv[++i]);
            else if (opt == "--json-summary")
                json_summary = true;
            else
                throw std::runtime_error("Unknown or incomplete option: " + opt);
        }

        if (keeper_log_level != "none")
        {
            Poco::AutoPtr<Poco::ConsoleChannel> console_chan(new Poco::ConsoleChannel(std::cerr));
            Poco::AutoPtr<Poco::PatternFormatter> formatter(new Poco::PatternFormatter("%p [%s]: %t"));
            Poco::AutoPtr<Poco::FormattingChannel> fmt_chan(new Poco::FormattingChannel(formatter, console_chan));
            Poco::Logger::root().setChannel(fmt_chan);
            Poco::Logger::root().setLevel(keeper_log_level);
        }

        if (swarm_mode)
        {
            keeper_sim::require(!swarm_output.empty(), "--swarm requires --output <dir>");
            return keeper_sim::SimSwarmRunner::run(argv[0], swarm_suite, swarm_output, swarm_workers, strict_raft_commit);
        }

        if (instrumentation_control)
        {
            runInstrumentationControl();
            return 0;
        }

        using namespace keeper_sim;
        require(!state_dir.empty(), "--state-dir is required");
        if (std::filesystem::exists(state_dir))
            std::filesystem::remove_all(state_dir);

        /// Initialize deterministic virtual time and process RNG BEFORE constructing any Keeper objects.
        g_sim_seed = seed;
        g_sim_time_ns = 1'000'000'000LL;
        g_virtual_time_enabled = true;
        seedProcessRng(seed);

        const size_t host_reads_before = g_host_clock_reads.load();
        require(g_thread_attempts.load() == 0 && liveThreadCount() == 1,
            "Simulation did not start single-threaded");

        SimEngine engine(seed, host_delay_ms, trace_file);
        engine.buggify.initialize(
            engine.rng,
            enable_buggify,
            profile,
            no_streaming,
            force_compress_logs,
            no_bounded_cache,
            dynamic_membership,
            late_session_race,
            snapshot_torture,
            mixed_versions,
            async_replication,
            leadership_expiry,
            force_new_joiner,
            no_disk_faults,
            backpressure,
            large_snap_dist,
            acl_torture,
            torn_snapshots,
            auto_forward,
            full_consensus,
            rollback_torture,
            watch_oracle,
            remote_snapshots,
            encrypted_logs,
            client_stream,
            hot_reconfig,
            snapshot_on_exit,
            member_rotation,
            remote_snap_faults,
            deep_lsmt_stress,
            quorum_reads,
            log_size_cache,
            priority_torture,
            batch_cache_retry,
            local_disk_cache,
            reconfig_race,
            batch_cache_snapshot,
            reconfig_overlap,
            broken_tail_writeat);

        ClusterConfig cluster_cfg;
        cluster_cfg.num_nodes = num_nodes;
        cluster_cfg.use_lsmt = use_lsmt;
        cluster_cfg.lsmt_on_disk = lsmt_on_disk;
        cluster_cfg.mixed_storage = mixed_storage;
        cluster_cfg.unsafe_quorum_control = unsafe_quorum_control;
        cluster_cfg.strict_raft_commit = strict_raft_commit;
        cluster_cfg.nuraft_log_level = nuraft_log_level;
        cluster_cfg.root_dir = state_dir;

        const int64_t start_ms = simTimeMs();
        const int64_t chaos_end_ms = start_ms + duration_ms;

        {
            SimCluster cluster(engine, cluster_cfg);
            ZooKeeperWorkload workload(engine, cluster, chaos_end_ms);
            ChaosActor chaos(engine, cluster, profile, chaos_end_ms);

            cluster.startAll();
            workload.start();
            chaos.start();

            /// Phase 1: Active concurrent workload + continuous FDB chaos.
            engine.runUntilMs(chaos_end_ms);

            /// Phase 2: FDB Quieting — heal network, reboot crashed nodes, prove liveness & convergence.
            workload.stop();
            chaos.stopAndHeal();

            /// Wait for a healthy leader after healing.
            const bool elected = engine.runUntilCondition(
                quiet_ms / 2,
                [&cluster]() { return cluster.findLeader() != 0; });
            require(elected, "LIVENESS_FAILURE: cluster failed to elect a leader after healing");

            /// Submit a final barrier write (retrying if cancelled by stepdown) and wait until it replicates to ALL nodes.
            bool barrier_attempted = false;
            const bool converged = engine.runUntilCondition(
                quiet_ms,
                [&]()
                {
                    const int leader = cluster.findLeader();
                    if (leader == 0)
                        return false;
                    if (!workload.isFinalBarrierAcked() && !workload.isFinalBarrierInFlight())
                    {
                        if (workload.submitFinalBarrier(leader))
                            barrier_attempted = true;
                    }

                    /// Wait until every node is online and matches the leader's commit index (and barrier acked if sessions existed).
                    const uint64_t leader_commit = cluster.getNodes().at(leader).machine->last_commit_index();
                    if (leader_commit == 0)
                        return false;
                    if (barrier_attempted && !workload.isFinalBarrierAcked())
                        return false;
                    for (int id : cluster.currentClusterMemberIds())
                    {
                        const auto & node = cluster.getNodes().at(id);
                        if (!node.online || !node.machine || node.machine->last_commit_index() != leader_commit)
                            return false;
                    }
                    return true;
                });
            require(converged, "CONVERGENCE_FAILURE: replicas did not converge within quieting window");

            /// Phase 3: Full deep verification across every replica, digest, tree, and client linearizability.
            cluster.verifyFullConvergence();
            workload.verifyLinearizabilityRegister();

            const auto final_leader = cluster.findLeader();
            const uint64_t final_commit_idx = cluster.getNodes().at(final_leader).machine->last_commit_index();
            const uint64_t final_digest = cluster.getNodes().at(final_leader).machine->getStorageUnsafe().getNodesDigest(/*committed=*/true, /*lock_transaction_mutex=*/true).value;

            /// Aggregate disk crash stats across all nodes before shutdown.
            uint64_t total_files_synced = 0;
            uint64_t total_power_losses = 0;
            uint64_t total_dirty_reverted = 0;
            uint64_t total_dirty_torn = 0;
            uint64_t total_dirty_corrupted = 0;
            uint64_t total_disk_faults = 0;
            for (const auto & [id, node] : cluster.getNodes())
            {
                for (const auto & d : {node.log_disk, node.snapshot_disk, node.state_disk, node.data_disk})
                {
                    if (!d)
                        continue;
                    const auto & st = d->getStats();
                    total_files_synced += st.files_synced;
                    total_power_losses += st.power_losses;
                    total_dirty_reverted += st.dirty_files_reverted;
                    total_dirty_torn += st.dirty_files_torn;
                    total_dirty_corrupted += st.dirty_files_corrupted_tail;
                    total_disk_faults += st.transient_faults_triggered;
                }
            }

            const auto net_stats = cluster.getNetwork().getStats();
            const auto raft_stats = cluster.getRaftOracle().getStats();
            const auto keeper_stats = cluster.getKeeperOracle().getStats();

            cluster.shutdownAll();

            require(g_thread_attempts.load() == 0 && liveThreadCount() == 1,
                "Uncontrolled OS thread created during simulation");
            require(g_host_clock_reads.load() == host_reads_before,
                "Simulation delegated to uncontrolled host clock");
            require(g_virtual_posix_reads.load() > 0, "Virtual POSIX clock instrumentation was not exercised");

            if (json_summary)
            {
                std::cout << "{";
                std::cout << "\"verdict\":\"PASS\"";
                std::cout << ",\"seed\":" << seed;
                std::cout << ",\"nodes\":" << num_nodes;
                std::cout << ",\"profile\":\"" << jsonEscape(profile) << "\"";
                std::cout << ",\"storage\":\"" << (use_lsmt ? (lsmt_on_disk ? "LSMT_DISK" : "LSMT_MEM") : "MEM") << "\"";
                std::cout << ",\"virtual_time_ms\":" << (simTimeMs() - start_ms);
                std::cout << ",\"executed_events\":" << engine.executed_events;
                std::cout << ",\"trace_lines\":" << engine.trace_lines;
                std::cout << ",\"trace_hash\":\"" << engine.traceHashHex() << "\"";
                std::cout << ",\"final_commit_index\":" << final_commit_idx;
                std::cout << ",\"final_digest\":" << final_digest;
                std::cout << ",\"thread_attempts\":" << g_thread_attempts.load();
                std::cout << ",\"host_clock_leaks\":" << (g_host_clock_reads.load() - host_reads_before);
                std::cout << ",\"workload\":{\"ops_submitted\":" << workload.ops_submitted
                          << ",\"ops_rejected_no_leader\":" << workload.ops_rejected_no_leader
                          << ",\"reads_executed\":" << workload.reads_executed << "}";
                std::cout << ",\"oracle\":{\"model_commits\":" << keeper_stats.model_commits
                          << ",\"replica_responses_checked\":" << keeper_stats.replica_responses_checked
                          << ",\"digests_checked\":" << keeper_stats.digests_checked
                          << ",\"deep_tree_checks\":" << keeper_stats.deep_tree_checks
                          << ",\"client_ops_acked\":" << keeper_stats.client_ops_acked
                          << ",\"client_ops_unknown\":" << keeper_stats.client_ops_unknown
                          << ",\"watches_delivered\":" << keeper_stats.watches_delivered
                          << ",\"multi_committed\":" << keeper_stats.multi_committed
                          << ",\"multi_rolled_back\":" << keeper_stats.multi_rolled_back
                          << ",\"sessions_closed\":" << keeper_stats.sessions_closed
                          << ",\"sequential_created\":" << keeper_stats.sequential_created
                          << ",\"raft_state_checks\":" << raft_stats.state_checks
                          << ",\"raft_elections\":" << raft_stats.elections_observed
                          << ",\"raft_figure8_violations\":" << raft_stats.figure8_violations << "}";
                std::cout << ",\"network\":{\"requests_sent\":" << net_stats.requests_sent
                          << ",\"dropped_partition\":" << net_stats.requests_dropped_partition
                          << ",\"dropped_loss\":" << net_stats.requests_dropped_loss
                          << ",\"responses_dropped\":" << net_stats.responses_dropped_loss
                          << ",\"clogged\":" << net_stats.requests_clogged
                          << ",\"snapshot_rpcs\":" << net_stats.snapshot_rpcs_sent << "}";
                std::cout << ",\"disk\":{\"files_synced\":" << total_files_synced
                          << ",\"power_losses\":" << total_power_losses
                          << ",\"dirty_reverted\":" << total_dirty_reverted
                          << ",\"dirty_torn\":" << total_dirty_torn
                          << ",\"dirty_corrupted_tail\":" << total_dirty_corrupted
                          << ",\"transient_faults\":" << total_disk_faults << "}";
                std::cout << ",\"buggify\":{";
                bool first = true;
                for (const auto & [k, v] : engine.buggify.switches)
                {
                    if (!first)
                        std::cout << ",";
                    first = false;
                    std::cout << "\"" << jsonEscape(k) << "\":" << (v ? "true" : "false");
                }
                std::cout << "}";
                std::cout << ",\"coverage\":{";
                first = true;
                for (const auto & [k, v] : engine.coverage.all())
                {
                    if (!first)
                        std::cout << ",";
                    first = false;
                    std::cout << "\"" << jsonEscape(k) << "\":" << v;
                }
                std::cout << "}";
                std::cout << "}\n";
            }
            else
            {
                std::cout << "PASS keeper_sim seed=" << seed
                          << " nodes=" << num_nodes
                          << " profile=" << profile
                          << " storage=" << (use_lsmt ? (lsmt_on_disk ? "LSMT_DISK" : "LSMT_MEM") : "MEM")
                          << " events=" << engine.executed_events
                          << " commits=" << keeper_stats.model_commits
                          << " final_idx=" << final_commit_idx
                          << " digest=" << final_digest
                          << " trace_hash=" << engine.traceHashHex() << '\n';
            }
        }
        return 0;
    }
    catch (const std::exception & e)
    {
        std::cerr << "FAIL " << e.what() << '\n';
        return 1;
    }
}
