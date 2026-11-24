#pragma once

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

namespace keeper_sim
{

/// Pure C++ FoundationDB Joshua-style multi-process swarm runner.
/// Uses POSIX `fork`/`execv`/`pipe`/`waitpid` (zero threads) so each seed runs in a
/// completely isolated process with fresh ClickHouse global state, full linker clock/thread
/// instrumentation, parallel CPU utilization, cross-run `--host-delay` determinism checking,
/// negative quorum safety controls, and aggregated `SOMETIMES` coverage reporting.
class SimSwarmRunner
{
public:
    struct JobSpec
    {
        std::string name;
        uint64_t seed = 1;
        int nodes = 3;
        std::string profile = "balanced";
        std::string storage = "mem"; // "mem", "lsmt-mem", "lsmt-disk", "mixed"
        int64_t duration_ms = 15000;
        bool host_delay = false;
        bool strict_raft_commit = false;
        bool unsafe_quorum_control = false;
        bool expect_failure = false;
        bool no_streaming = false;
        bool compress_logs = false;
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
        bool lazy_startup = false;
        bool snapshot_vote_safety = false;
        bool huge_lsmt_values = false;
        bool same_path_multi_churn = false;
        bool snapshot_versions_mix = false;
    };

    struct JobResult
    {
        JobSpec spec;
        int exit_code = -1;
        bool signalled = false;
        int term_signal = 0;
        bool passed = false;
        double elapsed_s = 0.0;
        std::string command;
        std::string stdout_text;
        std::string stderr_text;
        std::string trace_hash;
        uint64_t executed_events = 0;
        uint64_t final_commit_index = 0;
        uint64_t final_digest = 0;
        std::map<std::string, uint64_t> coverage;
    };

    static std::vector<JobSpec> buildSuite(const std::string & suite)
    {
        std::vector<JobSpec> jobs;
        auto add_group = [&](const std::string & name, const std::string & profile, int nodes,
                             const std::string & storage, const std::vector<uint64_t> & seeds, int64_t duration_ms,
                             bool no_streaming = false, bool compress_logs = false, bool no_bounded_cache = false,
                             bool dynamic_membership = false, bool late_session_race = false,
                             bool snapshot_torture = false, bool mixed_versions = false,
                             bool async_replication = false, bool leadership_expiry = false,
                             bool force_new_joiner = false, bool no_disk_faults = false,
                             bool backpressure = false, bool large_snap_dist = false,
                             bool acl_torture = false, bool torn_snapshots = false,
                             bool auto_forward = false, bool full_consensus = false,
                             bool rollback_torture = false, bool watch_oracle = false,
                             bool remote_snapshots = false, bool encrypted_logs = false,
                             bool client_stream = false, bool hot_reconfig = false,
                             bool snapshot_on_exit = false, bool member_rotation = false,
                             bool remote_snap_faults = false, bool deep_lsmt_stress = false,
                             bool quorum_reads = false, bool log_size_cache = false,
                             bool priority_torture = false, bool batch_cache_retry = false,
                             bool local_disk_cache = false, bool reconfig_race = false,
                             bool batch_cache_snapshot = false, bool reconfig_overlap = false,
                             bool broken_tail_writeat = false, bool lazy_startup = false,
                             bool snapshot_vote_safety = false, bool huge_lsmt_values = false,
                             bool same_path_multi_churn = false, bool snapshot_versions_mix = false)
        {
            for (uint64_t s : seeds)
            {
                JobSpec spec;
                spec.name = name + "_s" + std::to_string(s);
                spec.seed = s;
                spec.nodes = nodes;
                spec.profile = profile;
                spec.storage = storage;
                spec.duration_ms = duration_ms;
                spec.no_streaming = no_streaming;
                spec.compress_logs = compress_logs;
                spec.no_bounded_cache = no_bounded_cache;
                spec.dynamic_membership = dynamic_membership;
                spec.late_session_race = late_session_race;
                spec.snapshot_torture = snapshot_torture;
                spec.mixed_versions = mixed_versions;
                spec.async_replication = async_replication;
                spec.leadership_expiry = leadership_expiry;
                spec.force_new_joiner = force_new_joiner;
                spec.no_disk_faults = no_disk_faults;
                spec.backpressure = backpressure;
                spec.large_snap_dist = large_snap_dist;
                spec.acl_torture = acl_torture;
                spec.torn_snapshots = torn_snapshots;
                spec.auto_forward = auto_forward;
                spec.full_consensus = full_consensus;
                spec.rollback_torture = rollback_torture;
                spec.watch_oracle = watch_oracle;
                spec.remote_snapshots = remote_snapshots;
                spec.encrypted_logs = encrypted_logs;
                spec.client_stream = client_stream;
                spec.hot_reconfig = hot_reconfig;
                spec.snapshot_on_exit = snapshot_on_exit;
                spec.member_rotation = member_rotation;
                spec.remote_snap_faults = remote_snap_faults;
                spec.deep_lsmt_stress = deep_lsmt_stress;
                spec.quorum_reads = quorum_reads;
                spec.log_size_cache = log_size_cache;
                spec.priority_torture = priority_torture;
                spec.batch_cache_retry = batch_cache_retry;
                spec.local_disk_cache = local_disk_cache;
                spec.reconfig_race = reconfig_race;
                spec.batch_cache_snapshot = batch_cache_snapshot;
                spec.reconfig_overlap = reconfig_overlap;
                spec.broken_tail_writeat = broken_tail_writeat;
                spec.lazy_startup = lazy_startup;
                spec.snapshot_vote_safety = snapshot_vote_safety;
                spec.huge_lsmt_values = huge_lsmt_values;
                spec.same_path_multi_churn = same_path_multi_churn;
                spec.snapshot_versions_mix = snapshot_versions_mix;
                jobs.push_back(std::move(spec));
            }
        };

        if (suite == "quick")
        {
            add_group("mem-balanced-3n", "balanced", 3, "mem", {1, 2}, 12000);
            add_group("mem-power-loss-3n", "power-loss", 3, "mem", {21, 22}, 14000);
            add_group("mem-snapshot-storm-3n", "snapshot-storm", 3, "mem", {31}, 14000);
            add_group("mem-partition-5n", "partition-storm", 5, "mem", {41}, 14000);
            add_group("lsmt-disk-chaos-3n", "chaos", 3, "lsmt-disk", {61}, 14000);
        }
        else if (suite == "full")
        {
            add_group("mem-balanced-3n", "balanced", 3, "mem", {1, 2, 3, 4}, 15000);
            add_group("mem-chaos-3n", "chaos", 3, "mem", {12, 13, 14, 15}, 15000);
            add_group("mem-power-loss-3n", "power-loss", 3, "mem", {21, 22, 23, 24}, 15000);
            add_group("mem-snapshot-storm-3n", "snapshot-storm", 3, "mem", {31, 32, 33}, 15000);
            add_group("mem-partition-5n", "partition-storm", 5, "mem", {41, 42, 43}, 15000);
            add_group("lsmt-mem-chaos-3n", "chaos", 3, "lsmt-mem", {51, 52, 53}, 15000);
            add_group("lsmt-disk-chaos-3n", "chaos", 3, "lsmt-disk", {61, 62, 63}, 15000);
        }
        else if (suite == "hunt")
        {
            /// High-intensity bug-hunting campaign with --no-streaming to look past #133552:
            /// heterogeneous Mem+LSMT-Mem+LSMT-Disk differential clusters, compressed Zstd logs
            /// under power loss, multi-request batches, extended Keeper ops, TTL/Container GC,
            /// leadership yield, and 5-node partition storms.
            add_group("mixed-chaos-3n", "chaos", 3, "mixed", {1001, 1002, 1003, 1004, 1005, 1006, 1007, 1008}, 16000, true, false);
            add_group("mixed-power-loss-3n", "power-loss", 3, "mixed", {2001, 2002, 2003, 2004, 2005, 2006, 2007, 2008}, 16000, true, true);
            add_group("mixed-snap-3n", "snapshot-storm", 3, "mixed", {3001, 3002, 3003, 3004, 3005, 3006}, 16000, true, false);
            add_group("mixed-chaos-5n", "chaos", 5, "mixed", {4001, 4002, 4003, 4004, 4005, 4006}, 16000, true, false);
            add_group("lsmt-disk-pl-3n", "power-loss", 3, "lsmt-disk", {5001, 5002, 5003, 5004, 5005, 5006}, 16000, true, true);
            add_group("mem-zstd-pl-3n", "power-loss", 3, "mem", {6001, 6002, 6003, 6004, 6005, 6006}, 16000, true, true);
        }
        else if (suite == "hunt2")
        {
            /// Wave 2: 60 seeds exercising v9 snapshots with Container & TTL nodes, chunked snapshot
            /// transfers, Auth + SetACL + ACLMap refcounts, dynamic priority reconfiguration, async
            /// unsynced torn writes, and mixed Mem/LSMT-Mem/LSMT-Disk (--no-streaming --no-bounded-cache
            /// to look past Bugs #2, #4, #5, #6).
            add_group("w2-mixed-chaos-3n", "chaos", 3, "mixed", {10001, 10002, 10003, 10004, 10005, 10006, 10007, 10008, 10009, 10010}, 16000, true, false, true);
            add_group("w2-mixed-pl-3n", "power-loss", 3, "mixed", {10011, 10012, 10013, 10014, 10015, 10016, 10017, 10018, 10019, 10020}, 16000, true, false, true);
            add_group("w2-mixed-snap-3n", "snapshot-storm", 3, "mixed", {10021, 10022, 10023, 10024, 10025, 10026, 10027, 10028}, 16000, true, false, true);
            add_group("w2-mixed-chaos-5n", "chaos", 5, "mixed", {10029, 10030, 10031, 10032, 10033, 10034, 10035, 10036}, 16000, true, false, true);
            add_group("w2-lsmt-disk-3n", "chaos", 3, "lsmt-disk", {10037, 10038, 10039, 10040, 10041, 10042, 10043, 10044}, 16000, true, false, true);
            add_group("w2-lsmt-pl-3n", "power-loss", 3, "lsmt-disk", {10045, 10046, 10047, 10048, 10049, 10050, 10051, 10052}, 16000, true, true, true);
            add_group("w2-mem-5n-part", "partition-storm", 5, "mem", {10053, 10054, 10055, 10056, 10057, 10058, 10059, 10060}, 16000, true, false, true);
        }
        else if (suite == "hunt3")
        {
            /// Wave 3: 80 aggressive seeds stressing multi-file LSMT incremental merges + data_disk I/O
            /// faults, ghost-session oracle checks, v9 snapshots + chunked transfer, ACLs/Auth, dynamic
            /// priority reconfiguration, and 3n/5n mixed/LSMT/Mem chaos & power-loss.
            std::vector<uint64_t> s1, s2, s3, s4;
            for (uint64_t i = 0; i < 20; ++i)
            {
                s1.push_back(20001 + i);
                s2.push_back(20101 + i);
                s3.push_back(20201 + i);
                s4.push_back(20301 + i);
            }
            add_group("w3-lsmt-multifile-3n", "chaos", 3, "lsmt-disk", s1, 18000, true, false, true);
            add_group("w3-mixed-chaos-3n", "chaos", 3, "mixed", s2, 18000, true, false, true);
            add_group("w3-mixed-snap-3n", "snapshot-storm", 3, "mixed", s3, 18000, true, false, true);
            add_group("w3-mixed-pl-5n", "power-loss", 5, "mixed", s4, 18000, true, false, true);
        }
        else if (suite == "hunt4")
        {
            /// Wave 4: 80 seeds targeting dynamic membership (`add_srv` voter/learner, `remove_srv`,
            /// `pack`/`apply_pack` `sync_log_request`, `commit_config`), late-closed-session ghost
            /// resurrection race, `ListWithOptions`/`CheckStat`/`SetWatches`/`SetWatches2`, and
            /// preserved-`state_disk` `WipeAndRejoin` snapshot recovery under chaos & power-loss.
            std::vector<uint64_t> s1, s2, s3, s4;
            for (uint64_t i = 0; i < 20; ++i)
            {
                s1.push_back(30001 + i);
                s2.push_back(30101 + i);
                s3.push_back(30201 + i);
                s4.push_back(30301 + i);
            }
            add_group("w4-dyn-mixed-3n", "chaos", 3, "mixed", s1, 18000, true, false, true, true, false);
            add_group("w4-dyn-snap-3n", "snapshot-storm", 3, "lsmt-disk", s2, 18000, true, false, true, true, false);
            add_group("w4-late-sess-3n", "chaos", 3, "mixed", s3, 18000, true, false, true, false, true);
            add_group("w4-ops-pl-5n", "power-loss", 5, "mixed", s4, 18000, true, false, true, false, false);
        }
        else if (suite == "hunt5")
        {
            /// Wave 5: 80 seeds hammering chunked snapshot torture (multi-chunk 64-256B + 30% reply drop
            /// after exec), mixed-version rolling upgrades (v0/v8 + v1/v9), async_replication + non-zero
            /// leadership_expiry + Sync requests, and deep large-value LSMT-Disk + dynamic membership!
            std::vector<uint64_t> s1, s2, s3, s4;
            for (uint64_t i = 0; i < 20; ++i)
            {
                s1.push_back(40001 + i);
                s2.push_back(40101 + i);
                s3.push_back(40201 + i);
                s4.push_back(40301 + i);
            }
            add_group("w5-snap-torture-3n", "snapshot-storm", 3, "mixed", s1, 18000, true, false, true, false, false, true, false, false, false);
            add_group("w5-mixed-ver-3n", "chaos", 3, "mixed", s2, 18000, true, false, true, false, false, false, true, false, false);
            add_group("w5-async-repl-3n", "partition-storm", 3, "mixed", s3, 18000, true, false, true, false, false, false, false, true, true);
            add_group("w5-lsmt-deep-5n", "chaos", 5, "lsmt-disk", s4, 18000, true, false, true, true, false, true, false, false, false);
        }
        else if (suite == "hunt6")
        {
            /// Wave 6: 80 seeds with strict fatal `system_exit` checking:
            /// 1. Chunked snapshot torture + fast follower reboots + 5-min `snapshot_sync_ctx_timeout_`
            ///    + ZERO disk faults (targets `save_logical_snp_obj` missing-throw on `is_last_obj`!).
            /// 2. Dynamic membership with `use_new_joiner_type_ = true` (looks past Bug #9!).
            /// 3. Compressed Zstd logs + unbounded cache under power-loss & snapshot-storm.
            /// 4. 5-node mixed Mem/LSMT-Mem/LSMT-Disk heavy chaos with large values & all ZK ops.
            std::vector<uint64_t> s1, s2, s3, s4;
            for (uint64_t i = 0; i < 20; ++i)
            {
                s1.push_back(50001 + i);
                s2.push_back(50101 + i);
                s3.push_back(50201 + i);
                s4.push_back(50301 + i);
            }
            add_group("w6-snap-reboot-3n", "chaos", 3, "mixed", s1, 18000, true, false, true, false, false, true, false, false, false, false, true);
            add_group("w6-dyn-newjoiner-3n", "chaos", 3, "mixed", s2, 18000, true, false, true, true, false, true, false, false, false, true, true);
            add_group("w6-zstd-unbounded-3n", "power-loss", 3, "mixed", s3, 18000, true, true, true, false, false, false, false, false, false, false, true);
            add_group("w6-mixed-5n-heavy", "chaos", 5, "mixed", s4, 18000, true, false, true, true, false, true, false, false, false, true, false);
        }
        else if (suite == "hunt7")
        {
            /// Wave 7: 80 seeds targeting brand-new NuRaft backpressure (`bpon`/`bpoff`,
            /// `max_uncommitted_log_entries_`, `slow_member_backpressure_*`, `max_append_size_bytes_`,
            /// `append_entries_backward_probe_throttle_threshold_`, `track_peers_sm_commit_idx_`),
            /// AND real `parallel_log_appending_ = true` streaming mode under power-loss & partitions
            /// (with large snapshot_distance to avoid Bug #2 #133552)!
            std::vector<uint64_t> s1, s2, s3, s4;
            for (uint64_t i = 0; i < 20; ++i)
            {
                s1.push_back(60001 + i);
                s2.push_back(60101 + i);
                s3.push_back(60201 + i);
                s4.push_back(60301 + i);
            }
            add_group("w7-bp-stream-3n", "chaos", 3, "mixed", s1, 18000, false, false, true, false, false, false, false, false, false, false, true, true, true);
            add_group("w7-bp-dyn-5n", "chaos", 5, "mixed", s2, 18000, true, false, true, true, false, true, false, false, false, true, true, true, false);
            add_group("w7-stream-pl-3n", "power-loss", 3, "mixed", s3, 18000, false, false, true, false, false, false, false, false, false, false, true, true, true);
            add_group("w7-stream-part-5n", "partition-storm", 5, "mixed", s4, 18000, false, false, true, false, false, false, false, false, false, false, true, true, true);
        }
        else if (suite == "hunt8")
        {
            /// Wave 8: 80 seeds targeting:
            /// 1. Differential local-read oracle (`Get`/`Exists`/`List`/`FilteredList`/`ListWithOptions`/`GetACL`/`MultiRead`)
            ///    + 30-user high-churn `ACLMap` torture + Watch-map bijection & `total_watches_count` / `ttl_paths` /
            ///    `container_paths` / `committed_ephemerals` invariants!
            /// 2. Multi-snapshot retention (`snapshots_to_keep = 3`) + torn/corrupted latest snapshot fallback on power loss!
            /// 3. 5-node LSMT-Disk differential reads + `ACLMap` + `backpressure` + `Create2`/`CheckNotExists(version)`!
            /// 4. Heavy watch/session/container-GC/snapshot-storm torture!
            std::vector<uint64_t> s1, s2, s3, s4;
            for (uint64_t i = 0; i < 20; ++i)
            {
                s1.push_back(70001 + i);
                s2.push_back(70101 + i);
                s3.push_back(70201 + i);
                s4.push_back(70301 + i);
            }
            add_group("w8-acl-read-3n", "chaos", 3, "mixed", s1, 18000, true, false, true, false, false, false, false, false, false, false, true, false, false, true, false);
            add_group("w8-torn-snap-3n", "power-loss", 3, "mixed", s2, 18000, true, false, true, false, false, false, false, false, false, false, true, false, false, false, true);
            add_group("w8-lsmt-read-5n", "chaos", 5, "lsmt-disk", s3, 18000, true, false, true, false, false, false, false, false, false, false, true, true, false, true, false);
            add_group("w8-watch-snap-3n", "snapshot-storm", 3, "mixed", s4, 18000, true, false, true, false, false, false, false, false, false, false, true, true, false, true, false);
        }
        else if (suite == "hunt9")
        {
            /// Wave 9: 80 seeds hammering 4 brand-new surfaces:
            /// 1. NuRaft `auto_forwarding_ = true` (submitting writes + `set_priority` directly to FOLLOWERS!)
            /// 2. NuRaft `use_full_consensus_among_healthy_members_ = true` + `grace_period_of_lagging_state_machine_`!
            /// 3. Hot-key 2-path overlapping uncommitted mutation + partial Raft rollback torture (Mem vs LSMT-Mem vs LSMT-Disk)!
            /// 4. Watch semantic oracle (`CheckWatch` per-session leak, `SetWatches` `Event::CHANGED` vs `Event::CHILD`)!
            std::vector<uint64_t> s1, s2, s3, s4;
            for (uint64_t i = 0; i < 20; ++i)
            {
                s1.push_back(80001 + i);
                s2.push_back(80101 + i);
                s3.push_back(80201 + i);
            }
            for (uint64_t i = 0; i < 10; ++i)
                s4.push_back(80301 + i);
            add_group("w9-autofwd-3n", "chaos", 3, "mixed", s1, 18000, true, false, true, false, false, false, false, false, false, false, true, false, true, false, false, true, false, false, false);
            add_group("w9-fullcons-5n", "partition-storm", 5, "mixed", s2, 18000, true, false, true, true, false, false, false, false, false, true, true, false, true, false, false, false, true, false, false);
            add_group("w9-rollback-hot-3n", "partition-storm", 3, "mixed", s3, 18000, false, false, true, false, false, false, false, false, false, false, true, false, true, true, false, false, false, true, false);
            add_group("w9-watch-oracle-3n", "clean", 3, "mixed", s4, 10000, true, false, true, false, false, false, false, false, false, false, true, false, true, false, false, false, false, false, true);
        }
        else if (suite == "hunt10")
        {
            /// Wave 10: 80 seeds hammering 4 completely untouched surfaces:
            /// 1. Remote/Encrypted snapshot disks (`DiskEncrypted`) + dual-disk migration (`latest_snapshot_disk` -> `snapshot_disk`)
            ///    + `RemoteSnapshotLoader` + `StreamingSnapshotLoader` (`snapshot_transfer_remote_disk_streaming`)!
            /// 2. Encrypted `log_disk` (`DiskEncrypted` + `Changelog` `!isLocalDisk()`) + encrypted snapshots under power-loss!
            /// 3. NuRaft `client_req_stream` (`open_client_req_stream`) pipelined follower/leader forwarding + backpressure!
            /// 4. Hot `update_params` + directed `request_leadership(successor)` + `set_user_ctx` + 5-node dynamic membership!
            std::vector<uint64_t> s1, s2, s3, s4;
            for (uint64_t i = 0; i < 20; ++i)
            {
                s1.push_back(90001 + i);
                s2.push_back(90101 + i);
                s3.push_back(90201 + i);
                s4.push_back(90301 + i);
            }
            add_group("w10-remote-snap-3n", "snapshot-storm", 3, "mixed", s1, 18000, true, false, true, false, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, true, false, false, false);
            add_group("w10-enc-logs-3n", "power-loss", 3, "mixed", s2, 18000, true, false, true, false, false, false, false, false, false, false, true, false, false, false, false, false, false, false, false, true, true, false, false);
            add_group("w10-cli-stream-3n", "chaos", 3, "mixed", s3, 18000, true, false, true, false, false, false, false, false, false, false, true, true, true, false, false, false, false, false, false, false, false, true, false);
            add_group("w10-hot-reconfig-5n", "chaos", 5, "mixed", s4, 18000, true, false, true, true, false, false, false, false, false, true, true, true, false, false, false, false, false, false, false, false, false, true, true);
        }
        else if (suite == "hunt11")
        {
            /// Wave 11: 80 seeds hammering 4 brand-new production surfaces:
            /// 1. `create_snapshot_on_exit` (`KeeperServer.cpp:880-885`: `create_snapshot(serialize_commit_=true)`
            ///    in `Phase::SHUTDOWN` after `raft_instance->shutdown()`) + frequent graceful restarts!
            /// 2. Core-voter membership rotation (`{1,2,3} <-> {1,2,3,4}` removing original core voters &
            ///    old leaders + stale-disk rejoin after removal, with `use_new_joiner_type=true`)!
            /// 3. Remote/Encrypted snapshot transient READ + MID-STREAM READ + WRITE + REMOVE faults
            ///    (`RemoteSnapshotLoader` / `StreamingSnapshotLoader` / dual-disk `moveSnapshotCandidate`)!
            /// 4. Deep 5-level tree + shuffled recursive `ListWithOptions` + `ListRecursive` + `RemoveRecursive`
            ///    + `CreateTTL` + `CreateContainer` + 5-node LSMT-Disk stress + `DataDisk` faults + `snapshot_on_exit`!
            std::vector<uint64_t> s1, s2, s3, s4;
            for (uint64_t i = 0; i < 20; ++i)
            {
                s1.push_back(100001 + i);
                s2.push_back(100101 + i);
                s3.push_back(100201 + i);
                s4.push_back(100301 + i);
            }
            add_group("w11-snap-on-exit-3n", "chaos", 3, "mixed", s1, 18000, true, false, true, false, false, false, false, false, false, false, true, false, false, false, false, false, false, false, false, false, false, false, false, true, false, false, false);
            add_group("w11-member-rotate-3n", "chaos", 3, "mixed", s2, 18000, true, false, true, false, false, false, false, false, false, true, true, false, false, false, false, false, false, false, false, false, false, false, false, false, true, false, false);
            add_group("w11-remote-snap-faults-3n", "snapshot-storm", 3, "mixed", s3, 18000, true, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true, false, false, false, false, false, true, false);
            add_group("w11-deep-lsmt-5n", "chaos", 5, "lsmt-disk", s4, 18000, true, false, true, false, false, false, false, false, false, false, false, true, false, true, false, false, false, false, false, false, false, false, false, true, false, false, true);
        }
        else if (suite == "hunt12")
        {
            /// Wave 12: 80 seeds hammering 4 completely untouched surfaces:
            /// 1. `quorum_reads` (ALL 13 read & watch ops through Raft consensus!) + `use_xid_64` (>32-bit XIDs)
            ///    + `OpenTelemetry::TracingContext` serialization + `min_request_size_for_cache = 32`!
            /// 2. `Changelog` `max_size` + `overallocate_size` + byte-bounded `latest_logs_cache_size_threshold`
            ///    + `max_flush_batch_size > 1` under power-loss & torn writes!
            /// 3. NuRaft `set_priority_v2` (leader & forwarded) + `set_priority(0, broadcast)` + `flip_learner_flag`
            ///    + `update_srv_config` + `auto_forward` + `hot_reconfig` on 5-node partition storms!
            /// 4. `parsed_batch_cache` (`min_request_size_for_cache = 32`) with client retries / reused `(sid, xid)`
            ///    across leader failovers and variable-size v1 batches!
            std::vector<uint64_t> s1, s2, s3, s4;
            for (uint64_t i = 0; i < 20; ++i)
            {
                s1.push_back(110001 + i);
                s2.push_back(110101 + i);
                s3.push_back(110201 + i);
                s4.push_back(110301 + i);
            }
            add_group("w12-quorum-reads-3n", "chaos", 3, "mixed", s1, 18000, true, false, true, false, false, false, false, false, false, false, true, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true, false, false, false);
            add_group("w12-log-size-cache-3n", "power-loss", 3, "mixed", s2, 18000, true, false, true, false, false, false, false, false, false, false, true, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true, false, false);
            add_group("w12-prio-v2-5n", "partition-storm", 5, "mixed", s3, 18000, true, false, true, false, false, false, false, false, false, false, true, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true, false);
            add_group("w12-batch-cache-3n", "partition-storm", 3, "mixed", s4, 18000, true, false, true, false, false, false, false, false, false, false, true, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true);
        }
        else if (suite == "hunt13")
        {
            /// Wave 13: 80 seeds hammering 4 brand-new production surfaces:
            /// 1. Real `DB::DiskLocal` logs + snapshots (`isLocalDisk() == true`): FIRST-TIME coverage of
            ///    `KeeperSnapshotManager::writeSnapshotFile` (streaming snapshot writer), `fallocate`, `ftruncate`,
            ///    `getSizeFromFileDescriptor`, AND byte+count bounded `latest_logs_cache` + `ValidRuns` + `executeReadPlan`!
            /// 2. NuRaft concurrent reconfiguration race (`reconfig-race`, 5n chaos, NO `auto_forward`):
            ///    `add_srv` / `remove_srv` / `flip_learner_flag` / `set_priority_v2` / `update_srv_config` / `set_user_ctx`!
            /// 3. `parsed_batch_cache` (`min_request_size_for_cache = 32`) + mixed v0/v1 rolling upgrades + `quorum_reads`
            ///    + `snapshot-storm` WITHOUT `xid` reuse!
            /// 4. 5-node LSMT-Disk + real `DiskLocal` streaming snapshots (`SnapshotIterator`) + deep 5-level trees + `quorum_reads`!
            std::vector<uint64_t> s1, s2, s3, s4;
            for (uint64_t i = 0; i < 20; ++i)
            {
                s1.push_back(120001 + i);
                s2.push_back(120101 + i);
                s3.push_back(120201 + i);
                s4.push_back(120301 + i);
            }
            add_group("w13-local-disk-cache-3n", "chaos", 3, "mixed", s1, 18000, true, false, false, false, false, false, false, false, false, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true, false, false);
            add_group("w13-reconfig-race-5n", "chaos", 5, "mixed", s2, 18000, true, false, true, false, false, false, false, false, false, true, true, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true, false);
            add_group("w13-batch-cache-snap-3n", "snapshot-storm", 3, "mixed", s3, 18000, true, false, true, false, false, false, false, false, false, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true);
            add_group("w13-deep-lsmt-local-5n", "chaos", 5, "lsmt-disk", s4, 18000, true, false, false, false, false, false, false, false, false, false, true, true, false, true, false, false, false, false, false, false, false, false, false, true, false, false, true, true, false, false, false, true, false, false);
        }
        else if (suite == "hunt14")
        {
            /// Wave 14: 60 seeds targeting:
            /// 1. NuRaft concurrent reconfiguration overlap (`update_srv_config`/`set_user_ctx` while `add_srv`
            ///    or `flip_learner_flag` is uncommitted!) + `NURAFT_CONFIG_ORACLE` checking silent config reversion!
            /// 2. Torn trailing changelog recovery (`broken_at_end = true`) + leader failover `Changelog::writeAt`!
            /// 3. Real `DB::DiskLocal` streaming snapshots (`writeSnapshotFile`), `fallocate`, `ftruncate`,
            ///    byte+count bounded `latest_logs_cache`, `ValidRuns`, `executeReadPlan` with `large_snap_dist`!
            std::vector<uint64_t> s1, s2, s3;
            for (uint64_t i = 0; i < 20; ++i)
            {
                s1.push_back(130001 + i);
                s2.push_back(130101 + i);
                s3.push_back(130201 + i);
            }
            add_group("w14-reconfig-overlap-5n", "chaos", 5, "mixed", s1, 18000, true, false, true, false, false, false, false, false, false, true, true, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true, false);
            add_group("w14-broken-tail-3n", "power-loss", 3, "mixed", s2, 18000, true, false, true, false, false, false, false, false, false, false, true, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true);
            add_group("w14-local-disk-nocomp-3n", "chaos", 3, "mixed", s3, 18000, true, false, false, false, false, false, false, false, false, false, true, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true, false, false, false, false);
        }
        else if (suite == "hunt15")
        {
            /// Wave 15: 80 seeds exercising exact production `KeeperServer::callbackFunc` lazy startup
            /// preprocessing (`!localLogsPreprocessed()`) across crashes, power-loss rollbacks,
            /// snapshot-storm `apply_snapshot` while `!localLogsPreprocessed()`, and 5-node LSMT-Disk!
            std::vector<uint64_t> s1, s2, s3, s4;
            for (uint64_t i = 0; i < 20; ++i)
            {
                s1.push_back(140001 + i);
                s2.push_back(140101 + i);
                s3.push_back(140201 + i);
                s4.push_back(140301 + i);
            }
            add_group("w15-lazy-chaos-3n", "chaos", 3, "mixed", s1, 18000, true, false, true, false, false, false, false, false, false, false, true, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true);
            add_group("w15-lazy-pl-3n", "power-loss", 3, "mixed", s2, 18000, true, false, true, false, false, false, false, false, false, false, true, false, true, false, false, false, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true);
            add_group("w15-lazy-snap-3n", "snapshot-storm", 3, "mixed", s3, 18000, true, false, true, false, false, false, false, false, false, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true);
            add_group("w15-lazy-lsmt-5n", "chaos", 5, "lsmt-disk", s4, 18000, true, false, true, false, false, false, false, false, false, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true, false, false, false, false, false, false, false, false, true);
        }
        else if (suite == "hunt16")
        {
            /// Wave 16: 80 seeds targeting:
            /// 1. NuRaft Raft Election Safety (`handle_vote_req` `last_entry()->get_term()` vs `term_for_log`)
            ///    after `InstallSnapshot` + log compaction!
            /// 2. Huge LSMT single values (2-10 KB > block/file/memtable/cache thresholds) + `nodes_count` oracle!
            /// 3. Same-path complex `Multi` churn (Create->Set->Remove->Recreate(eph/TTL/container) + forced rollbacks)
            ///    under partition-storm + rollback torture!
            /// 4. Cross-version snapshot matrix (V5/V6/V7/V8/V9 + Zstd/LZ4 + ACLs + ephemerals + chunked transfer)!
            std::vector<uint64_t> s1, s2, s3, s4;
            for (uint64_t i = 0; i < 20; ++i)
            {
                s1.push_back(150001 + i);
                s2.push_back(150101 + i);
                s3.push_back(150201 + i);
                s4.push_back(150301 + i);
            }
            add_group("w16-snap-vote-safety-3n", "chaos", 3, "mixed", s1, 16000, true, false, true, false, false, false, false, false, false, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true, false, false, false);
            add_group("w16-huge-lsmt-3n", "chaos", 3, "lsmt-disk", s2, 18000, true, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true, false, false);
            add_group("w16-same-path-multi-3n", "partition-storm", 3, "mixed", s3, 18000, true, false, true, false, false, false, false, false, false, false, true, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true, false);
            add_group("w16-snap-versions-3n", "snapshot-storm", 3, "mixed", s4, 18000, true, false, true, false, false, false, false, false, false, false, true, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, false, true);
        }
        else if (suite == "hunt17")
        {
            /// Wave 17: 80 seeds targeting:
            /// 1. NuRaft `send_msg_to_leader` (`auto_forward` + `member_rotation`) null `srv_conf` SIGSEGV!
            /// 2. `priority_torture` + `member_rotation` overlapping config changes & `become_leader`!
            /// 3. 5-node `client_stream` + `member_rotation` + `backpressure` under `partition-storm`!
            /// 4. 5-node `lsmt-disk` + `huge_lsmt_values` + `same_path_multi_churn` + `snapshot_on_exit` under `power-loss`!
            for (uint64_t i = 0; i < 20; ++i)
            {
                JobSpec j1;
                j1.name = "w17-autofwd-reconfig-3n_s" + std::to_string(160001 + i);
                j1.seed = 160001 + i;
                j1.nodes = 3;
                j1.profile = "chaos";
                j1.storage = "mixed";
                j1.duration_ms = 18000;
                j1.no_streaming = true;
                j1.no_bounded_cache = true;
                j1.no_disk_faults = true;
                j1.large_snap_dist = true;
                j1.force_new_joiner = true;
                j1.auto_forward = true;
                j1.member_rotation = true;
                jobs.push_back(std::move(j1));

                JobSpec j2;
                j2.name = "w17-priority-conf-overlap-3n_s" + std::to_string(160101 + i);
                j2.seed = 160101 + i;
                j2.nodes = 3;
                j2.profile = "chaos";
                j2.storage = "mixed";
                j2.duration_ms = 18000;
                j2.no_streaming = true;
                j2.no_bounded_cache = true;
                j2.no_disk_faults = true;
                j2.large_snap_dist = true;
                j2.force_new_joiner = true;
                j2.priority_torture = true;
                j2.member_rotation = true;
                jobs.push_back(std::move(j2));

                JobSpec j3;
                j3.name = "w17-clistream-rotate-5n_s" + std::to_string(160201 + i);
                j3.seed = 160201 + i;
                j3.nodes = 5;
                j3.profile = "partition-storm";
                j3.storage = "mixed";
                j3.duration_ms = 18000;
                j3.no_streaming = true;
                j3.no_bounded_cache = true;
                j3.no_disk_faults = true;
                j3.large_snap_dist = true;
                j3.force_new_joiner = true;
                j3.client_stream = true;
                j3.member_rotation = true;
                j3.backpressure = true;
                jobs.push_back(std::move(j3));

                JobSpec j4;
                j4.name = "w17-5n-heavy-lsmt-disk_s" + std::to_string(160301 + i);
                j4.seed = 160301 + i;
                j4.nodes = 5;
                j4.profile = "power-loss";
                j4.storage = "lsmt-disk";
                j4.duration_ms = 18000;
                j4.no_streaming = true;
                j4.no_bounded_cache = true;
                j4.huge_lsmt_values = true;
                j4.same_path_multi_churn = true;
                j4.snapshot_on_exit = true;
                jobs.push_back(std::move(j4));
            }
        }
        else
        {
            throw std::runtime_error("Unknown swarm suite: " + suite + " (expected quick, full, hunt..hunt17)");
        }
        return jobs;
    }

    static int run(
        const std::string & self_exe,
        const std::string & suite,
        const std::filesystem::path & output_dir,
        size_t max_workers,
        bool strict_raft_commit)
    {
        std::filesystem::create_directories(output_dir);
        const auto work_dir = output_dir / "work";
        std::filesystem::create_directories(work_dir);

        const auto t_start = std::chrono::steady_clock::now();

        /// 1. Run instrumentation control in an isolated child process.
        {
            auto ctrl = runOneSubprocess(self_exe, {"--instrumentation-control"}, JobSpec{"instrumentation-control"}, work_dir);
            if (ctrl.exit_code != 0 || ctrl.stdout_text.find("PASS keeper-sim-instrumentation-control") == std::string::npos)
            {
                std::cerr << "[FAIL] instrumentation-control: " << ctrl.stderr_text << '\n';
                return 1;
            }
            std::cout << "[PASS] instrumentation-control\n" << std::flush;
        }

        /// 2. Run negative safety control: unsafe quorum=1 under partition-storm MUST fail!
        {
            JobSpec neg;
            neg.name = "negative-unsafe-quorum-control";
            neg.seed = 777;
            neg.nodes = 3;
            neg.profile = "partition-storm";
            neg.storage = "mem";
            neg.duration_ms = 12000;
            neg.unsafe_quorum_control = true;
            neg.expect_failure = true;
            auto res = runJob(self_exe, neg, work_dir);
            if (!res.passed)
            {
                std::cerr << "[FAIL] negative-unsafe-quorum-control unexpectedly passed!\n";
                return 1;
            }
            std::cout << "[PASS] negative-unsafe-quorum-control (oracle rejected unsafe quorum)\n" << std::flush;
        }

        /// 3. Run all campaign jobs in parallel across `max_workers` child processes.
        auto jobs = buildSuite(suite);
        for (auto & j : jobs)
            j.strict_raft_commit = strict_raft_commit;

        std::vector<JobResult> results = runParallel(self_exe, jobs, work_dir, std::max<size_t>(1, max_workers));

        /// 4. Determinism verification: pick up to 3 passing runs across distinct profiles/storages
        /// and re-run with `--host-delay`, asserting byte-identical `trace_hash` and `final_digest`.
        std::vector<JobSpec> det_jobs;
        std::vector<const JobResult *> det_originals;
        for (const auto & r : results)
        {
            if (r.passed && !r.trace_hash.empty() && det_jobs.size() < 3)
            {
                JobSpec replay = r.spec;
                replay.name = "det_" + r.spec.name;
                replay.host_delay = true;
                det_jobs.push_back(replay);
                det_originals.push_back(&r);
            }
        }
        auto det_results = runParallel(self_exe, det_jobs, work_dir, std::max<size_t>(1, max_workers));
        size_t det_passed = 0;
        for (size_t i = 0; i < det_results.size(); ++i)
        {
            const auto & orig = *det_originals[i];
            const auto & rep = det_results[i];
            const bool same = rep.passed
                && rep.trace_hash == orig.trace_hash
                && rep.executed_events == orig.executed_events
                && rep.final_commit_index == orig.final_commit_index
                && rep.final_digest == orig.final_digest;
            if (same)
                ++det_passed;
            std::cout << "[" << (same ? "PASS" : "FAIL") << "] determinism " << orig.spec.name
                      << " orig=" << orig.trace_hash << " replay=" << rep.trace_hash << '\n' << std::flush;
        }

        /// 5. Aggregate SOMETIMES coverage & totals.
        size_t passed_count = 0;
        std::map<std::string, uint64_t> total_coverage;
        uint64_t total_events = 0;
        for (const auto & r : results)
        {
            if (r.passed)
            {
                ++passed_count;
                total_events += r.executed_events;
                for (const auto & [k, v] : r.coverage)
                    total_coverage[k] += v;
            }
        }

        const double total_elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - t_start).count();
        const bool all_ok = (passed_count == results.size()) && (det_passed == det_results.size());

        std::cout << "\n=== Keeper Deterministic Simulation Swarm Report ===\n";
        std::cout << "Verdict: " << (all_ok ? "PASS" : "FAIL")
                  << " (" << passed_count << "/" << results.size() << " seeds passed, "
                  << det_passed << "/" << det_results.size() << " determinism checks passed, "
                  << total_events << " total virtual events, "
                  << std::fixed << std::setprecision(2) << total_elapsed << "s)\n";
        std::cout << "SOMETIMES Coverage:\n";
        for (const auto & [k, v] : total_coverage)
            std::cout << "  " << std::left << std::setw(36) << k << ": " << v << '\n';

        if (!all_ok)
        {
            std::cout << "\nFailures & Repro Commands:\n";
            for (const auto & r : results)
            {
                if (!r.passed)
                {
                    std::cout << "  FAIL " << r.spec.name
                              << " (exit=" << r.exit_code << ", sig=" << r.term_signal << ")\n"
                              << "    repro: " << r.command << '\n'
                              << "    error: " << firstLine(r.stderr_text.empty() ? r.stdout_text : r.stderr_text) << '\n';
                }
            }
        }

        return all_ok ? 0 : 1;
    }

private:
    static std::string firstLine(const std::string & s)
    {
        auto pos = s.find('\n');
        return pos == std::string::npos ? s : s.substr(0, pos);
    }

    static std::string extractJsonString(const std::string & json, const std::string & key)
    {
        const std::string needle = "\"" + key + "\":\"";
        const auto pos = json.find(needle);
        if (pos == std::string::npos)
            return "";
        const auto start = pos + needle.size();
        const auto end = json.find('"', start);
        return end == std::string::npos ? "" : json.substr(start, end - start);
    }

    static uint64_t extractJsonU64(const std::string & json, const std::string & key)
    {
        const std::string needle = "\"" + key + "\":";
        const auto pos = json.find(needle);
        if (pos == std::string::npos)
            return 0;
        const auto start = pos + needle.size();
        size_t end = start;
        while (end < json.size() && json[end] >= '0' && json[end] <= '9')
            ++end;
        return end > start ? std::stoull(json.substr(start, end - start)) : 0;
    }

    static std::map<std::string, uint64_t> extractCoverageMap(const std::string & json)
    {
        std::map<std::string, uint64_t> out;
        const std::string needle = "\"coverage\":{";
        const auto pos = json.find(needle);
        if (pos == std::string::npos)
            return out;
        size_t i = pos + needle.size();
        const auto close = json.find('}', i);
        if (close == std::string::npos)
            return out;
        while (i < close)
        {
            const auto q1 = json.find('"', i);
            if (q1 == std::string::npos || q1 >= close)
                break;
            const auto q2 = json.find('"', q1 + 1);
            if (q2 == std::string::npos || q2 >= close)
                break;
            const std::string key = json.substr(q1 + 1, q2 - q1 - 1);
            const auto colon = json.find(':', q2 + 1);
            if (colon == std::string::npos || colon >= close)
                break;
            size_t num_start = colon + 1;
            size_t num_end = num_start;
            while (num_end < close && json[num_end] >= '0' && json[num_end] <= '9')
                ++num_end;
            if (num_end > num_start)
                out[key] = std::stoull(json.substr(num_start, num_end - num_start));
            i = num_end + 1;
        }
        return out;
    }

    static std::vector<std::string> buildArgs(const JobSpec & spec, const std::filesystem::path & state_dir)
    {
        std::vector<std::string> args = {
            "--seed", std::to_string(spec.seed),
            "--nodes", std::to_string(spec.nodes),
            "--profile", spec.profile,
            "--duration-ms", std::to_string(spec.duration_ms),
            "--state-dir", state_dir.string(),
            "--json-summary",
        };
        if (spec.storage == "lsmt-mem")
            args.push_back("--lsmt");
        else if (spec.storage == "lsmt-disk")
            args.push_back("--lsmt-disk");
        else if (spec.storage == "mixed")
            args.push_back("--mixed-storage");
        if (spec.no_streaming)
            args.push_back("--no-streaming");
        if (spec.compress_logs)
            args.push_back("--compress-logs");
        if (spec.no_bounded_cache)
            args.push_back("--no-bounded-cache");
        if (spec.dynamic_membership)
            args.push_back("--dynamic-membership");
        if (spec.late_session_race)
            args.push_back("--late-session-race");
        if (spec.snapshot_torture)
            args.push_back("--snapshot-torture");
        if (spec.mixed_versions)
            args.push_back("--mixed-versions");
        if (spec.async_replication)
            args.push_back("--async-replication");
        if (spec.leadership_expiry)
            args.push_back("--leadership-expiry");
        if (spec.force_new_joiner)
            args.push_back("--force-new-joiner");
        if (spec.no_disk_faults)
            args.push_back("--no-disk-faults");
        if (spec.backpressure)
            args.push_back("--backpressure");
        if (spec.large_snap_dist)
            args.push_back("--large-snap-dist");
        if (spec.acl_torture)
            args.push_back("--acl-torture");
        if (spec.torn_snapshots)
            args.push_back("--torn-snapshots");
        if (spec.auto_forward)
            args.push_back("--auto-forward");
        if (spec.full_consensus)
            args.push_back("--full-consensus");
        if (spec.rollback_torture)
            args.push_back("--rollback-torture");
        if (spec.watch_oracle)
            args.push_back("--watch-oracle");
        if (spec.remote_snapshots)
            args.push_back("--remote-snapshots");
        if (spec.encrypted_logs)
            args.push_back("--encrypted-logs");
        if (spec.client_stream)
            args.push_back("--client-stream");
        if (spec.hot_reconfig)
            args.push_back("--hot-reconfig");
        if (spec.snapshot_on_exit)
            args.push_back("--snapshot-on-exit");
        if (spec.member_rotation)
            args.push_back("--member-rotation");
        if (spec.remote_snap_faults)
            args.push_back("--remote-snap-faults");
        if (spec.deep_lsmt_stress)
            args.push_back("--deep-lsmt-stress");
        if (spec.quorum_reads)
            args.push_back("--quorum-reads");
        if (spec.log_size_cache)
            args.push_back("--log-size-cache");
        if (spec.priority_torture)
            args.push_back("--priority-torture");
        if (spec.batch_cache_retry)
            args.push_back("--batch-cache-retry");
        if (spec.local_disk_cache)
            args.push_back("--local-disk-cache");
        if (spec.reconfig_race)
            args.push_back("--reconfig-race");
        if (spec.batch_cache_snapshot)
            args.push_back("--batch-cache-snapshot");
        if (spec.reconfig_overlap)
            args.push_back("--reconfig-overlap");
        if (spec.broken_tail_writeat)
            args.push_back("--broken-tail-writeat");
        if (spec.lazy_startup)
            args.push_back("--lazy-startup");
        if (spec.snapshot_vote_safety)
            args.push_back("--snapshot-vote-safety");
        if (spec.huge_lsmt_values)
            args.push_back("--huge-lsmt-values");
        if (spec.same_path_multi_churn)
            args.push_back("--same-path-multi-churn");
        if (spec.snapshot_versions_mix)
            args.push_back("--snapshot-versions-mix");
        if (spec.host_delay)
            args.push_back("--host-delay");
        if (spec.strict_raft_commit)
            args.push_back("--strict-raft-commit");
        if (spec.unsafe_quorum_control)
            args.push_back("--unsafe-quorum-control");
        return args;
    }

    static JobResult runOneSubprocess(
        const std::string & self_exe,
        const std::vector<std::string> & extra_args,
        const JobSpec & spec,
        const std::filesystem::path & work_dir)
    {
        JobResult res;
        res.spec = spec;
        const auto t0 = std::chrono::steady_clock::now();

        std::vector<std::string> argv_storage;
        argv_storage.push_back(self_exe);
        for (const auto & a : extra_args)
            argv_storage.push_back(a);
        std::ostringstream cmd_ss;
        for (size_t i = 0; i < argv_storage.size(); ++i)
            cmd_ss << (i ? " " : "") << argv_storage[i];
        res.command = cmd_ss.str();

        int out_pipe[2]{-1, -1};
        int err_pipe[2]{-1, -1};
        if (pipe(out_pipe) != 0 || pipe(err_pipe) != 0)
            throw std::runtime_error("SimSwarmRunner: pipe() failed");

        const pid_t pid = fork();
        if (pid < 0)
            throw std::runtime_error("SimSwarmRunner: fork() failed");

        if (pid == 0)
        {
            dup2(out_pipe[1], STDOUT_FILENO);
            dup2(err_pipe[1], STDERR_FILENO);
            close(out_pipe[0]);
            close(out_pipe[1]);
            close(err_pipe[0]);
            close(err_pipe[1]);

            std::vector<char *> c_argv;
            c_argv.reserve(argv_storage.size() + 1);
            for (auto & s : argv_storage)
                c_argv.push_back(s.data());
            c_argv.push_back(nullptr);
            execv(self_exe.c_str(), c_argv.data());
            _exit(127);
        }

        close(out_pipe[1]);
        close(err_pipe[1]);

        auto read_all = [](int fd) -> std::string
        {
            std::string data;
            char buf[4096];
            for (;;)
            {
                const ssize_t n = read(fd, buf, sizeof(buf));
                if (n <= 0)
                    break;
                data.append(buf, static_cast<size_t>(n));
            }
            close(fd);
            return data;
        };
        res.stdout_text = read_all(out_pipe[0]);
        res.stderr_text = read_all(err_pipe[0]);

        int status = 0;
        waitpid(pid, &status, 0);
        res.elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        if (WIFEXITED(status))
        {
            res.exit_code = WEXITSTATUS(status);
        }
        else if (WIFSIGNALED(status))
        {
            res.signalled = true;
            res.term_signal = WTERMSIG(status);
            res.exit_code = 128 + res.term_signal;
        }
        (void)work_dir;
        return res;
    }

    static JobResult runJob(
        const std::string & self_exe,
        const JobSpec & spec,
        const std::filesystem::path & work_dir)
    {
        const auto state_dir = work_dir / ("state_" + spec.name);
        auto res = runOneSubprocess(self_exe, buildArgs(spec, state_dir), spec, work_dir);
        std::error_code ec;
        std::filesystem::remove_all(state_dir, ec);

        if (spec.expect_failure)
        {
            res.passed = (res.exit_code != 0);
            return res;
        }

        if (res.exit_code == 0 && res.stdout_text.find("\"verdict\":\"PASS\"") != std::string::npos)
        {
            res.passed = true;
            res.trace_hash = extractJsonString(res.stdout_text, "trace_hash");
            res.executed_events = extractJsonU64(res.stdout_text, "executed_events");
            res.final_commit_index = extractJsonU64(res.stdout_text, "final_commit_index");
            res.final_digest = extractJsonU64(res.stdout_text, "final_digest");
            res.coverage = extractCoverageMap(res.stdout_text);
        }
        return res;
    }

    struct ActiveChild
    {
        pid_t pid = -1;
        size_t job_idx = 0;
        std::filesystem::path state_dir;
        std::filesystem::path out_file;
        std::filesystem::path err_file;
        std::string command;
        std::chrono::steady_clock::time_point started;
    };

    static std::vector<JobResult> runParallel(
        const std::string & self_exe,
        const std::vector<JobSpec> & jobs,
        const std::filesystem::path & work_dir,
        size_t max_workers)
    {
        std::vector<JobResult> results(jobs.size());
        std::map<pid_t, ActiveChild> active;
        size_t next_idx = 0;

        auto launch = [&](size_t idx)
        {
            const auto & spec = jobs[idx];
            const auto state_dir = work_dir / ("state_" + spec.name);
            const auto out_file = work_dir / ("out_" + spec.name + ".log");
            const auto err_file = work_dir / ("err_" + spec.name + ".log");
            auto args = buildArgs(spec, state_dir);

            std::vector<std::string> argv_storage;
            argv_storage.push_back(self_exe);
            for (const auto & a : args)
                argv_storage.push_back(a);
            std::ostringstream cmd_ss;
            for (size_t i = 0; i < argv_storage.size(); ++i)
                cmd_ss << (i ? " " : "") << argv_storage[i];

            const pid_t pid = fork();
            if (pid < 0)
                throw std::runtime_error("SimSwarmRunner: fork() failed");
            if (pid == 0)
            {
                FILE * fo = fopen(out_file.c_str(), "w");
                FILE * fe = fopen(err_file.c_str(), "w");
                if (fo)
                {
                    dup2(fileno(fo), STDOUT_FILENO);
                    fclose(fo);
                }
                if (fe)
                {
                    dup2(fileno(fe), STDERR_FILENO);
                    fclose(fe);
                }
                std::vector<char *> c_argv;
                c_argv.reserve(argv_storage.size() + 1);
                for (auto & s : argv_storage)
                    c_argv.push_back(s.data());
                c_argv.push_back(nullptr);
                execv(self_exe.c_str(), c_argv.data());
                _exit(127);
            }
            active[pid] = ActiveChild{pid, idx, state_dir, out_file, err_file, cmd_ss.str(), std::chrono::steady_clock::now()};
        };

        auto read_file = [](const std::filesystem::path & p) -> std::string
        {
            std::ifstream in(p);
            return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        };

        while (next_idx < jobs.size() || !active.empty())
        {
            while (next_idx < jobs.size() && active.size() < max_workers)
                launch(next_idx++);

            int status = 0;
            const pid_t done_pid = waitpid(-1, &status, 0);
            if (done_pid <= 0)
                break;

            auto it = active.find(done_pid);
            if (it == active.end())
                continue;
            const auto child = it->second;
            active.erase(it);

            JobResult res;
            res.spec = jobs[child.job_idx];
            res.command = child.command;
            res.elapsed_s = std::chrono::duration<double>(std::chrono::steady_clock::now() - child.started).count();
            res.stdout_text = read_file(child.out_file);
            res.stderr_text = read_file(child.err_file);
            if (WIFEXITED(status))
            {
                res.exit_code = WEXITSTATUS(status);
            }
            else if (WIFSIGNALED(status))
            {
                res.signalled = true;
                res.term_signal = WTERMSIG(status);
                res.exit_code = 128 + res.term_signal;
            }

            std::error_code ec;
            std::filesystem::remove_all(child.state_dir, ec);
            std::filesystem::remove(child.out_file, ec);
            std::filesystem::remove(child.err_file, ec);

            if (res.spec.expect_failure)
            {
                res.passed = (res.exit_code != 0);
            }
            else if (res.exit_code == 0 && res.stdout_text.find("\"verdict\":\"PASS\"") != std::string::npos)
            {
                res.passed = true;
                res.trace_hash = extractJsonString(res.stdout_text, "trace_hash");
                res.executed_events = extractJsonU64(res.stdout_text, "executed_events");
                res.final_commit_index = extractJsonU64(res.stdout_text, "final_commit_index");
                res.final_digest = extractJsonU64(res.stdout_text, "final_digest");
                res.coverage = extractCoverageMap(res.stdout_text);
            }

            std::cout << "[" << (res.passed ? "PASS" : "FAIL") << "] " << res.spec.name
                      << " (" << std::fixed << std::setprecision(2) << res.elapsed_s << "s)";
            if (res.passed)
                std::cout << " events=" << res.executed_events << " idx=" << res.final_commit_index << " hash=" << res.trace_hash;
            else
                std::cout << " exit=" << res.exit_code << " " << firstLine(res.stderr_text.empty() ? res.stdout_text : res.stderr_text);
            std::cout << '\n' << std::flush;

            results[child.job_idx] = std::move(res);
        }
        return results;
    }
};

} // namespace keeper_sim
