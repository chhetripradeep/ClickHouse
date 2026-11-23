#pragma once

#include <Common/thread_local_rng.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iterator>
#include <map>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

#include <pthread.h>
#include <time.h>

namespace keeper_sim
{

inline std::atomic<size_t> g_thread_attempts{0};
inline std::atomic<size_t> g_virtual_posix_reads{0};
inline std::atomic<size_t> g_host_clock_reads{0};
inline std::array<std::atomic<size_t>, 17> g_delegated_posix_reads{};

inline bool g_virtual_time_enabled = false;
inline int64_t g_sim_time_ns = 1'000'000'000LL; // Start at 1000 ms
inline uint64_t g_sim_seed = 1;

inline int64_t simTimeMs()
{
    return g_sim_time_ns / 1'000'000LL;
}

inline void require(bool condition, const std::string & message)
{
    if (!condition)
        throw std::runtime_error(message);
}

inline size_t liveThreadCount()
{
    const auto tasks = std::filesystem::directory_iterator("/proc/self/task");
    return static_cast<size_t>(std::distance(std::filesystem::begin(tasks), std::filesystem::end(tasks)));
}

/// Deterministic PRNG wrapper inspired by FoundationDB's DeterministicRandom.
class SimRandom
{
public:
    explicit SimRandom(uint64_t seed_)
        : seed(seed_)
        , rng(seed_)
    {
    }

    uint64_t nextU64()
    {
        return rng();
    }

    uint32_t nextU32()
    {
        return static_cast<uint32_t>(rng() & 0xFFFFFFFFULL);
    }

    int64_t rangeInt(int64_t lo, int64_t hi)
    {
        require(lo <= hi, "SimRandom::rangeInt invalid bounds");
        if (lo == hi)
            return lo;
        const uint64_t span = static_cast<uint64_t>(hi - lo) + 1ULL;
        return lo + static_cast<int64_t>(rng() % span);
    }

    size_t rangeSize(size_t lo, size_t hi)
    {
        require(lo <= hi, "SimRandom::rangeSize invalid bounds");
        if (lo == hi)
            return lo;
        return lo + static_cast<size_t>(rng() % (hi - lo + 1));
    }

    double unit()
    {
        return static_cast<double>(rng() >> 11) * (1.0 / 9007199254740992.0);
    }

    bool chance(double probability)
    {
        if (probability <= 0.0)
            return false;
        if (probability >= 1.0)
            return true;
        return unit() < probability;
    }

    template <typename T>
    const T & pick(const std::vector<T> & items)
    {
        require(!items.empty(), "SimRandom::pick on empty vector");
        return items[rangeSize(0, items.size() - 1)];
    }

    uint64_t getSeed() const { return seed; }

private:
    uint64_t seed;
    std::mt19937_64 rng;
};

/// FoundationDB-inspired BUGGIFY table: per-seed deterministic perturbation switches.
/// Each switch is rolled once at seed initialization; when active, it deterministically
/// perturbs timing, batching, rotation, snapshot frequency, clogging, or fault profiles
/// to push the cluster into extreme corner cases without ever leaving valid semantics.
struct BuggifyConfig
{
    bool enabled = true;
    std::map<std::string, bool> switches;

    /// Derived extreme-but-valid cluster parameters chosen deterministically for this seed.
    uint64_t log_rotate_interval = 4;
    int32_t snapshot_distance = 18;
    int32_t max_append_size = 8;
    bool parallel_log_appending = true;
    int32_t max_log_gap_in_stream = 8;
    int32_t election_timeout_lower_ms = 150;
    int32_t election_timeout_upper_ms = 300;
    int32_t heartbeat_interval_ms = 50;
    uint64_t lsmt_memtable_size = 2048;
    uint64_t lsmt_block_size = 512;
    uint64_t lsmt_min_files_to_merge = 2;
    uint64_t lsmt_block_cache_size = 4096;
    uint64_t lsmt_file_block_size = 4096;
    uint64_t lsmt_block_group_compressed_size = 4096;
    uint64_t lsmt_sorted_file_uncompressed_size = 16384;
    uint64_t write_log_entry_version = 0;
    bool compress_logs = false;
    bool compress_snapshots_zstd = true;
    bool multi_request_batches = false;
    uint64_t latest_logs_cache_entry_count_threshold = 0;
    uint64_t snapshot_transfer_chunk_size = 0;
    uint64_t write_snapshot_version = 9;
    bool force_sync_logs = true;
    int32_t log_sync_stop_gap = 2;
    bool use_new_joiner_type = false;
    bool enable_dynamic_membership = false;
    bool enable_late_session_race = false;
    bool snapshot_torture = false;
    bool mixed_versions = false;
    bool async_replication = false;
    bool large_values = false;
    bool force_new_joiner = false;
    bool no_disk_faults = false;
    bool enable_backpressure = false;
    bool large_snapshot_distance = false;
    bool acl_torture = false;
    bool torn_snapshots = false;
    bool auto_forward = false;
    bool full_consensus = false;
    bool rollback_torture = false;
    bool watch_oracle = false;
    int32_t grace_period_lagging_sm_ms = 0;
    uint64_t snapshots_to_keep = 3;
    uint64_t container_gc_max_never_used_ms = 0;
    uint64_t max_uncommitted_log_entries = 0;
    uint64_t slow_member_backpressure_max_uncommitted = 0;
    int32_t slow_member_backpressure_no_progress_timeout_ms = 0;
    uint64_t max_append_size_bytes = 0;
    int32_t backward_probe_throttle_threshold = 5;
    bool track_peers_sm_commit_idx = false;
    int32_t leadership_expiry_ms = 0;

    void initialize(
        SimRandom & rng,
        bool enable_buggify,
        const std::string & profile,
        bool disable_streaming = false,
        bool force_compress_logs = false,
        bool disable_bounded_cache = false,
        bool dynamic_membership = false,
        bool late_session_race = false,
        bool enable_snapshot_torture = false,
        bool enable_mixed_versions = false,
        bool enable_async_replication = false,
        bool enable_leadership_expiry = false,
        bool force_new_joiner_ = false,
        bool disable_disk_faults = false,
        bool backpressure = false,
        bool large_snap_dist = false,
        bool enable_acl_torture = false,
        bool enable_torn_snapshots = false,
        bool enable_auto_forward = false,
        bool enable_full_consensus = false,
        bool enable_rollback_torture = false,
        bool enable_watch_oracle = false)
    {
        enabled = enable_buggify;
        switches.clear();

        auto roll = [&](const std::string & name, double prob) -> bool
        {
            const bool active = enabled && rng.chance(prob);
            switches[name] = active;
            return active;
        };

        if (roll("tiny_log_rotation", 0.55))
            log_rotate_interval = static_cast<uint64_t>(rng.rangeInt(1, 3));
        else
            log_rotate_interval = static_cast<uint64_t>(rng.rangeInt(4, 10));

        if (large_snap_dist)
            snapshot_distance = 500;
        else if (profile == "snapshot-storm" || roll("frequent_snapshots", 0.60))
            snapshot_distance = static_cast<int32_t>(rng.rangeInt(6, 16));
        else
            snapshot_distance = static_cast<int32_t>(rng.rangeInt(18, 40));

        if (profile == "figure8-hunt" || roll("small_append_batch", 0.45))
            max_append_size = static_cast<int32_t>(rng.rangeInt(1, 3));
        else
            max_append_size = static_cast<int32_t>(rng.rangeInt(4, 16));

        if (disable_streaming)
        {
            parallel_log_appending = false;
            max_log_gap_in_stream = 0;
        }
        else if (large_snap_dist)
        {
            parallel_log_appending = true;
            max_log_gap_in_stream = static_cast<int32_t>(rng.rangeInt(2, 8));
        }
        else
        {
            parallel_log_appending = !roll("disable_parallel_log_appending", 0.25);
            max_log_gap_in_stream = parallel_log_appending ? static_cast<int32_t>(rng.rangeInt(1, 8)) : 0;
        }

        if (roll("tight_election_timeouts", 0.35))
        {
            heartbeat_interval_ms = 35;
            election_timeout_lower_ms = 100;
            election_timeout_upper_ms = 200;
        }

        roll("slow_disk_flush", 0.50);
        roll("slow_commits", 0.45);
        roll("network_reordering", 0.65);
        roll("aggressive_clogging", profile == "partition-storm" || profile == "chaos" ? 0.85 : 0.45);
        roll("asymmetric_partitions", profile == "partition-storm" || profile == "chaos" ? 0.85 : 0.55);
        roll("clock_skew", 0.55);
        roll("torn_writes_on_power_loss", profile == "power-loss" || profile == "chaos" ? 0.90 : 0.60);
        roll("transient_disk_io_errors", profile == "power-loss" || profile == "snapshot-storm" ? 0.85 : 0.50);
        roll("delayed_snapshot_publish", 0.55);
        write_log_entry_version = roll("batched_log_entries", 0.60) ? 1 : 0;
        multi_request_batches = (write_log_entry_version == 1) && roll("multi_request_batches", 0.70);
        compress_logs = force_compress_logs || roll("compressed_logs", 0.45);
        compress_snapshots_zstd = !roll("lz4_snapshots", 0.35);

        if (roll("lsmt_tiny_memtables", 0.70))
        {
            lsmt_memtable_size = static_cast<uint64_t>(rng.rangeInt(512, 2048));
            lsmt_block_size = static_cast<uint64_t>(rng.rangeInt(256, 512));
            lsmt_min_files_to_merge = 2;
        }
        else
        {
            lsmt_memtable_size = 4096;
            lsmt_block_size = 1024;
            lsmt_min_files_to_merge = 3;
        }

        if (roll("lsmt_tiny_block_cache", 0.65))
            lsmt_block_cache_size = static_cast<uint64_t>(rng.rangeInt(512, 2048));
        else
            lsmt_block_cache_size = 65536;

        if (roll("lsmt_tiny_sorted_files", 0.70))
        {
            lsmt_file_block_size = static_cast<uint64_t>(rng.rangeInt(256, 512));
            lsmt_block_group_compressed_size = static_cast<uint64_t>(rng.rangeInt(256, 512));
            lsmt_sorted_file_uncompressed_size = static_cast<uint64_t>(rng.rangeInt(512, 1280));
        }
        else
        {
            lsmt_file_block_size = 4096;
            lsmt_block_group_compressed_size = 4096;
            lsmt_sorted_file_uncompressed_size = 16384;
        }

        write_snapshot_version = 9;
        if (!disable_bounded_cache && roll("bounded_log_cache", 0.65))
            latest_logs_cache_entry_count_threshold = static_cast<uint64_t>(rng.rangeInt(2, 8));
        else
            latest_logs_cache_entry_count_threshold = 0;

        if (enable_snapshot_torture)
            snapshot_transfer_chunk_size = static_cast<uint64_t>(rng.rangeInt(64, 256));
        else if (roll("chunked_snapshot_transfer", 0.65))
            snapshot_transfer_chunk_size = static_cast<uint64_t>(rng.rangeInt(256, 1024));
        else
            snapshot_transfer_chunk_size = 0;

        force_sync_logs = true;
        log_sync_stop_gap = roll("small_log_sync_stop_gap", 0.50) ? static_cast<int32_t>(rng.rangeInt(0, 3)) : static_cast<int32_t>(rng.rangeInt(4, 15));
        use_new_joiner_type = force_new_joiner_ || roll("use_new_joiner_type", 0.50);
        enable_dynamic_membership = dynamic_membership;
        enable_late_session_race = late_session_race;
        snapshot_torture = enable_snapshot_torture;
        mixed_versions = enable_mixed_versions;
        async_replication = enable_async_replication;
        large_values = enable_snapshot_torture || roll("large_values", 0.20);
        force_new_joiner = force_new_joiner_;
        no_disk_faults = disable_disk_faults;
        enable_backpressure = backpressure;
        large_snapshot_distance = large_snap_dist;
        acl_torture = enable_acl_torture;
        torn_snapshots = enable_torn_snapshots;
        auto_forward = enable_auto_forward;
        full_consensus = enable_full_consensus;
        rollback_torture = enable_rollback_torture;
        watch_oracle = enable_watch_oracle;
        if (enable_rollback_torture)
        {
            switches["slow_commits"] = true;
            switches["asymmetric_partitions"] = true;
            switches["network_reordering"] = true;
            acl_torture = true;
        }
        if (enable_full_consensus)
        {
            track_peers_sm_commit_idx = true;
            grace_period_lagging_sm_ms = static_cast<int32_t>(rng.rangeInt(300, 1200));
        }
        snapshots_to_keep = enable_torn_snapshots ? 3ULL : static_cast<uint64_t>(rng.rangeInt(1, 3));
        container_gc_max_never_used_ms = roll("container_gc_never_used", 0.35) ? 1500ULL : 0ULL;
        if (backpressure)
        {
            max_uncommitted_log_entries = static_cast<uint64_t>(rng.rangeInt(3, 10));
            slow_member_backpressure_max_uncommitted = static_cast<uint64_t>(rng.rangeInt(2, 5));
            slow_member_backpressure_no_progress_timeout_ms = static_cast<int32_t>(rng.rangeInt(400, 1500));
            max_append_size_bytes = static_cast<uint64_t>(rng.rangeInt(512, 2048));
            backward_probe_throttle_threshold = static_cast<int32_t>(rng.rangeInt(1, 3));
            track_peers_sm_commit_idx = roll("track_peers_sm_commit_idx", 0.50);
            large_values = true;
        }
        if (disable_disk_faults)
            switches["transient_disk_io_errors"] = false;
        leadership_expiry_ms = enable_leadership_expiry ? static_cast<int32_t>(rng.rangeInt(1200, 2500)) : 0;
    }

    bool isActive(const std::string & name) const
    {
        auto it = switches.find(name);
        return it != switches.end() && it->second;
    }

    bool fire(const std::string & name, SimRandom & rng, double prob = 0.25) const
    {
        return isActive(name) && rng.chance(prob);
    }
};

/// FoundationDB-inspired SOMETIMES / ALWAYS simulation coverage tracker.
/// Tracks rare protocol & fault states hit during simulation so campaigns can verify
/// that chaos actually reached hard distributed states rather than running trivially.
class CoverageTracker
{
public:
    void hit(const std::string & name)
    {
        ++counts[name];
    }

    uint64_t count(const std::string & name) const
    {
        auto it = counts.find(name);
        return it == counts.end() ? 0 : it->second;
    }

    const std::map<std::string, uint64_t> & all() const
    {
        return counts;
    }

private:
    std::map<std::string, uint64_t> counts;
};

/// Seed ClickHouse's thread_local_rng deterministically so snapshot/LSMT filenames
/// and any internal randomized code paths stay 100% reproducible for a given seed.
inline void seedProcessRng(uint64_t seed)
{
    thread_local_rng.seed(seed ^ 0x9E3779B97F4A7C15ULL);
}

} // namespace keeper_sim

extern "C" int realPthreadCreate(pthread_t *, const pthread_attr_t *, void * (*)(void *), void *) asm("__real_pthread_create");
extern "C" int wrappedPthreadCreate(pthread_t *, const pthread_attr_t *, void * (*)(void *), void *) asm("__wrap_pthread_create");
extern "C" int wrappedPthreadCreate(pthread_t * thread, const pthread_attr_t * attr, void * (*start)(void *), void * arg)
{
    ++keeper_sim::g_thread_attempts;
    return realPthreadCreate(thread, attr, start, arg);
}

extern "C" int realClockGettime(clockid_t, timespec *) asm("__real_clock_gettime");
extern "C" int wrappedClockGettime(clockid_t, timespec *) asm("__wrap_clock_gettime");
extern "C" int wrappedClockGettime(clockid_t id, timespec * value)
{
    if (keeper_sim::g_virtual_time_enabled
        && (id == CLOCK_MONOTONIC || id == CLOCK_MONOTONIC_RAW || id == CLOCK_REALTIME))
    {
        ++keeper_sim::g_virtual_posix_reads;
        const int64_t base_ns = keeper_sim::g_sim_time_ns
            + (id == CLOCK_REALTIME ? (1700000000LL * 1'000'000'000LL + static_cast<int64_t>(keeper_sim::g_sim_seed & 0xFFFFULL)) : 0LL);
        value->tv_sec = static_cast<time_t>(base_ns / 1'000'000'000LL);
        value->tv_nsec = static_cast<long>(base_ns % 1'000'000'000LL);
        return 0;
    }
    ++keeper_sim::g_host_clock_reads;
    const size_t bucket = (id >= 0 && id < 16) ? static_cast<size_t>(id) : 16;
    ++keeper_sim::g_delegated_posix_reads[bucket];
    return realClockGettime(id, value);
}
