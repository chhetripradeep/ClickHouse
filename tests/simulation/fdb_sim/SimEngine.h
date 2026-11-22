#pragma once

#include <SimClock.h>

#include <chrono>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iomanip>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

namespace fdb_sim
{

using EventId = uint64_t;

/// Thrown by `SimKeeperStateManager::system_exit` to simulate a single node's
/// fail-fast process termination without aborting other nodes in the cluster.
struct SimNodeSystemExit : public std::runtime_error
{
    int node_id;
    int exit_code;

    SimNodeSystemExit(int node_id_, int exit_code_)
        : std::runtime_error(
            "Node " + std::to_string(node_id_) + " called system_exit(" + std::to_string(exit_code_) + ")")
        , node_id(node_id_)
        , exit_code(exit_code_)
    {
    }
};

struct SimEvent
{
    int64_t due_ns = 0;
    EventId id = 0;
    int node_id = 0;       // Owning node (0 = cluster/global/client actor)
    uint64_t incarnation = 0; // If non-zero, event is dropped if node incarnation changed
    std::string kind;
    std::function<void()> callback;

    bool operator>(const SimEvent & other) const
    {
        if (due_ns != other.due_ns)
            return due_ns > other.due_ns;
        return id > other.id;
    }
};

/// FoundationDB Sim2-inspired single-threaded virtual-time discrete-event scheduler.
/// Every timer, RPC delivery, background disk flush, commit step, LSMT flush/merge,
/// snapshot write, client workload tick, and chaos event runs in deterministic total
/// order on this queue with zero OS threads and zero host-clock nondeterminism.
class SimEngine
{
public:
    explicit SimEngine(uint64_t seed, int host_delay_ms_ = 0, const std::string & trace_path = "")
        : rng(seed)
        , host_delay_ms(host_delay_ms_)
    {
        if (!trace_path.empty())
        {
            trace_stream.open(trace_path);
            require(trace_stream.is_open(), "Failed to open trace file: " + trace_path);
        }
    }

    EventId scheduleAt(
        int64_t due_ns,
        std::string kind,
        std::function<void()> cb,
        int node_id = 0,
        uint64_t incarnation = 0)
    {
        const int64_t clamped_due = std::max(due_ns, g_sim_time_ns);
        const EventId id = ++next_event_id;
        queue.push(SimEvent{clamped_due, id, node_id, incarnation, std::move(kind), std::move(cb)});
        return id;
    }

    EventId scheduleAfterMs(
        int64_t delay_ms,
        std::string kind,
        std::function<void()> cb,
        int node_id = 0,
        uint64_t incarnation = 0)
    {
        require(delay_ms >= 0, "Negative event delay");
        return scheduleAt(g_sim_time_ns + delay_ms * 1'000'000LL, std::move(kind), std::move(cb), node_id, incarnation);
    }

    EventId scheduleAfterUs(
        int64_t delay_us,
        std::string kind,
        std::function<void()> cb,
        int node_id = 0,
        uint64_t incarnation = 0)
    {
        require(delay_us >= 0, "Negative microsecond delay");
        return scheduleAt(g_sim_time_ns + delay_us * 1'000LL, std::move(kind), std::move(cb), node_id, incarnation);
    }

    void cancel(EventId id)
    {
        if (id != 0)
            cancelled.insert(id);
    }

    /// Immediately purge all queued events owned by `node_id` so their captured
    /// callbacks and shared_ptrs are destroyed before weak_ptr leak checks run.
    void purgeNodeEvents(int node_id)
    {
        std::vector<SimEvent> kept;
        kept.reserve(queue.size());
        while (!queue.empty())
        {
            SimEvent ev = queue.top();
            queue.pop();
            if (ev.node_id == node_id)
            {
                cancelled.erase(ev.id);
                continue;
            }
            kept.push_back(std::move(ev));
        }
        for (auto & ev : kept)
            queue.push(std::move(ev));
    }

    void setNodeIncarnation(int node_id, uint64_t inc)
    {
        node_incarnations[node_id] = inc;
    }

    uint64_t getNodeIncarnation(int node_id) const
    {
        auto it = node_incarnations.find(node_id);
        return it == node_incarnations.end() ? 0 : it->second;
    }

    /// Optional per-step hook (e.g. polling manual Changelog / LSMT queues & invariants).
    std::function<void()> on_before_step;
    std::function<void(const std::string & kind, int node_id)> on_after_step;
    std::function<void(int node_id, int exit_code)> on_node_system_exit;

    void trace(const std::string & line)
    {
        /// FNV-1a 64-bit streaming digest over every trace record for O(1) memory determinism checks.
        for (unsigned char c : line)
        {
            trace_hash ^= static_cast<uint64_t>(c);
            trace_hash *= 1099511628211ULL;
        }
        trace_hash ^= '\n';
        trace_hash *= 1099511628211ULL;
        ++trace_lines;

        if (trace_stream.is_open())
            trace_stream << g_sim_time_ns / 1'000'000LL << "ms #" << executed_events << " " << line << '\n';
    }

    bool step(int64_t max_time_ns = INT64_MAX)
    {
        if (on_before_step)
            on_before_step();

        while (!queue.empty())
        {
            if (queue.top().due_ns > max_time_ns)
                return false;

            SimEvent ev = queue.top();
            queue.pop();

            if (cancelled.erase(ev.id) > 0)
                continue;

            if (ev.node_id != 0 && ev.incarnation != 0)
            {
                auto it = node_incarnations.find(ev.node_id);
                if (it == node_incarnations.end() || it->second != ev.incarnation)
                    continue;
            }

            if (ev.due_ns > g_sim_time_ns)
                g_sim_time_ns = ev.due_ns;

            ++executed_events;
            require(executed_events <= max_events_budget, "Simulation event budget exceeded (" + std::to_string(max_events_budget) + ")");

            /// Diagnostic host-delay perturbation: never used for synchronization, only to prove
            /// that varying real wall-clock pacing produces the exact same virtual trace hash.
            if (host_delay_ms > 0 && (executed_events % 50 == 0))
                std::this_thread::sleep_for(std::chrono::milliseconds(host_delay_ms));

            trace("EV " + ev.kind + " n=" + std::to_string(ev.node_id) + " t=" + std::to_string(g_sim_time_ns));
            try
            {
                ev.callback();
            }
            catch (const SimNodeSystemExit & exit_ex)
            {
                if (!on_node_system_exit)
                    throw;
                on_node_system_exit(exit_ex.node_id, exit_ex.exit_code);
            }

            require(g_thread_attempts.load() == 0 && liveThreadCount() == 1,
                "Uncontrolled OS thread detected after event '" + ev.kind
                + "': attempts=" + std::to_string(g_thread_attempts.load())
                + " live=" + std::to_string(liveThreadCount()));

            if (on_after_step)
                on_after_step(ev.kind, ev.node_id);

            return true;
        }
        return false;
    }

    void runUntilMs(int64_t target_ms)
    {
        const int64_t target_ns = target_ms * 1'000'000LL;
        while (step(target_ns))
        {
        }
        if (g_sim_time_ns < target_ns)
            g_sim_time_ns = target_ns;
    }

    bool runUntilCondition(int64_t max_additional_ms, const std::function<bool()> & done)
    {
        const int64_t deadline_ns = g_sim_time_ns + max_additional_ms * 1'000'000LL;
        while (!done())
        {
            if (!step(deadline_ns))
                return done();
        }
        return true;
    }

    std::string traceHashHex() const
    {
        std::ostringstream oss;
        oss << std::hex << std::setw(16) << std::setfill('0') << trace_hash;
        return oss.str();
    }

    void clearPending()
    {
        while (!queue.empty())
            queue.pop();
        cancelled.clear();
    }

    SimRandom rng;
    BuggifyConfig buggify;
    CoverageTracker coverage;
    uint64_t executed_events = 0;
    uint64_t max_events_budget = 250'000;
    uint64_t trace_lines = 0;

private:
    std::priority_queue<SimEvent, std::vector<SimEvent>, std::greater<SimEvent>> queue;
    std::unordered_set<EventId> cancelled;
    std::map<int, uint64_t> node_incarnations;
    EventId next_event_id = 0;
    int host_delay_ms = 0;
    uint64_t trace_hash = 14695981039346656037ULL;
    std::ofstream trace_stream;
};

} // namespace fdb_sim
