#include <libnuraft/nuraft.hxx>
#include <libnuraft/raft_server_handler.hxx>
#include <in_memory_state_mgr.hxx>
#include <Coordination/SessionExpiryQueue.h>

#if SIMULATION_KEEPER
#include <Coordination/KeeperLogStore.h>
#include <Coordination/KeeperStateManager.h>
#include <Coordination/KeeperStateMachine.h>
#include <Coordination/KeeperStorage.h>
#include <Common/Stopwatch.h>
#include <Common/ThreadPool.h>
#include <Common/tests/gtest_global_context.h>
#include <Common/scope_guard_safe.h>
#include <Disks/DiskLocal.h>
#include <IO/SharedThreadPools.h>
#include <IO/SwapHelper.h>
#include <IO/WriteBufferFromFileDecorator.h>
#include <Poco/Logger.h>
#include <Poco/ThreadPool.h>

namespace DB::CoordinationSetting
{
    extern const CoordinationSettingsBool use_lsmt_storage;
    extern const CoordinationSettingsBool storage_memory_only;
    extern const CoordinationSettingsMilliseconds dead_session_check_period_ms;
    extern const CoordinationSettingsUInt64 parallel_read_threads;
}
#endif

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <pthread.h>

namespace
{
std::atomic<size_t> thread_attempts{0};
#if !SIMULATION_KEEPER
std::atomic<size_t> steady_reads{0};
std::atomic<size_t> system_reads{0};
#else
std::atomic<size_t> virtual_posix_reads{0};
std::array<std::atomic<size_t>, 17> delegated_posix_reads{};
#endif
std::atomic<size_t> host_clock_reads{0};
bool virtual_time = false;
int64_t logical_ms = 1000;
uint32_t random_seed = 1;
std::string record_schedule_path;
std::string replay_schedule_path;
std::string schedule_version = "nuraft-schedule-v3";
bool recovery_scenario = false;
bool lost_reply_scenario = false;
#if SIMULATION_KEEPER
bool metadata_write_failure_scenario = false;
bool unsafe_metadata_backup_loss = false;
bool snapshot_write_failure_scenario = false;
bool unsafe_snapshot_write_control = false;
std::string metadata_fault = "open";
std::string snapshot_fault = "open";
bool generated_workload = false;
bool multi_workload = false;
bool pipelined_workload = false;
std::string pipeline_fault;
size_t pipeline_fault_batch = 0;
#endif

void require(bool condition, const std::string & message)
{
    if (!condition)
        throw std::runtime_error(message);
}

size_t liveThreads()
{
    const auto tasks = std::filesystem::directory_iterator("/proc/self/task");
    return std::distance(begin(tasks), end(tasks));
}
}

extern "C" int realPthreadCreate(pthread_t *, const pthread_attr_t *, void * (*)(void *), void *) asm("__real_pthread_create");
extern "C" int wrappedPthreadCreate(pthread_t *, const pthread_attr_t *, void * (*)(void *), void *) asm("__wrap_pthread_create");
extern "C" int wrappedPthreadCreate(pthread_t * thread, const pthread_attr_t * attr, void * (*start)(void *), void * arg)
{
    ++thread_attempts;
    return realPthreadCreate(thread, attr, start, arg);
}

#if SIMULATION_KEEPER
extern "C" int realClockGettime(clockid_t, timespec *) asm("__real_clock_gettime");
extern "C" int wrappedClockGettime(clockid_t, timespec *) asm("__wrap_clock_gettime");
extern "C" int wrappedClockGettime(clockid_t id, timespec * value)
{
    if (virtual_time && (id == CLOCK_MONOTONIC || id == CLOCK_MONOTONIC_RAW || id == CLOCK_REALTIME))
    {
        ++virtual_posix_reads;
        value->tv_sec = logical_ms / 1000 + (id == CLOCK_REALTIME ? 1700000000 : 0);
        value->tv_nsec = (logical_ms % 1000) * 1000000 + (id == CLOCK_REALTIME ? random_seed : 0);
        value->tv_sec += value->tv_nsec / 1000000000;
        value->tv_nsec %= 1000000000;
        return 0;
    }
    ++host_clock_reads;
    const auto bucket = id >= 0 && id < 16 ? static_cast<size_t>(id) : 16;
    ++delegated_posix_reads[bucket];
    return realClockGettime(id, value);
}
#else
extern "C" std::chrono::steady_clock::time_point __real__ZNSt6chrono3_V212steady_clock3nowEv();
extern "C" std::chrono::steady_clock::time_point __wrap__ZNSt6chrono3_V212steady_clock3nowEv()
{
    ++steady_reads;
    if (virtual_time)
        return std::chrono::steady_clock::time_point(std::chrono::milliseconds(logical_ms));
    ++host_clock_reads;
    return __real__ZNSt6chrono3_V212steady_clock3nowEv();
}

extern "C" std::chrono::system_clock::time_point __real__ZNSt6chrono3_V212system_clock3nowEv();
extern "C" std::chrono::system_clock::time_point __wrap__ZNSt6chrono3_V212system_clock3nowEv()
{
    ++system_reads;
    if (virtual_time)
        return std::chrono::system_clock::time_point(std::chrono::milliseconds(logical_ms)
            + std::chrono::nanoseconds(random_seed));
    ++host_clock_reads;
    return __real__ZNSt6chrono3_V212system_clock3nowEv();
}
#endif

namespace
{
using namespace nuraft;

class ManualServer : public raft_server
{
public:
    explicit ManualServer(context * ctx) : raft_server(ctx, init_options(false, false, true))
    {
    }

    void appendStep()
    {
        append_entries_in_bg_exec();
    }

    void commitStep()
    {
        require(commit_in_bg_exec(), "Commit step unexpectedly yielded");
    }
};

struct Event
{
    uint64_t id;
    std::string kind;
    int node;
    int other;
    std::function<void()> run;
};

class Driver : public global_mgr, public raft_server_handler
{
public:
    using raft_server_handler::process_req;

    struct PendingResponse
    {
        int src;
        int dst;
        std::function<void(bool)> cancel;
    };

    std::deque<Event> events;
    std::map<uint64_t, PendingResponse> pending_responses;
    std::map<int, ptr<ManualServer>> nodes;
    std::map<int, uint64_t> incarnations;
    size_t executed = 0;
    size_t cancelled_response_completions = 0;
    size_t lost_responses = 0;
    int delay_ms = 0;
    int lose_response_from = 0;
    bool lose_response_after_durability = false;
#if SIMULATION_KEEPER
    std::set<int> paused_log_writes;
    std::function<void()> poll_background;
#endif
    std::mt19937 schedule_rng{random_seed};
    uint64_t next_event_id = 1;
    uint64_t next_response_id = 1;
    std::ofstream recorded_schedule;
    std::ifstream replay_schedule;

    Driver()
    {
        require(record_schedule_path.empty() || replay_schedule_path.empty(), "Cannot record and replay together");
        if (!record_schedule_path.empty())
        {
            require(!std::filesystem::exists(record_schedule_path), "Schedule output already exists");
            recorded_schedule.exceptions(std::ios::failbit | std::ios::badbit);
            recorded_schedule.open(record_schedule_path);
            recorded_schedule << schedule_version << " " << random_seed << " " << recovery_scenario
                << " " << lost_reply_scenario << '\n';
        }
        if (!replay_schedule_path.empty())
        {
            replay_schedule.open(replay_schedule_path);
            std::string version;
            uint64_t seed = 0;
            bool recovery = false;
            bool lost_reply = false;
            require(bool(replay_schedule >> version >> seed >> recovery >> lost_reply), "Cannot read replay header");
            require(version == schedule_version && seed == random_seed && recovery == recovery_scenario
                && lost_reply == lost_reply_scenario,
                "Replay header does not match seed/version/scenario");
        }
    }

    void finishReplay()
    {
        if (replay_schedule.is_open())
        {
            std::string unused;
            require(!(replay_schedule >> unused), "Replay schedule has unused choices");
            require(replay_schedule.eof(), "Replay schedule read failed");
        }
        if (recorded_schedule.is_open())
            recorded_schedule.flush();
    }

    ~Driver() override
    {
        for (const auto & [id, node] : nodes)
            node->shutdown();
        nodes.clear();
    }

    void record(const std::string & message)
    {
        std::cout << "TRACE " << executed << " " << message << '\n';
    }

    void enqueue(std::string kind, int node, int other, std::function<void()> run)
    {
        events.push_back({next_event_id++, std::move(kind), node, other, std::move(run)});
    }

    bool step(int withheld = 0, bool local_only = false)
    {
#if SIMULATION_KEEPER
        if (poll_background)
            poll_background();
#endif
        std::vector<size_t> candidates;
        for (size_t i = 0; i < events.size(); ++i)
        {
            const auto & event = events[i];
#if SIMULATION_KEEPER
            if (event.kind == "log-write" && paused_log_writes.contains(event.node))
                continue;
#endif
            if ((!withheld || (event.node != withheld && event.other != withheld))
                && (!local_only || (event.node == 1 && event.other == 0)))
                candidates.push_back(i);
        }
        if (candidates.empty())
            return false;
        auto chosen = candidates[schedule_rng() % candidates.size()];
        if (replay_schedule.is_open())
        {
            uint64_t id = 0;
            std::string kind;
            int node = 0;
            int other = 0;
            require(bool(replay_schedule >> id >> kind >> node >> other), "Replay schedule exhausted or malformed");
            auto choice = std::find_if(candidates.begin(), candidates.end(), [&](size_t index)
            {
                const auto & event = events[index];
                return event.id == id && event.kind == kind && event.node == node && event.other == other;
            });
            require(choice != candidates.end(), "Replay choice is not eligible");
            chosen = *choice;
        }
        const auto it = events.begin() + chosen;
        Event event = std::move(*it);
        events.erase(it);
        require(++executed <= 10000, "Event budget exhausted");
        if (recorded_schedule.is_open())
            recorded_schedule << event.id << " " << event.kind << " " << event.node << " " << event.other << '\n';
        // Diagnostic host perturbation, never a synchronization or progress mechanism.
        if (delay_ms)
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        record("choice " + std::to_string(event.id));
        record(event.kind + " " + std::to_string(event.node) + " " + std::to_string(event.other));
        event.run();
        require(liveThreads() == 1 && thread_attempts == 0, "Uncontrolled thread detected");
        for (const auto & [id, node] : nodes)
            record("state " + std::to_string(id) + " " + std::to_string(node->get_term())
                + " " + std::to_string(node->get_last_log_idx())
                + " " + std::to_string(node->get_target_committed_log_idx())
                + " " + std::to_string(node->get_committed_log_idx()));
        return true;
    }

    void drain(int withheld = 0)
    {
        while (step(withheld))
        {
        }
    }

    void init_raft_server(raft_server *) override
    {
    }

    void close_raft_server(raft_server * server) override
    {
        const int id = server->get_id();
        events.erase(std::remove_if(events.begin(), events.end(), [id](const Event & event)
        {
            return event.node == id || event.other == id;
        }), events.end());
        for (auto it = pending_responses.begin(); it != pending_responses.end();)
        {
            if (it->second.src == id || it->second.dst == id)
            {
                it->second.cancel(it->second.src != id);
                it = pending_responses.erase(it);
            }
            else
                ++it;
        }
    }

    void request_append(ptr<raft_server> server) override
    {
        auto node = std::static_pointer_cast<ManualServer>(server);
        enqueue("append", node->get_id(), 0, [node]
        {
            node->appendStep();
        });
    }

    void request_commit(ptr<raft_server> server) override
    {
        auto node = std::static_pointer_cast<ManualServer>(server);
        enqueue("commit", node->get_id(), 0, [node]
        {
            node->commitStep();
        });
    }

    void send(ptr<req_msg> request, rpc_handler callback)
    {
        const int src = request->get_src();
        const int dst = request->get_dst();
        const uint64_t target = nodes.count(dst) ? incarnations.at(dst) : 0;
        enqueue("request-" + std::to_string(request->get_type()), src, dst, [this, request, callback, src, dst, target]() mutable
        {
            if (!nodes.count(dst) || incarnations.at(dst) != target)
            {
                enqueue("rpc-error", dst, src, [request, callback]() mutable
                {
                    ptr<resp_msg> response;
                    auto error = cs_new<rpc_exception>("simulated node unavailable", request);
                    callback(response, error);
                });
                return;
            }
            auto response = process_req(nodes.at(dst).get(), *request);
            require(response && !response->has_cb(), "Unexpected blocking RPC response");
            const bool lose_response = dst == lose_response_from && request->get_type() == msg_type::append_entries_request;
            if (lose_response)
                lose_response_from = 0;
            auto deliver = [this, callback, request, response, src, dst, lose_response]
            {
                if (lose_response)
                {
                    ++lost_responses;
                    record("lose-replication-response " + std::to_string(dst) + " " + std::to_string(src));
                    enqueue("rpc-lost-response", dst, src, [request, callback]() mutable
                    {
                        ptr<resp_msg> no_response;
                        auto error = cs_new<rpc_exception>("simulated response loss after request execution", request);
                        callback(no_response, error);
                    });
                    return;
                }
                enqueue("response-" + std::to_string(response->get_type()), dst, src, [callback, response]() mutable
                {
                    ptr<rpc_exception> error;
                    callback(response, error);
                });
            };
            if (lose_response && !lose_response_after_durability)
            {
                deliver();
                return;
            }
            if (response->has_async_cb())
            {
                record("defer-response " + std::to_string(dst) + " " + std::to_string(src));
                auto deferred = response->call_async_cb();
                // Break the response/promise ownership cycle before installing its completion.
                response->set_async_cb({});
                const auto response_id = next_response_id++;
                auto cancelled = std::make_shared<bool>(false);
                pending_responses.emplace(response_id, PendingResponse{src, dst,
                    [this, cancelled, request, callback, src](bool notify) mutable
                    {
                        *cancelled = true;
                        if (notify)
                        {
                            enqueue("rpc-cancelled", src, 0, [request, callback]() mutable
                            {
                                ptr<resp_msg> no_response;
                                auto error = cs_new<rpc_exception>("simulated runtime stopped before response", request);
                                callback(no_response, error);
                            });
                        }
                    }});
                deferred->when_ready([this, response_id, cancelled, response, deliver]
                    (cmd_result<ptr<buffer>> & result, ptr<std::exception> & error)
                {
                    if (*cancelled)
                    {
                        ++cancelled_response_completions;
                        record("discard-cancelled-response " + std::to_string(response_id));
                        result.reset();
                        return;
                    }
                    pending_responses.erase(response_id);
                    require(!error, "Deferred RPC completed with exception");
                    response->set_ctx(result.get());
                    const auto code = result.get_result_code();
                    if (code != cmd_result_code::OK)
                    {
                        response->unaccept();
                        response->set_result_code(code);
                    }
                    deliver();
                    result.reset();
                });
            }
            else
                deliver();
        });
    }
};

class ManualClient : public rpc_client
{
public:
    explicit ManualClient(Driver & driver_, uint64_t id_) : driver(driver_), id(id_)
    {
    }

    void send(ptr<req_msg> & request, rpc_handler & callback, uint64_t) override
    {
        driver.send(request, callback);
    }

    uint64_t get_id() const override
    {
        return id;
    }

    bool is_abandoned() const override
    {
        return false;
    }
private:
    Driver & driver;
    uint64_t id;
};

class ManualFactory : public rpc_client_factory
{
public:
    explicit ManualFactory(Driver & driver_) : driver(driver_)
    {
    }

    ptr<rpc_client> create_client(const std::string & endpoint) override
    {
        require(endpoint == "1" || endpoint == "2" || endpoint == "3", "Unknown endpoint");
        return cs_new<ManualClient>(driver, ++next_id);
    }
private:
    Driver & driver;
    uint64_t next_id = 0;
};

class ManualTimers : public delayed_task_scheduler
{
public:
    ManualTimers(int id_, Driver & driver_) : id(id_), driver(driver_)
    {
    }

    void schedule(ptr<delayed_task> & task, int32 milliseconds) override
    {
        require(milliseconds >= 0, "Negative timer delay");
        cancel_impl(task);
        task->reset();
        tasks.push_back({task, logical_ms + milliseconds});
        driver.record("timer-arm " + std::to_string(id) + " " + std::to_string(task->get_type())
            + " " + std::to_string(milliseconds));
    }

    void fire(int type)
    {
        const auto it = std::find_if(tasks.begin(), tasks.end(), [type](const auto & entry)
        {
            return entry.first->get_type() == type;
        });
        require(it != tasks.end(), "Requested timer is not pending");
        auto task = it->first;
        if (virtual_time)
            logical_ms = std::max(logical_ms, it->second);
        tasks.erase(it);
        driver.record("timer-fire " + std::to_string(id) + " " + std::to_string(type)
            + " " + std::to_string(logical_ms));
        task->execute();
    }

    bool empty() const
    {
        return tasks.empty();
    }

private:
    int id;
    Driver & driver;
    std::vector<std::pair<ptr<delayed_task>, int64_t>> tasks;
    void cancel_impl(ptr<delayed_task> & task) override
    {
        tasks.erase(std::remove_if(tasks.begin(), tasks.end(), [&task](const auto & entry)
        {
            return entry.first == task;
        }), tasks.end());
    }
};

class ManualLog : public inmem_log_store
{
public:
    void pauseDurability()
    {
        require(!paused, "Log durability already paused");
        durable_index = next_slot() - 1;
        paused = true;
    }

    void completeDurability()
    {
        require(paused, "No pending durability completion");
        paused = false;
    }

    nuraft::ulong last_durable_index() override
    {
        return paused ? durable_index : next_slot() - 1;
    }

    bool flush() override
    {
        require(!paused, "Synchronous flush bypassed manual durability completion");
        return inmem_log_store::flush();
    }

    [[maybe_unused]] ptr<ManualLog> durableImage()
    {
        require(paused, "Volatile-tail loss requires pending durability");
        require(start_index() == 1, "Durable-image model does not support compacted logs");
        auto image = cs_new<ManualLog>();
        for (nuraft::ulong index = 1; index <= durable_index; ++index)
        {
            auto entry = entry_at(index);
            require(image->append(entry) == index, "Durable-image index changed");
        }
        return image;
    }

private:
    bool paused = false;
    nuraft::ulong durable_index = 0;
};

class MemoryManager : public inmem_state_mgr
{
public:
    explicit MemoryManager(int id) : inmem_state_mgr(id, std::to_string(id))
    {
    }

    ptr<ManualLog> log = cs_new<ManualLog>();

    ptr<log_store> load_log_store() override
    {
        return log;
    }

    void system_exit(int code) override
    {
        throw std::runtime_error("NuRaft fatal error " + std::to_string(code));
    }
};

#if !SIMULATION_KEEPER
struct MachineImage
{
    std::vector<int32_t> values;
    nuraft::ulong last_index = 0;
};

class Machine : public state_machine
{
public:
    Machine(int id_, Driver & driver_, std::shared_ptr<MachineImage> image_)
        : image(std::move(image_)), values(image->values), id(id_), driver(driver_)
    {
    }

    std::shared_ptr<MachineImage> image;
    std::vector<int32_t> & values;
    ptr<buffer> commit(const nuraft::ulong index, buffer & data) override
    {
        data.pos(0);
        const auto value = data.get_int();
        values.push_back(value);
        image->last_index = index;
        driver.record("apply " + std::to_string(id) + " " + std::to_string(index) + " " + std::to_string(value));
        return nullptr;
    }
    void commit_config(const nuraft::ulong index, ptr<cluster_config> &) override
    {
        image->last_index = index;
    }

    nuraft::ulong last_commit_index() override
    {
        return image->last_index;
    }

    ptr<snapshot> last_snapshot() override
    {
        return nullptr;
    }

    bool apply_snapshot(snapshot &) override
    {
        throw std::runtime_error("Snapshot outside probe scope");
    }

    void create_snapshot(snapshot &, async_result<bool>::handler_type &) override
    {
        throw std::runtime_error("Snapshot outside probe scope");
    }
private:
    int id;
    Driver & driver;
};

void runKeeperExpiry(int delay_ms)
{
    int64_t now = 1000;
    DB::SessionExpiryQueue queue(100, [&now]
    {
        return now;
    });
    auto check = [](const DB::SessionExpiryQueue & subject, std::vector<int64_t> expected)
    {
        auto actual = subject.getExpiredSessions();
        std::sort(actual.begin(), actual.end());
        require(actual == expected, "Keeper session expiry differs");
    };
    auto advance = [&now, delay_ms](int64_t time)
    {
        require(time >= now, "Keeper logical clock moved backward");
        if (delay_ms)
            std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
        now = time;
    };

    queue.addNewSessionOrUpdate(11, 100);
    queue.addNewSessionOrUpdate(22, 149);
    queue.addNewSessionOrUpdate(33, 200);
    advance(1199);
    check(queue, {});
    advance(1200);
    check(queue, {11, 22});
    check(queue, {11, 22}); // Polling must not consume the expired sessions.
    queue.addNewSessionOrUpdate(22, 200);
    queue.addNewSessionOrUpdate(11, 0);
    advance(1299);
    check(queue, {});
    require(queue.remove(33) && !queue.remove(33), "Keeper session removal differs");
    advance(1300);
    check(queue, {11});
    require(queue.remove(11), "Could not remove expired Keeper session");
    check(queue, {});
    advance(1499);
    check(queue, {});
    advance(1500);
    check(queue, {22});
    queue.clear();
    check(queue, {});
    queue.addNewSessionOrUpdate(44, 1);
    queue.addNewSessionOrUpdate(44, 50);
    queue.addNewSessionOrUpdate(55, 99);
    advance(1600);
    check(queue, {44, 55});
    queue.clear();
    queue.addNewSessionOrUpdate(66, 700);
    advance(5000);
    check(queue, {66});

    int64_t other_now = 1000;
    DB::SessionExpiryQueue other(100, [&other_now]
    {
        return other_now;
    });
    other.addNewSessionOrUpdate(66, 700);
    check(other, {});
    check(queue, {66});
    other_now = 1800;
    check(other, {66});
    other.clear();
    check(queue, {66});
    require(host_clock_reads == 0 && steady_reads == 0 && system_reads == 0,
        "Injected Keeper expiry clock escaped to a global clock");
    require(thread_attempts == 0 && liveThreads() == 1, "Keeper expiry created uncontrolled work");

    // The ordinary constructor must retain its system-clock behavior.
    virtual_time = true;
    logical_ms = 1000;
    DB::SessionExpiryQueue default_clock_queue(100);
    default_clock_queue.addNewSessionOrUpdate(77, 100);
    logical_ms = 1199;
    check(default_clock_queue, {});
    logical_ms = 1200;
    check(default_clock_queue, {77});
    require(system_reads > 0 && host_clock_reads == 0, "Default Keeper clock control failed");
    std::cout << "PASS keeper-expiry boundaries renewal removal clear isolation default-clock\n";
}

void runProbe(int delay_ms, bool unsafe_quorum, bool unsafe_durability)
{
    require(virtual_time || (record_schedule_path.empty() && replay_schedule_path.empty()),
        "Schedule recording/replay requires virtual time");
    require(liveThreads() == 1 && thread_attempts == 0, "Probe did not start single-threaded");
    Driver driver;
    driver.delay_ms = delay_ms;
    std::map<int, ptr<Machine>> machines;
    std::map<int, ptr<ManualTimers>> timers;
    std::map<int, ptr<MemoryManager>> managers;
    std::map<int, std::shared_ptr<MachineImage>> images;
    auto factory = cs_new<ManualFactory>(driver);
    cluster_config config;
    for (int id = 1; id <= 3; ++id)
        config.get_servers().push_back(cs_new<srv_config>(id, std::to_string(id)));

    raft_params params;
    params.election_timeout_lower_bound_ = virtual_time ? 150 : 0;
    params.election_timeout_upper_bound_ = virtual_time ? 300 : 0;
    params.rpc_failure_backoff_ = 0;
    params.heart_beat_interval_ = virtual_time ? 50 : 1000000;
    params.leadership_expiry_ = virtual_time ? 0 : -1;
    params.return_method_ = raft_params::async_handler;
    params.use_bg_thread_for_urgent_commit_ = true;
    params.use_bg_thread_for_snapshot_io_ = false;
    params.snapshot_distance_ = 0;
    params.parallel_log_appending_ = recovery_scenario && !unsafe_durability;
    params.max_log_gap_in_stream_ = recovery_scenario ? 8 : 0;
    if (unsafe_quorum)
        params.custom_commit_quorum_size_ = 1;

    for (int id = 1; id <= 3; ++id)
    {
        managers[id] = cs_new<MemoryManager>(id);
        managers[id]->save_config(config);
        images[id] = std::make_shared<MachineImage>();
        machines[id] = cs_new<Machine>(id, driver, images[id]);
        timers[id] = cs_new<ManualTimers>(id, driver);
        driver.incarnations[id] = 1;
        driver.nodes[id] = cs_new<ManualServer>(new context(managers[id], machines[id], {}, nullptr,
            factory, timers[id], params, &driver));
    }
    for (const auto & [id, node] : driver.nodes)
        node->start_server(false);
    require(driver.events.empty(), "Unexpected work before selected election");
    for (const auto & [id, node] : driver.nodes)
        require(!node->is_leader() && machines.at(id)->values.empty(), "Unselected progress");

    driver.record("fire-election 1");
    timers.at(1)->fire(timer_task_type::election_timer);
    driver.drain();
    require(driver.nodes.at(1)->is_leader(), "Selected election did not elect node 1");
    require(!driver.nodes.at(2)->is_leader() && !driver.nodes.at(3)->is_leader(), "Multiple leaders");

    auto value = buffer::alloc(sizeof(int32_t));
    value->put(int32_t{731});
    value->pos(0);
    auto result = driver.nodes.at(1)->append_entries({value});
    require(result->get_accepted(), "Leader did not accept the append");
    driver.record("accepted 731");
    require(!result->has_result(), "Append completed without scheduler progress");
    const auto index = driver.nodes.at(1)->get_last_log_idx();
    driver.record("appended 1 " + std::to_string(index));

    // Execute all local work, withholding both follower links.
    while (driver.step(0, true))
    {
    }
    require(!result->has_result(), "No-quorum append falsely completed");
    require(driver.nodes.at(1)->get_target_committed_log_idx() < index, "Committed without quorum");
    for (const auto & [id, machine] : machines)
        require(machine->values.empty(), "Applied without quorum");
    driver.record("no-quorum-safe");

    if (lost_reply_scenario)
    {
        require(virtual_time, "Lost-reply scenario requires virtual time");
        driver.lose_response_from = 2;
        driver.drain(3);
        require(driver.lose_response_from == 0, "Replication response-loss fault did not execute");
        require(driver.nodes.at(2)->get_last_log_idx() == index, "Response lost before follower appended");
        require(!result->has_result(), "Lost replication response falsely completed client append");
        for (const auto & [id, machine] : machines)
            require(machine->values.empty(), "Response-loss phase unexpectedly applied");
        driver.record("lost-response-append-still-pending");
        for (int attempt = 0; attempt < 8 && !result->has_result(); ++attempt)
        {
            timers.at(1)->fire(timer_task_type::heartbeat_timer);
            driver.drain(3);
        }
    }
    driver.drain(3);
    require(result->has_result() && result->get_result_code() == cmd_result_code::OK, "Majority did not complete append");
    require(machines.at(1)->values == std::vector<int32_t>{731}, "Wrong leader state");
    require(machines.at(2)->values == std::vector<int32_t>{731}, "Majority follower did not apply");
    require(machines.at(3)->values.empty(), "Withheld follower made progress");
    driver.record("majority-applied-with-follower-withheld");

    auto second_value = buffer::alloc(sizeof(int32_t));
    second_value->put(int32_t{-47});
    second_value->pos(0);
    auto second_result = driver.nodes.at(1)->append_entries({second_value});
    require(second_result->get_accepted() && !second_result->has_result(), "Second append was not pending");
    driver.record("accepted -47");
    driver.drain(3);
    require(second_result->has_result() && second_result->get_result_code() == cmd_result_code::OK,
        "Second majority append did not complete");
    require(machines.at(3)->values.empty(), "Withheld follower applied second value");

    driver.drain();
    for (const auto & [id, machine] : machines)
        require(machine->values == std::vector<int32_t>({731, -47}), "Ordered follower catchup differs");
    driver.record("all-replicas-exact");

    if (recovery_scenario)
    {
        require(virtual_time, "Recovery scenario requires virtual time");
        auto stop = [&](int id)
        {
            driver.record("stop-runtime " + std::to_string(id));
            std::weak_ptr<ManualServer> old_server = driver.nodes.at(id);
            std::weak_ptr<Machine> old_machine = machines.at(id);
            driver.nodes.at(id)->shutdown();
            driver.nodes.erase(id);
            machines.erase(id);
            require(old_server.expired() && old_machine.expired(), "Stopped node runtime was retained");
            require(timers.at(id)->empty(), "Stopped node kept timers");
        };
        auto restart = [&](int id)
        {
            ++driver.incarnations.at(id);
            timers[id] = cs_new<ManualTimers>(id, driver);
            machines[id] = cs_new<Machine>(id, driver, images.at(id));
            driver.nodes[id] = cs_new<ManualServer>(new context(managers.at(id), machines.at(id), {}, nullptr,
                factory, timers.at(id), params, &driver));
            driver.nodes.at(id)->start_server(false);
            driver.record("restart-runtime " + std::to_string(id));
        };
        stop(3);

        managers.at(1)->log->pauseDurability();
        if (!unsafe_durability)
            managers.at(2)->log->pauseDurability();
        auto third_value = buffer::alloc(sizeof(int32_t));
        third_value->put(int32_t{902});
        third_value->pos(0);
        auto third_result = driver.nodes.at(1)->append_entries({third_value});
        require(third_result->get_accepted() && !third_result->has_result(), "Offline-follower append was not pending");
        driver.drain();
        require(!third_result->has_result(), "Undurable leader falsely counted toward majority");
        const auto third_index = driver.nodes.at(1)->get_last_log_idx();
        require(managers.at(1)->log->last_durable_index() < third_index, "Leader log became durable without completion");
        require(managers.at(2)->log->last_durable_index() < third_index, "Follower log became durable without completion");
        require(driver.nodes.at(2)->get_last_log_idx() == third_index, "Follower did not append while durability was paused");
        for (const auto & [id, machine] : machines)
            require(machine->values == std::vector<int32_t>({731, -47}), "Applied without durable majority");
        driver.record("both-writes-await-durability");
        driver.enqueue("durable", 2, 0, [&]
        {
            managers.at(2)->log->completeDurability();
            driver.record("log-durable 2 " + std::to_string(third_index));
            driver.nodes.at(2)->notify_log_append_completion(true);
        });
        driver.drain();
        require(!third_result->has_result(), "One durable voter falsely completed append");
        require(managers.at(2)->log->last_durable_index() == third_index, "Follower did not durably append");
        for (const auto & [id, machine] : machines)
            require(machine->values == std::vector<int32_t>({731, -47}), "Applied without durable majority");
        driver.record("one-durable-voter-still-pending");
        driver.enqueue("durable", 1, 0, [&]
        {
            managers.at(1)->log->completeDurability();
            driver.record("log-durable 1 " + std::to_string(third_index));
            driver.nodes.at(1)->notify_log_append_completion(true);
        });
        driver.drain();
        require(third_result->has_result() && third_result->get_result_code() == cmd_result_code::OK,
            "Offline follower prevented majority commit");
        require(images.at(3)->values == std::vector<int32_t>({731, -47}), "Offline image changed");
        driver.record("majority-committed-with-runtime-offline");

        restart(3);
        for (int attempt = 0; attempt < 8 && machines.at(3)->values.size() != 3; ++attempt)
        {
            timers.at(1)->fire(timer_task_type::heartbeat_timer);
            driver.drain();
        }
        for (const auto & [id, machine] : machines)
            require(machine->values == std::vector<int32_t>({731, -47, 902}), "Restart lost or duplicated committed values");
        driver.record("restarted-replicas-exact");

        const auto previous_term = driver.nodes.at(1)->get_term();
        stop(1);
        // Both surviving voters must observe heartbeat loss before granting pre-votes.
        timers.at(3)->fire(timer_task_type::election_timer);
        driver.drain();
        for (int attempt = 0; attempt < 8 && !driver.nodes.at(2)->is_leader(); ++attempt)
        {
            timers.at(2)->fire(timer_task_type::election_timer);
            driver.drain();
        }
        require(driver.nodes.at(2)->is_leader() && !driver.nodes.at(3)->is_leader(), "Failover did not elect node 2");
        require(driver.nodes.at(2)->get_term() > previous_term, "Failover did not advance term");
        driver.record("leader-replaced 1 2");

        auto fourth_value = buffer::alloc(sizeof(int32_t));
        fourth_value->put(int32_t{-613});
        fourth_value->pos(0);
        auto fourth_result = driver.nodes.at(2)->append_entries({fourth_value});
        require(fourth_result->get_accepted() && !fourth_result->has_result(), "New-leader append was not pending");
        driver.drain();
        require(fourth_result->has_result() && fourth_result->get_result_code() == cmd_result_code::OK,
            "New leader failed to commit with majority");
        require(images.at(1)->values == std::vector<int32_t>({731, -47, 902}), "Offline former leader changed");
        restart(1);
        for (int attempt = 0; attempt < 8 && machines.at(1)->values.size() != 4; ++attempt)
        {
            timers.at(2)->fire(timer_task_type::heartbeat_timer);
            driver.drain();
        }
        require(!driver.nodes.at(1)->is_leader(), "Former leader resumed stale leadership");
        for (const auto & [id, machine] : machines)
            require(machine->values == std::vector<int32_t>({731, -47, 902, -613}), "Failover lost or duplicated committed values");
        driver.record("failover-replicas-exact");

        managers.at(1)->log->pauseDurability();
        auto fifth_value = buffer::alloc(sizeof(int32_t));
        fifth_value->put(int32_t{809});
        fifth_value->pos(0);
        auto fifth_result = driver.nodes.at(2)->append_entries({fifth_value});
        require(fifth_result->get_accepted() && !fifth_result->has_result(), "Interrupted-write append was not pending");
        driver.drain(3);
        const auto fifth_index = driver.nodes.at(2)->get_last_log_idx();
        require(driver.nodes.at(1)->get_last_log_idx() == fifth_index, "Follower did not receive interrupted write");
        require(driver.pending_responses.size() == 1, "Interrupted write has no unique pending response");
        require(!fifth_result->has_result(), "Undurable follower falsely completed append");
        auto durable_image = managers.at(1)->log->durableImage();
        require(durable_image->next_slot() == fifth_index, "Durable image did not discard exactly the volatile tail");
        stop(1);
        require(driver.pending_responses.empty(), "Stopped runtime retained deferred transport work");
        managers.at(1)->log = durable_image;
        driver.drain(3);
        require(!fifth_result->has_result(), "Stopping undurable follower falsely completed append");
        require(images.at(1)->values == std::vector<int32_t>({731, -47, 902, -613}), "Uncommitted value survived in applied image");
        driver.record("volatile-tail-lost 1 " + std::to_string(fifth_index));
        restart(1);
        require(driver.nodes.at(1)->get_last_log_idx() < fifth_index, "Restart restored the volatile entry");
        for (int attempt = 0; attempt < 8 && !fifth_result->has_result(); ++attempt)
        {
            timers.at(2)->fire(timer_task_type::heartbeat_timer);
            driver.drain(3);
        }
        require(fifth_result->has_result() && fifth_result->get_result_code() == cmd_result_code::OK,
            "Retransmission after volatile-tail loss did not commit");
        driver.drain();
        for (const auto & [id, machine] : machines)
            require(machine->values == std::vector<int32_t>({731, -47, 902, -613, 809}),
                "Interrupted-write recovery lost or duplicated committed values");
        driver.record("interrupted-write-replicas-exact");

        managers.at(1)->log->pauseDurability();
        auto abandoned_value = buffer::alloc(sizeof(int32_t));
        abandoned_value->put(int32_t{-1201});
        abandoned_value->pos(0);
        auto abandoned_result = driver.nodes.at(2)->append_entries({abandoned_value});
        require(abandoned_result->get_accepted() && !abandoned_result->has_result(), "Sender-stop append was not pending");
        driver.drain(3);
        require(driver.pending_responses.size() == 1, "Sender-stop control has no unique pending response");
        require(!abandoned_result->has_result(), "Sender-stop append completed without durable majority");
        stop(2);
        require(driver.pending_responses.empty(), "Stopped sender retained deferred transport work");
        const auto cancelled_before = driver.cancelled_response_completions;
        managers.at(1)->log->completeDurability();
        driver.nodes.at(1)->notify_log_append_completion(true);
        require(driver.cancelled_response_completions == cancelled_before + 1,
            "Late-completion control did not execute the cancelled response");
        driver.drain(3);
        require(!abandoned_result->has_result() || abandoned_result->get_result_code() != cmd_result_code::OK,
            "Destroyed leader falsely acknowledged interrupted append");
        for (const auto & [id, machine] : machines)
            require(machine->values == std::vector<int32_t>({731, -47, 902, -613, 809}),
                "Late deferred response applied an uncommitted value");
        driver.record("late-response-after-sender-destruction-safe");
    }

    // Queue traffic before shutdown; closing nodes must discard its callbacks.
    timers.at(recovery_scenario ? 3 : 1)->fire(
        recovery_scenario ? timer_task_type::election_timer : timer_task_type::heartbeat_timer);
    require(!driver.events.empty(), "Shutdown control has no queued traffic");
    for (const auto & [id, node] : driver.nodes)
        node->shutdown();
    require(driver.events.empty(), "Stale work remains after shutdown");
    require(driver.pending_responses.empty(), "Deferred response remains after shutdown");
    for (const auto & [id, timer] : timers)
        require(timer->empty(), "Timer remains after shutdown");
    driver.nodes.clear();
    require(!driver.step(), "Destroyed node callback executed");
    require(thread_attempts == 0 && liveThreads() == 1, "Uncontrolled thread during lifecycle");
    require(steady_reads > 0 && system_reads > 0, "Clock instrumentation missed direct clock use");
    require(!virtual_time || host_clock_reads == 0, "Virtual probe delegated to host clock");
    driver.finishReplay();
    driver.record("shutdown-clean");
    std::cout << "MEASURE thread_attempts=" << thread_attempts << " live_threads=" << liveThreads()
        << " steady_clock_reads=" << steady_reads << " system_clock_reads=" << system_reads
        << " host_clock_reads=" << host_clock_reads << '\n';
    std::cout << "PASS manual-core-probe clock_mode=" << (virtual_time ? "virtual" : "real")
        << "; FULL_CLICKHOUSE_DETERMINISTIC_GATE=BLOCKED\n";
}
#else
/// Preserve a real prefix or flush bytes before reporting failure; never roll back disk contents.
class FailingFileBuffer : public DB::WriteBufferFromFileDecorator
{
public:
    FailingFileBuffer(std::unique_ptr<DB::WriteBuffer> writer, std::string mode_, size_t & failures_, size_t & failed_file_bytes_)
        : DB::WriteBufferFromFileDecorator(std::move(writer)), mode(std::move(mode_)), failures(failures_), failed_file_bytes(failed_file_bytes_)
    {
    }

    void sync() override
    {
        if (mode == "sync")
        {
            next();
            failed_file_bytes = std::filesystem::file_size(getFileName());
            ++failures;
            throw std::runtime_error("simulated sync write failure");
        }
        DB::WriteBufferFromFileDecorator::sync();
    }

private:
    void nextImpl() override
    {
        if (mode == "partial")
        {
            const size_t prefix = std::min<size_t>(13, offset());
            {
                DB::SwapHelper swap(*this, *impl);
                impl->position() = impl->buffer().begin() + prefix;
                impl->next();
            }
            failed_file_bytes = std::filesystem::file_size(getFileName());
            ++failures;
            throw std::runtime_error("simulated partial write failure");
        }
        DB::SwapHelper swap(*this, *impl);
        impl->next();
    }

    const std::string mode;
    size_t & failures;
    size_t & failed_file_bytes;
};

class InterruptedWriteDisk : public DB::DiskLocal
{
public:
    InterruptedWriteDisk(const std::string & name_, const std::string & path) : DB::DiskLocal(name_, path)
    {
    }

    bool fail_next_state_write = false;
    bool fail_next_snapshot_write = false;
    size_t failures = 0;
    size_t failed_file_bytes = 0;

    std::unique_ptr<DB::WriteBufferFromFileBase> writeFile(
        const String & path, size_t size, DB::WriteMode mode, const DB::WriteSettings & settings) override
    {
        auto writer = DB::DiskLocal::writeFile(path, size, mode, settings);
        if (path == "state" && fail_next_state_write)
        {
            fail_next_state_write = false;
            if (metadata_fault == "partial")
                return std::make_unique<FailingFileBuffer>(std::move(writer), metadata_fault, failures, failed_file_bytes);
            ++failures;
            failed_file_bytes = 0;
            throw std::runtime_error("simulated interrupted state rewrite");
        }
        if (path.starts_with("snapshot_") && fail_next_snapshot_write)
        {
            if (snapshot_fault == "marker")
                return writer;
            fail_next_snapshot_write = false;
            if (snapshot_fault != "open")
                return std::make_unique<FailingFileBuffer>(std::move(writer), snapshot_fault, failures, failed_file_bytes);
            ++failures;
            failed_file_bytes = 0;
            throw std::runtime_error("simulated interrupted snapshot write");
        }
        return writer;
    }

    void removeFile(const String & path) override
    {
        if (snapshot_fault == "marker" && fail_next_snapshot_write && path.starts_with("tmp_snapshot_"))
        {
            fail_next_snapshot_write = false;
            failed_file_bytes = getFileSize(path.substr(4));
            ++failures;
            throw std::runtime_error("simulated snapshot publication failure");
        }
        DB::DiskLocal::removeFile(path);
    }
};

void runKeeperProbe(
    const std::filesystem::path & directory, bool lsmt, bool real_logs, int delay_ms,
    bool unsafe_quorum, bool unsafe_durability, bool unsafe_log_loss, bool unsafe_metadata_loss)
{
    require(std::filesystem::create_directory(directory), "Keeper state directory must be new");
    require(liveThreads() == 1 && thread_attempts == 0, "Keeper probe did not start single-threaded");
    const auto host_reads_before = host_clock_reads.load();
    Driver driver;
    driver.delay_ms = delay_ms;
    std::map<int, ptr<DB::KeeperStateMachine>> machines;
    std::map<int, DB::KeeperContextPtr> contexts;
    std::map<int, ptr<DB::SnapshotsQueue>> snapshots;
    std::map<int, ptr<ManualTimers>> timers;
    std::map<int, ptr<state_mgr>> managers;
    std::map<int, ptr<DB::KeeperLogStore>> disk_logs;
    std::set<std::pair<int, DB::Changelog::BackgroundTask>> queued_log_tasks;
    std::map<int, std::vector<DB::KeeperResponseForSession>> responses;
    std::map<int, int64_t> session_activity;
    auto factory = cs_new<ManualFactory>(driver);
    cluster_config config;
    for (int id = 1; id <= 3; ++id)
        config.get_servers().push_back(cs_new<srv_config>(id, std::to_string(id)));

    raft_params params;
    params.election_timeout_lower_bound_ = 150;
    params.election_timeout_upper_bound_ = 300;
    params.heart_beat_interval_ = 50;
    params.rpc_failure_backoff_ = 0;
    params.leadership_expiry_ = 0;
    params.return_method_ = raft_params::async_handler;
    params.use_bg_thread_for_urgent_commit_ = true;
    params.use_bg_thread_for_snapshot_io_ = false;
    params.snapshot_distance_ = 0;
    params.parallel_log_appending_ = !unsafe_durability;
    params.max_log_gap_in_stream_ = 8;
    if (unsafe_quorum)
        params.custom_commit_quorum_size_ = 1;

    auto make_context = [&](int id)
    {
        auto settings = std::make_shared<DB::CoordinationSettings>();
        (*settings)[DB::CoordinationSetting::use_lsmt_storage] = lsmt;
        (*settings)[DB::CoordinationSetting::storage_memory_only] = true;
        (*settings)[DB::CoordinationSetting::dead_session_check_period_ms] = 500;
        (*settings)[DB::CoordinationSetting::parallel_read_threads] = 0;
        auto keeper = std::make_shared<DB::KeeperContext>(true, settings, []
        {
            return logical_ms;
        });
        keeper->setLocalLogsPreprocessed();
        const auto node_directory = directory / std::to_string(id);
        keeper->setSnapshotDisk(std::make_shared<DB::DiskLocal>("SnapshotDisk", node_directory.string() + "/"));
        if (snapshot_write_failure_scenario)
            keeper->setSnapshotDisk(std::make_shared<InterruptedWriteDisk>("SnapshotDisk", node_directory.string() + "/"));
        if (real_logs)
        {
            const auto log_directory = node_directory / "logs";
            std::filesystem::create_directory(log_directory);
            keeper->setLogDisk(std::make_shared<DB::DiskLocal>("LogDisk", log_directory.string() + "/"));
            const auto metadata_directory = node_directory / "metadata";
            std::filesystem::create_directory(metadata_directory);
            keeper->setStateFileDisk(std::make_shared<InterruptedWriteDisk>("StateDisk", metadata_directory.string() + "/"));
        }
        return keeper;
    };

    auto make_persistent_manager = [&](int id, uint64_t snapshot_index)
    {
        auto store = cs_new<DB::KeeperLogStore>(
            DB::LogFileSettings{.force_sync = true, .compress_logs = false, .rotate_interval = 4, .startup_read_max_streams = 1},
            DB::FlushSettings{.max_flush_batch_size = 1, .min_time_between_fsyncs_ms = 0},
            DB::ReadAheadSettings{}, contexts.at(id), DB::Changelog::ExecutionMode::Manual);
        auto manager = cs_new<DB::KeeperStateManager>(id, std::to_string(id), 0, contexts.at(id), store);
        /// This probe compares the entire persisted prefix, including workloads exceeding 100 entries.
        manager->loadLogStore(snapshot_index, std::max<uint64_t>(100, snapshot_index));
        require(store->flush(), "Keeper startup log barrier failed");
        auto snapshot_config = machines.at(id)->getClusterConfig();
        auto log_config = manager->getLatestConfigFromLogStore();
        if (snapshot_config && (!log_config || snapshot_config->get_log_idx() > log_config->get_log_idx()))
            manager->save_config(*snapshot_config);
        else if (log_config)
            manager->save_config(*log_config);
        else
        {
            require(snapshot_index == 0, "Keeper recovery lost persisted configuration");
            manager->save_config(config);
        }
        require(thread_attempts == 0 && liveThreads() == 1, "Keeper log initialization created uncontrolled threads");
        disk_logs[id] = store;
        managers[id] = manager;
    };

    if (real_logs)
    {
        driver.poll_background = [&]
        {
            for (const auto & [id, store] : disk_logs)
            {
                for (const auto & [task, name] : {
                    std::pair{DB::Changelog::BackgroundTask::Write, "log-write"},
                    std::pair{DB::Changelog::BackgroundTask::Completion, "log-complete"},
                    std::pair{DB::Changelog::BackgroundTask::FileOperation, "log-file"}})
                {
                    if (store->hasPendingTask(task) && queued_log_tasks.emplace(id, task).second)
                    {
                        driver.enqueue(name, id, 0, [&, id, store, task]
                        {
                            queued_log_tasks.erase({id, task});
                            store->runTask(task);
                        });
                    }
                }
            }
        };
    }

    auto make_machine = [&](int id)
    {
        auto machine = cs_new<DB::KeeperStateMachine>([&, id](DB::KeeperResponseForSession response)
        {
            driver.record("keeper-response " + std::to_string(id) + " " + std::to_string(response.response->xid)
                + " " + std::to_string(static_cast<int>(response.response->error)));
            responses[id].push_back(std::move(response));
        }, *snapshots.at(id), contexts.at(id), nullptr, [&, id](uint64_t index, const DB::KeeperRequestForSession & request)
        {
            if (request.session_id == 1)
                session_activity[id] = logical_ms;
            driver.record("keeper-apply " + std::to_string(id) + " " + std::to_string(index)
                + " " + std::to_string(request.request->xid));
        });
        machine->init();
        if (machine->last_snapshot())
            session_activity[id] = logical_ms;
        require(liveThreads() == 1 && thread_attempts == 0,
            "Keeper storage initialization created uncontrolled threads: attempts=" + std::to_string(thread_attempts.load())
                + " live=" + std::to_string(liveThreads()));
        return machine;
    };

    for (int id = 1; id <= 3; ++id)
    {
        std::filesystem::create_directory(directory / std::to_string(id));
        contexts[id] = make_context(id);
        snapshots[id] = cs_new<DB::SnapshotsQueue>(1);
        machines[id] = make_machine(id);
        timers[id] = cs_new<ManualTimers>(id, driver);
        if (real_logs)
            make_persistent_manager(id, 0);
        else
        {
            managers[id] = cs_new<MemoryManager>(id);
            managers.at(id)->save_config(config);
        }
        driver.incarnations[id] = 1;
        driver.nodes[id] = cs_new<ManualServer>(new context(managers.at(id), machines.at(id), {}, nullptr,
            factory, timers.at(id), params, &driver));
        if (real_logs)
            disk_logs.at(id)->setRaftServer(driver.nodes.at(id));
    }
    for (const auto & [id, node] : driver.nodes)
        node->start_server(false);
    require(driver.events.empty(), "Keeper progressed before selected election");
    timers.at(1)->fire(timer_task_type::election_timer);
    driver.drain();
    require(driver.nodes.at(1)->is_leader(), "Keeper selected election failed");

    int leader = 1;
    auto append = [&](Coordination::ZooKeeperRequestPtr request, int64_t session)
    {
        DB::KeeperRequestForSession entry;
        entry.session_id = session;
        entry.time = logical_ms;
        entry.request = std::move(request);
        auto buffer = DB::KeeperStateMachine::getZooKeeperLogEntry(entry);
        auto result = driver.nodes.at(leader)->append_entries({buffer});
        require(result->get_accepted(), "Keeper leader rejected request");
        return result;
    };
    auto check = [&](int id, const std::string & path, const std::string & expected, bool exists)
    {
        std::string actual;
        auto & storage = machines.at(id)->getStorageUnsafe();
        require(storage.nodes_storage->getCommittedNodeSimple(path, nullptr, &actual) == exists,
            "Keeper committed node existence differs: " + path);
        if (exists)
            require(actual == expected, "Keeper committed payload differs: " + path);
    };
    auto succeeded = [](const auto & result)
    {
        require(result->has_result() && result->get_result_code() == cmd_result_code::OK, "Keeper Raft append did not complete");
    };

    auto session_request = std::make_shared<Coordination::ZooKeeperSessionIDRequest>();
    session_request->xid = 1;
    session_request->internal_id = 71;
    session_request->server_id = 1;
    session_request->session_timeout_ms = 30000;
    auto session_result = append(session_request, -1);
    driver.drain();
    succeeded(session_result);
    for (int id = 1; id <= 3; ++id)
    {
        require(responses[id].size() == 1, "Keeper session allocation response missing");
        const auto & response = dynamic_cast<const Coordination::ZooKeeperSessionIDResponse &>(*responses[id].front().response);
        require(response.session_id == 1 && response.internal_id == 71, "Keeper session identities differ");
    }
    driver.record("keeper-session-replicated");

    auto pause_durability = [&](int id)
    {
        if (real_logs)
            driver.paused_log_writes.insert(id);
        else
            std::static_pointer_cast<MemoryManager>(managers.at(id))->log->pauseDurability();
    };
    auto complete_durability = [&](int id)
    {
        if (real_logs)
            require(driver.paused_log_writes.erase(id) == 1, "Keeper log was not paused");
        else
        {
            std::static_pointer_cast<MemoryManager>(managers.at(id))->log->completeDurability();
            driver.nodes.at(id)->notify_log_append_completion(true);
        }
    };
    if (!unsafe_quorum)
        pause_durability(1);
    pause_durability(2);
    auto create = std::make_shared<Coordination::ZooKeeperCreateRequest>();
    create->xid = 2;
    create->path = "/experiment";
    create->data = "asymmetric-731";
    create->acls = {{31, "world", "anyone"}};
    auto create_result = append(create, 1);
    while (driver.step(0, true))
    {
    }
    require(!create_result->has_result(), "Keeper no-quorum append falsely completed");
    for (int id = 1; id <= 3; ++id)
        check(id, "/experiment", "", false);
    driver.drain(3);
    require(!create_result->has_result(), "Keeper undurable majority falsely completed append");
    require(driver.pending_responses.size() == 1, "Keeper follower did not defer durability response");
    for (int id = 1; id <= 3; ++id)
        check(id, "/experiment", "", false);
    complete_durability(2);
    driver.drain(3);
    require(!create_result->has_result(), "Keeper one durable voter falsely completed append");
    complete_durability(1);
    driver.drain(3);
    succeeded(create_result);
    check(1, "/experiment", "asymmetric-731", true);
    check(2, "/experiment", "asymmetric-731", true);
    check(3, "/experiment", "", false);
    driver.drain();
    for (int id = 1; id <= 3; ++id)
        check(id, "/experiment", "asymmetric-731", true);
    driver.record("keeper-durable-majority-and-catchup");

    auto update = std::make_shared<Coordination::ZooKeeperSetRequest>();
    update->xid = 3;
    update->path = "/experiment";
    update->data = "updated--47";
    update->version = 0;
    driver.lose_response_from = 2;
    auto update_result = append(update, 1);
    driver.drain(3);
    require(driver.lose_response_from == 0 && !update_result->has_result(), "Keeper response-loss fault was ineffective");
    for (int attempt = 0; attempt < 8 && !update_result->has_result(); ++attempt)
    {
        timers.at(1)->fire(timer_task_type::heartbeat_timer);
        driver.drain(3);
    }
    succeeded(update_result);
    driver.drain();
    for (int id = 1; id <= 3; ++id)
    {
        check(id, "/experiment", "updated--47", true);
        DB::KeeperNodeStats stats;
        require(machines.at(id)->getStorageUnsafe().nodes_storage->getCommittedNodeSimple("/experiment", &stats, nullptr)
            && stats.version == 1, "Keeper retransmission applied update more than once");
    }
    driver.record("keeper-lost-reply-update-exact");

    auto ephemeral = std::make_shared<Coordination::ZooKeeperCreateRequest>();
    ephemeral->xid = 4;
    ephemeral->path = "/ephemeral";
    ephemeral->data = "ephemeral-902";
    ephemeral->is_ephemeral = true;
    ephemeral->acls = {{31, "world", "anyone"}};
    auto ephemeral_result = append(ephemeral, 1);
    driver.drain();
    succeeded(ephemeral_result);
    for (int id = 1; id <= 3; ++id)
        check(id, "/ephemeral", "ephemeral-902", true);

    std::set<std::string> workload_paths;
    int64_t read_xid = 100000;
    auto observe_workload = [&](const std::string & phase)
    {
        if (!generated_workload)
            return;
        driver.record("workload-check " + phase);
        for (int id = 1; id <= 3; ++id)
        {
            for (const auto & path : workload_paths)
            {
                auto read = [&](Coordination::ZooKeeperRequestPtr request)
                {
                    request->xid = read_xid++;
                    const auto count = responses.at(id).size();
                    machines.at(id)->processReadRequests({DB::KeeperRequestForSession{.session_id = 2, .request = request}});
                    require(liveThreads() == 1 && thread_attempts == 0, "Keeper generated read created uncontrolled threads");
                    require(responses.at(id).size() == count + 1, "Keeper generated read response missing or duplicated");
                    auto response = std::move(responses.at(id).back());
                    responses.at(id).pop_back();
                    require(response.session_id == 2 && response.response->xid == request->xid,
                        "Keeper generated read response identity differs");
                    return response.response;
                };
                auto get = std::make_shared<Coordination::ZooKeeperGetRequest>();
                get->path = path;
                auto value = std::dynamic_pointer_cast<Coordination::ZooKeeperGetResponse>(read(get));
                require(value != nullptr, "Keeper generated get response type differs");
                driver.record("workload-get " + std::to_string(id) + " " + path + " " + std::to_string(static_cast<int>(value->error))
                    + " " + (value->data.empty() ? "-" : value->data) + " " + std::to_string(value->stat.version)
                    + " " + std::to_string(value->stat.ephemeralOwner));
                auto list = std::make_shared<Coordination::ZooKeeperListRequest>();
                list->path = path;
                auto children = std::dynamic_pointer_cast<Coordination::ZooKeeperListResponse>(read(list));
                require(children != nullptr, "Keeper generated list response type differs");
                std::sort(children->names.begin(), children->names.end());
                std::string names;
                for (const auto & name : children->names)
                    names += " " + name;
                driver.record("workload-list " + std::to_string(id) + " " + path + " " + std::to_string(static_cast<int>(children->error))
                    + " " + std::to_string(children->names.size()) + names);
            }
        }
        driver.record("workload-checked " + phase);
    };
    if (generated_workload)
    {
        auto observer = std::make_shared<Coordination::ZooKeeperSessionIDRequest>();
        observer->xid = 90;
        observer->internal_id = 72;
        observer->server_id = 1;
        observer->session_timeout_ms = 90000;
        auto observer_result = append(observer, -1);
        driver.drain();
        succeeded(observer_result);
        for (int id = 1; id <= 3; ++id)
        {
            require(responses.at(id).size() == 5, "Keeper observer session response missing or duplicated");
            const auto & response = dynamic_cast<const Coordination::ZooKeeperSessionIDResponse &>(*responses.at(id).back().response);
            require(response.session_id == 2 && response.internal_id == 72, "Keeper observer session identity differs");
            responses.at(id).pop_back();
        }
        struct Operation
        {
            std::string kind;
            std::string path;
            std::string data;
            int32_t version = -1;
            bool ephemeral = false;
        };
        std::vector<Operation> operations{
            {"create", "/generated", "root-83"},
            {"create", "/generated/a", "first-129"},
            {"create", "/generated/a", "duplicate-71"},
            {"set", "/generated/a", "updated-937", 0},
            {"set", "/generated/a", "stale-44", 0},
            {"create", "/generated/a/c", "child-267"},
            {"remove", "/generated/a", "-"},
            {"remove", "/generated/a/c", "-", 1},
            {"remove", "/generated/a/c", "-", 0},
            {"remove", "/generated/missing", "-"},
            {"create", "/generated/e", "ephemeral-593", -1, true},
            {"create", "/generated/e/c", "forbidden-651"},
        };
        std::mt19937 workload_rng(random_seed ^ 0x9e3779b9U);
        for (size_t index = 0; index < 64; ++index)
        {
            const auto kind = workload_rng() % 3;
            std::string path = "/generated/k" + std::to_string(workload_rng() % 5);
            if (workload_rng() % 4 == 0)
                path += "/c";
            operations.push_back({kind == 0 ? "create" : (kind == 1 ? "set" : "remove"), path,
                "payload-" + std::to_string(index) + "-" + std::to_string(workload_rng()),
                static_cast<int32_t>(workload_rng() % 4) - 1, workload_rng() % 3 == 0});
        }
        auto make_request = [](const Operation & operation) -> Coordination::ZooKeeperRequestPtr
        {
            if (operation.kind == "create")
            {
                auto create_request = std::make_shared<Coordination::ZooKeeperCreateRequest>();
                create_request->path = operation.path;
                create_request->data = operation.data;
                create_request->is_ephemeral = operation.ephemeral;
                create_request->acls = {{31, "world", "anyone"}};
                return create_request;
            }
            else if (operation.kind == "set")
            {
                auto set_request = std::make_shared<Coordination::ZooKeeperSetRequest>();
                set_request->path = operation.path;
                set_request->data = operation.data;
                set_request->version = operation.version;
                return set_request;
            }
            else if (operation.kind == "check")
            {
                auto check_request = std::make_shared<Coordination::ZooKeeperCheckRequest>();
                check_request->path = operation.path;
                check_request->version = operation.version;
                return check_request;
            }
            else
            {
                auto remove_request = std::make_shared<Coordination::ZooKeeperRemoveRequest>();
                remove_request->path = operation.path;
                remove_request->version = operation.version;
                return remove_request;
            }
        };
        auto record_operation = [&](const std::string & prefix, const Operation & operation)
        {
            workload_paths.insert(operation.path);
            const auto parent = operation.path.substr(0, operation.path.rfind('/'));
            if (parent != "")
                workload_paths.insert(parent);
            driver.record(prefix + " " + operation.kind + " " + operation.path
                + " " + operation.data + " " + std::to_string(operation.version) + " " + std::to_string(operation.ephemeral));
        };
        auto record_response = [&](int id, const Coordination::ZooKeeperRequestPtr & request, const DB::KeeperResponseForSession & response)
        {
            require(response.session_id == 1 && response.response->xid == request->xid,
                "Keeper generated write response identity differs");
            driver.record("workload-result " + std::to_string(id) + " " + std::to_string(request->xid)
                + " " + std::to_string(static_cast<int>(response.response->error)));
            if (request->getOpNum() == Coordination::OpNum::Multi)
            {
                auto multi_response = std::dynamic_pointer_cast<Coordination::ZooKeeperMultiResponse>(response.response);
                require(multi_response != nullptr, "Keeper generated multi response type differs");
                std::string suberrors;
                for (const auto & subresponse : multi_response->responses)
                    suberrors += " " + std::to_string(static_cast<int>(subresponse->error));
                driver.record("workload-multi-result " + std::to_string(id) + " " + std::to_string(request->xid)
                    + " " + std::to_string(multi_response->responses.size()) + suberrors);
            }
        };
        auto submit_and_observe = [&](const Coordination::ZooKeeperRequestPtr & request)
        {
            std::array<size_t, 4> counts{};
            for (int id = 1; id <= 3; ++id)
                counts[id] = responses.at(id).size();
            auto result = append(request, 1);
            driver.drain();
            succeeded(result);
            for (int id = 1; id <= 3; ++id)
            {
                require(responses.at(id).size() == counts[id] + 1, "Keeper generated write response missing or duplicated");
                const auto response = std::move(responses.at(id).back());
                responses.at(id).pop_back();
                record_response(id, request, response);
            }
            observe_workload("step-" + std::to_string(request->xid));
        };
        int64_t xid = 100;
        for (const auto & operation : operations)
        {
            auto request = make_request(operation);
            request->xid = xid++;
            record_operation("workload-submit " + std::to_string(request->xid), operation);
            submit_and_observe(request);
        }
        if (multi_workload)
        {
            std::vector<std::vector<Operation>> transactions{
                {{"create", "/generated/txn", "parent-82"}, {"create", "/generated/txn/a", "child-613"},
                    {"set", "/generated/txn/a", "inside-941", 0}},
                {{"set", "/generated/txn/a", "must-rollback-431", 1}, {"check", "/generated/txn/a", "-", 99},
                    {"create", "/generated/rolled-back", "unexecuted-523"}},
                {{"remove", "/generated/txn/a", "-", 1}, {"create", "/generated/txn/a", "replacement-719"},
                    {"check", "/generated/txn/a", "-", 0}},
                {{"create", "/generated/txn/e", "ephemeral-329", -1, true}, {"create", "/generated/txn/e/c", "forbidden-97"}},
                {{"remove", "/generated/txn/a", "-", 0}, {"remove", "/generated/txn", "-"}},
                {{"create", "/generated/txn2", "parent-283"}, {"create", "/generated/txn2/a", "first-151"},
                    {"create", "/generated/txn2/a", "duplicate-557"}},
                {{"check", "/generated/txn2", "-"}, {"create", "/generated/unexecuted", "unexecuted-887"}},
                {{"create", "/generated/txn3", "survivor-563"}, {"create", "/generated/txn3/e", "ephemeral-271", -1, true}},
                {{"set", "/generated/txn3", "must-rollback-647", 0}, {"remove", "/generated/txn3", "-"}},
                {},
            };
            for (size_t index = 0; index < 24; ++index)
            {
                const std::string path = "/generated/t" + std::to_string(index);
                const std::string payload = "transaction-" + std::to_string(workload_rng());
                switch (workload_rng() % 4)
                {
                    case 0:
                        transactions.push_back({{"create", path, payload}, {"create", path + "/c", "child-173"},
                            {"set", path + "/c", "changed-859", 0}});
                        break;
                    case 1:
                        transactions.push_back({{"create", path, payload}, {"create", path + "/c", "child-353"},
                            {"set", path + "/c", "wrong-version-619", 1}});
                        break;
                    case 2:
                        transactions.push_back({{"set", "/generated/a", payload}, {"check", path, "-"},
                            {"create", path, "unexecuted-367"}});
                        break;
                    case 3:
                        transactions.push_back({{"create", path, payload, -1, true}, {"create", path + "/c", "forbidden-743"},
                            {"check", path, "-", 0}});
                        break;
                }
            }
            for (const auto & transaction : transactions)
            {
                auto request = std::make_shared<Coordination::ZooKeeperMultiRequest>();
                request->xid = xid++;
                driver.record("workload-multi " + std::to_string(request->xid) + " " + std::to_string(transaction.size()));
                for (size_t index = 0; index < transaction.size(); ++index)
                {
                    record_operation("workload-part " + std::to_string(request->xid) + " " + std::to_string(index), transaction[index]);
                    request->requests.push_back(make_request(transaction[index]));
                }
                submit_and_observe(request);
            }
        }
        if (pipelined_workload)
        {
            if (!pipeline_fault.empty())
                driver.record("workload-pipeline-faults " + pipeline_fault);
            if (pipeline_fault_batch)
                driver.record("workload-pipeline-fault-batch " + std::to_string(pipeline_fault_batch));
            for (size_t batch = 0; batch < 8; ++batch)
            {
                /// Reuse the abandoned namespace to expose stale preprocessing after log conflict.
                const std::string path = "/generated/p" + std::to_string(
                    pipeline_fault == "leader-partition" && batch == pipeline_fault_batch + 1 ? pipeline_fault_batch : batch);
                std::vector<std::vector<Operation>> transactions{
                    {{"create", path, "pipeline-parent-607"}, {"create", path + "/a", "pipeline-initial-419"}},
                    {{"set", path + "/a", "pipeline-rollback-683", 0}, {"check", path + "/missing", "-"}},
                    {{"check", path + "/a", "-", 0}, {"set", path + "/a", "pipeline-committed-821", 0},
                        {"create", path + "/e", "pipeline-ephemeral-257", -1, true}},
                    batch % 2 == 0
                        ? std::vector<Operation>{{"remove", path + "/e", "-", 0}, {"set", path + "/a", "pipeline-final-953", 1}}
                        : std::vector<Operation>{{"remove", path + "/e", "-", 0}, {"create", path + "/a", "duplicate-163"},
                            {"set", path + "/a", "unexecuted-487"}},
                };
                driver.record("workload-pipeline " + std::to_string(batch) + " 4");
                const auto committed_before = machines.at(1)->last_commit_index();
                const bool conflicting_tail = (pipeline_fault == "leader-partition" || pipeline_fault == "leader-prefix-partition")
                    && batch == pipeline_fault_batch;
                const bool partial_survivor = (pipeline_fault == "leader-prefix" || pipeline_fault == "leader-prefix-partition")
                    && batch == pipeline_fault_batch;
                const bool replace_leader = (pipeline_fault == "leader-failover" || pipeline_fault == "leader-prefix")
                    && batch == pipeline_fault_batch;
                const bool restart_pending = (pipeline_fault == "restart" && batch < 2) || replace_leader;
                const int restart_node = replace_leader ? 1 : 3;
                if (restart_pending)
                {
                    bool completed = false;
                    snapshot checkpoint(committed_before, disk_logs.at(restart_node)->term_at(committed_before), machines.at(restart_node)->getClusterConfig());
                    async_result<bool>::handler_type done = [&](bool & success, ptr<std::exception> & error)
                    {
                        require(success && !error && !completed, "Keeper pipeline checkpoint failed or repeated");
                        completed = true;
                    };
                    machines.at(restart_node)->create_snapshot(checkpoint, done);
                    DB::CreateSnapshotTask checkpoint_task;
                    require(!completed && snapshots.at(restart_node)->tryPop(checkpoint_task), "Keeper pipeline checkpoint was not queued");
                    driver.enqueue("keeper-pipeline-checkpoint", restart_node, 0, [&]
                    {
                        auto file = checkpoint_task.create_snapshot(std::move(checkpoint_task.snapshot), false);
                        require(file && completed, "Keeper pipeline checkpoint was not published");
                        checkpoint_task.create_snapshot = {};
                    });
                    driver.drain();
                }
                std::array<size_t, 4> counts{};
                for (int id = 1; id <= 3; ++id)
                    counts[id] = responses.at(id).size();
                const int pending_follower = restart_pending && !replace_leader ? 3 : 2;
                const int withheld_follower = restart_pending && !replace_leader ? 2 : 3;
                if (!conflicting_tail)
                    pause_durability(1);
                if (!conflicting_tail || partial_survivor)
                    pause_durability(pending_follower);
                const auto lost_before = driver.lost_responses;
                if (pipeline_fault == "reply-loss")
                {
                    driver.lose_response_from = 2;
                    driver.lose_response_after_durability = true;
                }
                std::vector<Coordination::ZooKeeperRequestPtr> requests;
                std::vector<decltype(append(Coordination::ZooKeeperRequestPtr{}, 1))> results;
                for (const auto & transaction : transactions)
                {
                    auto request = std::make_shared<Coordination::ZooKeeperMultiRequest>();
                    request->xid = xid++;
                    driver.record("workload-multi " + std::to_string(request->xid) + " " + std::to_string(transaction.size()));
                    for (size_t index = 0; index < transaction.size(); ++index)
                    {
                        record_operation("workload-part " + std::to_string(request->xid) + " " + std::to_string(index), transaction[index]);
                        request->requests.push_back(make_request(transaction[index]));
                    }
                    requests.push_back(request);
                    results.push_back(append(request, 1));
                    if (partial_survivor && requests.size() == 2)
                    {
                        driver.drain(withheld_follower);
                        require(disk_logs.at(2)->next_slot() == committed_before + 3,
                            "Keeper partial survivor did not receive the first two requests");
                    }
                }
                if (conflicting_tail || partial_survivor)
                {
                    while (driver.step(0, true))
                    {
                    }
                }
                else
                    driver.drain(pipeline_fault.empty() ? 0 : withheld_follower);
                for (const auto & result : results)
                    require(!result->has_result(), "Keeper pipelined transaction committed without durable quorum");
                for (int id = 1; id <= 3; ++id)
                    require(machines.at(id)->last_commit_index() == committed_before && responses.at(id).size() == counts[id],
                        "Keeper pipelined transaction applied before durable quorum");
                observe_workload("pipeline-before-" + std::to_string(batch));
                if (partial_survivor)
                {
                    complete_durability(2);
                    driver.drain(1);
                    require(disk_logs.at(2)->last_durable_index() == committed_before + 2
                        && disk_logs.at(2)->next_slot() == committed_before + 3
                        && disk_logs.at(3)->next_slot() == committed_before + 1,
                        "Keeper partial survivor did not persist exactly the first two requests");
                    for (const auto & result : results)
                        require(!result->has_result(), "Keeper partial survivor completed before leader isolation");
                    for (int id = 1; id <= 3; ++id)
                        require(machines.at(id)->last_commit_index() == committed_before && responses.at(id).size() == counts[id],
                            "Keeper partial survivor applied before leader isolation");
                    driver.record("keeper-pipeline-surviving-prefix " + std::to_string(batch) + " 2 "
                        + std::to_string(committed_before + 2));
                }
                if (conflicting_tail)
                {
                    require(driver.nodes.at(1)->is_leader() && disk_logs.at(1)->last_durable_index() == committed_before + 4
                        && disk_logs.at(2)->next_slot() == committed_before + (partial_survivor ? 3 : 1)
                        && disk_logs.at(3)->next_slot() == committed_before + 1,
                        "Keeper partition did not isolate a durable minority tail");
                    const auto previous_term = driver.nodes.at(1)->get_term();
                    const auto old_server = driver.nodes.at(1);
                    const auto old_machine = machines.at(1);
                    std::vector<std::string> retained_prefix;
                    if (partial_survivor)
                    {
                        for (uint64_t index = 1; index <= committed_before + 2; ++index)
                        {
                            auto entry = disk_logs.at(1)->entry_at(index);
                            require(entry != nullptr, "Keeper live leader prefix missing");
                            auto bytes = entry->serialize();
                            retained_prefix.emplace_back(reinterpret_cast<const char *>(bytes->data_begin()), bytes->size());
                        }
                    }
                    driver.record("workload-pipeline-fault " + std::to_string(batch) + " " + pipeline_fault);
                    timers.at(3)->fire(timer_task_type::election_timer);
                    driver.drain(1);
                    for (int attempt = 0; attempt < 8 && !driver.nodes.at(2)->is_leader(); ++attempt)
                    {
                        timers.at(2)->fire(timer_task_type::election_timer);
                        driver.drain(1);
                    }
                    require(driver.nodes.at(2)->is_leader() && driver.nodes.at(2)->get_term() > previous_term
                        && driver.nodes.at(1)->is_leader() && driver.nodes.at(1)->get_term() == previous_term,
                        "Keeper partition did not elect a replacement behind the isolated leader");
                    leader = 2;
                    const auto replacement_index = committed_before + (partial_survivor ? 3 : 1);
                    require(driver.nodes.at(leader)->get_last_log_idx() == replacement_index
                        && machines.at(2)->last_commit_index() == replacement_index
                        && machines.at(3)->last_commit_index() == replacement_index,
                        "Keeper partition did not commit the conflicting new-term configuration");
                    driver.record("keeper-pipeline-leader-replaced 1 2 " + std::to_string(driver.nodes.at(leader)->get_term()));
                    driver.record("keeper-pipeline-partition-healed 1");
                    driver.drain();
                    for (int attempt = 0; attempt < 8 && machines.at(1)->last_commit_index() != replacement_index; ++attempt)
                    {
                        timers.at(leader)->fire(timer_task_type::heartbeat_timer);
                        driver.drain();
                    }
                    require(driver.nodes.at(1) == old_server && machines.at(1) == old_machine
                        && driver.incarnations.at(1) == 1 && !driver.nodes.at(1)->is_leader()
                        && machines.at(1)->last_commit_index() == replacement_index
                        && disk_logs.at(1)->next_slot() == replacement_index + 1
                        && disk_logs.at(1)->last_durable_index() == replacement_index,
                        "Keeper live former leader did not replace its conflicting tail");
                    for (size_t index = 0; index < retained_prefix.size(); ++index)
                    {
                        auto entry = disk_logs.at(1)->entry_at(index + 1);
                        require(entry != nullptr, "Keeper healed live leader prefix missing");
                        auto bytes = entry->serialize();
                        require(std::string(reinterpret_cast<const char *>(bytes->data_begin()), bytes->size()) == retained_prefix[index],
                            "Keeper healed live leader changed the retained prefix");
                    }
                    for (int id = 1; id <= 3; ++id)
                        require(responses.at(id).size() == counts[id] + (partial_survivor ? 2 : 0),
                            "Keeper applied an abandoned minority transaction or lost a surviving prefix");
                    driver.record("keeper-pipeline-conflicting-tail-replaced 1 " + std::to_string(replacement_index)
                        + " " + std::to_string(committed_before + 4));
                    if (!partial_survivor)
                        driver.record("workload-pipeline-aborted " + std::to_string(batch));
                }
                if (restart_pending)
                {
                    if (replace_leader)
                    {
                        if (!partial_survivor)
                        {
                            complete_durability(2);
                            driver.drain(3);
                        }
                        const size_t surviving = partial_survivor ? 2 : 4;
                        require(disk_logs.at(2)->last_durable_index() == committed_before + surviving
                            && disk_logs.at(2)->next_slot() == committed_before + surviving + 1,
                            "Keeper failover survivor did not persist the expected outstanding prefix");
                        for (const auto & result : results)
                            require(!result->has_result(), "Keeper failover batch completed before leader failure");
                        for (int id = 1; id <= 3; ++id)
                            require(machines.at(id)->last_commit_index() == committed_before && responses.at(id).size() == counts[id],
                                "Keeper failover batch applied before leader failure");
                    }
                    require(disk_logs.at(restart_node)->next_slot() == committed_before + 5
                        && disk_logs.at(restart_node)->last_durable_index() == committed_before
                        && (replace_leader || !driver.pending_responses.empty()), "Keeper pipeline restart had no outstanding unwritten batch");
                    auto saved_state = managers.at(restart_node)->read_state()->serialize();
                    std::vector<std::string> saved_prefix;
                    for (uint64_t index = 1; index <= committed_before; ++index)
                    {
                        auto entry = disk_logs.at(restart_node)->entry_at(index);
                        require(entry != nullptr, "Keeper pipeline original log prefix missing");
                        auto bytes = entry->serialize();
                        saved_prefix.emplace_back(reinterpret_cast<const char *>(bytes->data_begin()), bytes->size());
                    }
                    const auto previous_term = driver.nodes.at(leader)->get_term();
                    std::weak_ptr<ManualServer> old_server = driver.nodes.at(restart_node);
                    std::weak_ptr<DB::KeeperStateMachine> old_machine = machines.at(restart_node);
                    std::weak_ptr<DB::KeeperContext> old_context = contexts.at(restart_node);
                    std::weak_ptr<DB::KeeperLogStore> old_log = disk_logs.at(restart_node);
                    std::weak_ptr<state_mgr> old_manager = managers.at(restart_node);
                    driver.nodes.at(restart_node)->shutdown();
                    driver.nodes.erase(restart_node);
                    machines.erase(restart_node);
                    require(disk_logs.at(restart_node)->discardUnexecutedWritesForTesting() >= 5,
                        "Keeper pipeline restart did not discard the queued batch");
                    std::erase_if(queued_log_tasks, [&](const auto & item)
                    {
                        return item.first == restart_node;
                    });
                    managers.erase(restart_node);
                    disk_logs.erase(restart_node);
                    require(old_server.expired() && old_machine.expired() && old_log.expired() && old_manager.expired()
                        && timers.at(restart_node)->empty(), "Keeper pipeline restart retained old runtime");
                    if (replace_leader)
                    {
                        driver.record("workload-pipeline-fault " + std::to_string(batch) + " " + pipeline_fault);
                        complete_durability(1);
                        timers.at(3)->fire(timer_task_type::election_timer);
                        driver.drain();
                        for (int attempt = 0; attempt < 8 && !driver.nodes.at(2)->is_leader(); ++attempt)
                        {
                            timers.at(2)->fire(timer_task_type::election_timer);
                            driver.drain();
                        }
                        require(driver.nodes.at(2)->is_leader() && !driver.nodes.at(3)->is_leader()
                            && driver.nodes.at(2)->get_term() > previous_term, "Keeper outstanding failover did not elect a new leader");
                        leader = 2;
                        driver.record("keeper-pipeline-leader-replaced 1 2 " + std::to_string(driver.nodes.at(2)->get_term()));
                        if (partial_survivor)
                            require(machines.at(2)->last_commit_index() == committed_before + 3
                                && machines.at(3)->last_commit_index() == committed_before + 3,
                                "Keeper partial survivor did not commit exactly its prefix and new-term configuration");
                        for (const auto & result : results)
                            require(!result->has_result() || result->get_result_code() != cmd_result_code::OK,
                                "Keeper destroyed leader acknowledged an outstanding transaction");
                    }
                    contexts[restart_node] = make_context(restart_node);
                    require(old_context.expired(), "Keeper pipeline restart retained old context");
                    machines[restart_node] = make_machine(restart_node);
                    require(machines.at(restart_node)->last_commit_index() == committed_before,
                        "Keeper pipeline restart did not restore the pre-batch snapshot");
                    make_persistent_manager(restart_node, committed_before);
                    auto restored_state = managers.at(restart_node)->read_state();
                    require(restored_state != nullptr, "Keeper pipeline restart lost persisted term/vote");
                    auto state_bytes = restored_state->serialize();
                    require(state_bytes->size() == saved_state->size()
                        && std::equal(saved_state->data_begin(), saved_state->data_begin() + saved_state->size(), state_bytes->data_begin()),
                        "Keeper pipeline restart changed persisted term/vote");
                    require(disk_logs.at(restart_node)->next_slot() == committed_before + 1, "Keeper pipeline restart retained unwritten entries");
                    for (uint64_t index = 1; index <= committed_before; ++index)
                    {
                        auto entry = disk_logs.at(restart_node)->entry_at(index);
                        require(entry != nullptr, "Keeper pipeline restored log prefix missing");
                        auto bytes = entry->serialize();
                        require(std::string(reinterpret_cast<const char *>(bytes->data_begin()), bytes->size()) == saved_prefix.at(index - 1),
                            "Keeper pipeline restart changed persisted prefix");
                    }
                    ++driver.incarnations.at(restart_node);
                    timers[restart_node] = cs_new<ManualTimers>(restart_node, driver);
                    driver.nodes[restart_node] = cs_new<ManualServer>(new context(managers.at(restart_node), machines.at(restart_node), {}, nullptr,
                        factory, timers.at(restart_node), params, &driver));
                    driver.nodes.at(restart_node)->start_server(false);
                    disk_logs.at(restart_node)->setRaftServer(driver.nodes.at(restart_node));
                    driver.drain(replace_leader ? 0 : 2);
                    require(driver.pending_responses.empty(), "Keeper pipeline restart retained deferred responses");
                    driver.record("keeper-pipeline-prefix-recovered " + std::to_string(restart_node) + " " + std::to_string(committed_before));
                    if (!replace_leader)
                    {
                        for (const auto & result : results)
                            require(!result->has_result(), "Keeper pipeline completed during follower restart");
                        driver.record("workload-pipeline-fault " + std::to_string(batch) + " restart");
                        observe_workload("pipeline-recovered-" + std::to_string(batch));
                    }
                }
                if (!replace_leader && !conflicting_tail)
                    complete_durability(pending_follower);
                if (pipeline_fault == "reply-loss")
                {
                    /// Keep the leader unwritten and the other voter withheld until the reply is lost.
                    driver.drain(3);
                    require(driver.lost_responses == lost_before + 1 && driver.lose_response_from == 0,
                        "Keeper pipeline reply-loss fault did not execute exactly once");
                    for (const auto & result : results)
                        require(!result->has_result(), "Keeper pipeline completed before the selected reply loss");
                    for (int id = 1; id <= 3; ++id)
                        require(machines.at(id)->last_commit_index() == committed_before && responses.at(id).size() == counts[id],
                            "Keeper pipeline applied before the selected reply loss");
                    driver.record("workload-pipeline-fault " + std::to_string(batch) + " reply-loss");
                }
                if (!replace_leader && !conflicting_tail)
                    complete_durability(1);
                driver.drain();
                if (!pipeline_fault.empty())
                {
                    const auto batch_end = driver.nodes.at(leader)->get_last_log_idx();
                    for (int attempt = 0; attempt < 8 && std::ranges::any_of(machines, [&](const auto & item)
                        {
                            return item.second->last_commit_index() != batch_end;
                        }); ++attempt)
                    {
                        timers.at(leader)->fire(timer_task_type::heartbeat_timer);
                        driver.drain();
                    }
                }
                for (size_t index = 0; index < results.size(); ++index)
                {
                    const auto & result = results[index];
                    if (replace_leader || conflicting_tail)
                    {
                        if (result->has_result() && result->get_result_code() == cmd_result_code::OK)
                        {
                            require(conflicting_tail && partial_survivor && index < 2,
                                "Keeper acknowledged a discarded transaction " + std::to_string(requests[index]->xid));
                            driver.record("workload-pipeline-client-committed " + std::to_string(requests[index]->xid));
                        }
                        else
                            driver.record("keeper-pipeline-client-unknown " + std::to_string(requests[index]->xid));
                    }
                    else
                        succeeded(result);
                }
                if (partial_survivor)
                    driver.record("workload-pipeline-prefix " + std::to_string(batch) + " 2");
                for (int id = 1; id <= 3; ++id)
                {
                    if (conflicting_tail && !partial_survivor)
                    {
                        require(responses.at(id).size() == counts[id], "Keeper later applied an abandoned minority transaction");
                        continue;
                    }
                    const size_t completed = partial_survivor ? 2 : requests.size();
                    require(responses.at(id).size() == counts[id] + completed, "Keeper pipeline response count differs");
                    for (size_t index = 0; index < completed; ++index)
                        record_response(id, requests[index], responses.at(id)[counts[id] + index]);
                    responses.at(id).resize(counts[id]);
                }
                observe_workload("pipeline-after-" + std::to_string(batch));
                driver.record("workload-pipeline-finished " + std::to_string(batch));
            }
        }
        driver.record("workload-generated " + std::to_string(xid - 100));
    }

    const auto snapshot_index = machines.at(3)->last_commit_index();
    snapshot meta(snapshot_index, managers.at(3)->load_log_store()->term_at(snapshot_index), machines.at(3)->getClusterConfig());
    const auto previous_snapshot = machines.at(3)->last_snapshot();
    if (snapshot_write_failure_scenario)
    {
        std::set<std::string> previous_files;
        for (const auto & entry : std::filesystem::directory_iterator(directory / "3"))
            if (entry.path().filename().string().starts_with("snapshot_"))
                previous_files.insert(entry.path().filename().string());
        size_t failed_callbacks = 0;
        async_result<bool>::handler_type failed = [&](bool & success, ptr<std::exception> &)
        {
            require(!success, "Keeper failed snapshot was reported successful");
            require(++failed_callbacks == 1, "Keeper failed snapshot callback repeated");
        };
        machines.at(3)->create_snapshot(meta, failed);
        require(failed_callbacks == 0 && machines.at(3)->last_snapshot() == previous_snapshot, "Keeper failed snapshot progressed before selection");
        DB::CreateSnapshotTask failed_task;
        require(snapshots.at(3)->tryPop(failed_task), "Keeper failed snapshot did not queue work");
        driver.enqueue("keeper-snapshot-write-failure", 3, 0, [&]
        {
            auto disk = std::static_pointer_cast<InterruptedWriteDisk>(contexts.at(3)->getSnapshotDisk());
            const auto failures_before = disk->failures;
            disk->fail_next_snapshot_write = !unsafe_snapshot_write_control;
            auto file = failed_task.create_snapshot(std::move(failed_task.snapshot), false);
            failed_task.create_snapshot = {};
            require(!file && failed_callbacks == 1 && machines.at(3)->last_snapshot() == previous_snapshot, "Keeper failed snapshot was published");
            require(!disk->fail_next_snapshot_write, "Keeper snapshot fault did not intercept a write");
            require(disk->failures == failures_before + 1, "Keeper snapshot failure did not reach the selected disk boundary");
            require((snapshot_fault == "open" && disk->failed_file_bytes == 0)
                || (snapshot_fault == "partial" && disk->failed_file_bytes == 13)
                || ((snapshot_fault == "sync" || snapshot_fault == "marker") && disk->failed_file_bytes > 13),
                "Keeper snapshot fault left an unexpected byte count");
            driver.record("keeper-snapshot-disk-fault " + snapshot_fault + " " + std::to_string(disk->failed_file_bytes));
            for (const auto & entry : std::filesystem::directory_iterator(directory / "3"))
            {
                const auto name = entry.path().filename().string();
                require(!name.starts_with("tmp_snapshot_") && (!name.starts_with("snapshot_") || previous_files.erase(name) == 1),
                    "Keeper failed snapshot left partial artifacts");
            }
            require(previous_files.empty(), "Keeper failed snapshot removed an earlier successful snapshot");
            driver.record("keeper-snapshot-failed-write-cleaned 3");
        });
        driver.drain();
        check(3, "/experiment", "updated--47", true);
        check(3, "/ephemeral", "ephemeral-902", true);
    }
    bool snapshot_completed = false;
    async_result<bool>::handler_type when_done = [&](bool & success, ptr<std::exception> & error)
    {
        require(success && !error, "Keeper snapshot task failed");
        require(!snapshot_completed, "Keeper snapshot callback repeated");
        snapshot_completed = true;
    };
    machines.at(3)->create_snapshot(meta, when_done);
    require(!snapshot_completed && machines.at(3)->last_snapshot() == previous_snapshot, "Keeper snapshot published before selected work");
    DB::CreateSnapshotTask task;
    require(snapshots.at(3)->tryPop(task), "Keeper did not queue snapshot work");
    driver.enqueue("keeper-snapshot", 3, 0, [&]
    {
        auto file = task.create_snapshot(std::move(task.snapshot), false);
        require(file && snapshot_completed, "Keeper snapshot was not published by selected work");
        task.create_snapshot = {};
    });
    driver.drain();
    require(machines.at(3)->last_snapshot()->get_last_log_idx() == snapshot_index, "Keeper snapshot index differs");
    size_t workload_restarts = 0;
    auto restart_follower = [&](bool discard_unwritten)
    {
        std::weak_ptr<ManualServer> old_server = driver.nodes.at(3);
        std::weak_ptr<DB::KeeperStateMachine> old_machine = machines.at(3);
        std::weak_ptr<DB::KeeperContext> old_context = contexts.at(3);
        std::vector<std::string> saved_entries;
        ptr<buffer> saved_state;
        ptr<buffer> saved_config;
        if (real_logs)
        {
            auto state = managers.at(3)->read_state();
            require(state && state->get_term() == driver.nodes.at(3)->get_term() && state->get_term() > 0,
                "Keeper current term was not persisted");
            saved_state = state->serialize();
            saved_config = managers.at(3)->load_config()->serialize();
            require(disk_logs.at(3)->last_durable_index() == snapshot_index, "Keeper snapshot outran persisted log");
            for (uint64_t index = 1; index <= snapshot_index; ++index)
            {
                auto entry = disk_logs.at(3)->entry_at(index);
                require(entry != nullptr, "Keeper recovery comparison is missing original log index " + std::to_string(index));
                auto serialized = entry->serialize();
                saved_entries.emplace_back(reinterpret_cast<const char *>(serialized->data_begin()), serialized->size());
            }
            if (metadata_write_failure_scenario)
            {
                driver.enqueue("keeper-metadata-write-failure", 3, 0, [&]
                {
                    auto disk = std::static_pointer_cast<InterruptedWriteDisk>(contexts.at(3)->getStateFileDisk());
                    auto attempted_state = srv_state::deserialize(*saved_state);
                    attempted_state->set_term(attempted_state->get_term() + 7);
                    attempted_state->set_voted_for(2);
                    const auto failures_before = disk->failures;
                    disk->fail_next_state_write = true;
                    bool interrupted = false;
                    try
                    {
                        managers.at(3)->save_state(*attempted_state);
                    }
                    catch (const std::runtime_error & error)
                    {
                        const auto expected_error = metadata_fault == "partial" ? "simulated partial write failure" : "simulated interrupted state rewrite";
                        require(std::string(error.what()) == expected_error, "Unexpected metadata write failure");
                        interrupted = true;
                    }
                    const size_t expected_bytes = metadata_fault == "partial" ? 13 : 0;
                    require(interrupted && disk->failures == failures_before + 1
                        && disk->getFileSize("state") == expected_bytes && disk->existsFile("state-OLD"),
                        "Keeper metadata fault did not leave a truncated live file and backup");
                    driver.record("keeper-metadata-disk-fault " + metadata_fault + " " + std::to_string(disk->failed_file_bytes));
                    driver.record("keeper-metadata-rewrite-interrupted 3");
                    if (unsafe_metadata_backup_loss)
                    {
                        disk->removeFile("state-OLD");
                        driver.record("keeper-metadata-backup-removed 3");
                    }
                });
                driver.drain();
            }
        }
        driver.nodes.at(3)->shutdown();
        driver.nodes.erase(3);
        machines.erase(3);
        require(old_server.expired() && old_machine.expired(), "Keeper restart retained previous runtime");
        require(timers.at(3)->empty(), "Keeper stopped follower retained timers");
        if (real_logs)
        {
            std::weak_ptr<DB::KeeperLogStore> old_log = disk_logs.at(3);
            if (discard_unwritten && !unsafe_log_loss)
            {
                require(disk_logs.at(3)->discardUnexecutedWritesForTesting() >= 2, "Keeper interrupted write had no queued append/flush");
                driver.record("keeper-log-unwritten-tail-discarded 3 " + std::to_string(snapshot_index + 1));
            }
            std::erase_if(queued_log_tasks, [](const auto & item)
            {
                return item.first == 3;
            });
            std::weak_ptr<state_mgr> old_manager = managers.at(3);
            managers.erase(3);
            disk_logs.erase(3);
            require(old_log.expired() && old_manager.expired(), "Keeper restart retained previous log store or manager");
            driver.record("keeper-log-destroyed 3");
            if (unsafe_metadata_loss)
            {
                require(std::filesystem::remove(directory / "3/metadata/state"), "Keeper metadata loss control found no state");
                driver.record("keeper-metadata-state-removed 3");
            }
        }
        contexts[3] = make_context(3);
        require(old_context.expired() && contexts.at(3)->lastCommittedIndex() == 0, "Keeper restart retained previous context state");
        machines[3] = make_machine(3);
        require(machines.at(3)->last_commit_index() == snapshot_index, "Keeper snapshot did not restore commit index");
        check(3, "/experiment", "updated--47", true);
        check(3, "/ephemeral", "ephemeral-902", true);
        if (real_logs)
        {
            make_persistent_manager(3, snapshot_index);
            auto state = managers.at(3)->read_state();
            require(state != nullptr, "Keeper restart lost term/vote state");
            auto restored_state = state->serialize();
            auto restored_config = managers.at(3)->load_config()->serialize();
            require(restored_state->size() == saved_state->size()
                && std::equal(saved_state->data_begin(), saved_state->data_begin() + saved_state->size(), restored_state->data_begin()),
                "Keeper restart changed persisted term/vote state");
            require(restored_config->size() == saved_config->size()
                && std::equal(saved_config->data_begin(), saved_config->data_begin() + saved_config->size(), restored_config->data_begin()),
                "Keeper restart changed persisted configuration");
            require(!contexts.at(3)->getStateFileDisk()->existsFile("state-OLD"), "Keeper metadata backup was not promoted");
            driver.record("keeper-metadata-recovered-exact 3 " + std::to_string(state->get_term())
                + " " + std::to_string(state->get_voted_for()));
            require(disk_logs.at(3)->next_slot() == snapshot_index + 1, "Keeper disk recovery changed log tail");
            for (uint64_t index = 1; index <= snapshot_index; ++index)
            {
                auto entry = disk_logs.at(3)->entry_at(index);
                require(entry != nullptr, "Keeper recovery comparison is missing restored log index " + std::to_string(index));
                auto serialized = entry->serialize();
                require(std::string(reinterpret_cast<const char *>(serialized->data_begin()), serialized->size()) == saved_entries.at(index - 1),
                    "Keeper disk recovery changed a persisted entry");
            }
            driver.record("keeper-log-recovered-exact 3 " + std::to_string(snapshot_index));
        }
        ++driver.incarnations.at(3);
        timers[3] = cs_new<ManualTimers>(3, driver);
        driver.nodes[3] = cs_new<ManualServer>(new context(managers.at(3), machines.at(3), {}, nullptr,
            factory, timers.at(3), params, &driver));
        driver.nodes.at(3)->start_server(false);
        if (real_logs)
        {
            disk_logs.at(3)->setRaftServer(driver.nodes.at(3));
            driver.enqueue("keeper-vote-after-restart", 3, 0, [&]
            {
                auto state = managers.at(3)->read_state();
                require(state != nullptr, "Keeper restarted voter lost state");
                const auto term = state->get_term();
                const int candidate = state->get_voted_for() == -1 ? 2 : state->get_voted_for();
                const auto last_index = disk_logs.at(3)->next_slot() - 1;
                const auto last_term = disk_logs.at(3)->term_at(last_index);
                req_msg retry(term, msg_type::request_vote_request, candidate, 3, last_term, last_index, snapshot_index);
                auto response = driver.process_req(driver.nodes.at(3).get(), retry);
                require(response && response->get_accepted(), "Keeper restarted voter rejected an eligible vote/retry");
                const int competitor = candidate == 1 ? 2 : 1;
                req_msg conflict(term, msg_type::request_vote_request, competitor, 3, last_term, last_index, snapshot_index);
                response = driver.process_req(driver.nodes.at(3).get(), conflict);
                require(response && !response->get_accepted(), "Keeper restarted voter granted conflicting same-term votes");
                req_msg stale(term - 1, msg_type::request_vote_request, competitor, 3, last_term, last_index, snapshot_index);
                response = driver.process_req(driver.nodes.at(3).get(), stale);
                require(response && !response->get_accepted(), "Keeper restarted voter accepted a stale-term vote");
                state = managers.at(3)->read_state();
                require(state && state->get_term() == term && state->get_voted_for() == candidate,
                    "Keeper restarted vote decision was not persisted");
                driver.record("keeper-vote-retry-and-conflict-checked 3 " + std::to_string(term) + " " + std::to_string(candidate));
            });
            driver.drain(discard_unwritten ? (leader == 1 ? 2 : 1) : 0);
        }
        driver.record("keeper-snapshot-follower-restored " + std::to_string(snapshot_index));
        observe_workload("restored-" + std::to_string(++workload_restarts));
    };
    restart_follower(false);

    std::map<int, int64_t> session_deadlines;
    std::set<int64_t> expiry_checks;
    for (const auto & [id, machine] : machines)
    {
        /// Derive deadlines from observed commit/restoration times, never the expiry queue's output.
        const auto deadline = ((session_activity.at(id) + 30000) / 500 + 1) * 500;
        require(logical_ms < deadline - 1, "Keeper workload outlived its session");
        session_deadlines[id] = deadline;
        expiry_checks.insert(deadline - 1);
        expiry_checks.insert(deadline);
    }
    for (const auto time : expiry_checks)
    {
        logical_ms = time;
        for (const auto & [id, machine] : machines)
            require(machine->getDeadSessions() == (time >= session_deadlines.at(id) ? std::vector<int64_t>{1} : std::vector<int64_t>{}),
                "Keeper injected expiry boundary differs");
    }
    auto close = std::make_shared<Coordination::ZooKeeperCloseRequest>();
    close->xid = Coordination::CLOSE_XID;
    if (recovery_scenario)
    {
        pause_durability(leader);
        pause_durability(3);
    }
    auto close_result = append(close, 1);
    driver.drain(recovery_scenario ? (leader == 1 ? 2 : 1) : 3);
    if (recovery_scenario)
    {
        require(!close_result->has_result(), "Keeper interrupted close committed without durable majority");
        require(disk_logs.at(3)->next_slot() == snapshot_index + 2
            && disk_logs.at(3)->last_durable_index() == snapshot_index, "Keeper interrupted close was not an unwritten log tail");
        require(driver.pending_responses.size() == 1, "Keeper interrupted close had no deferred response");
        require(machines.at(3)->last_commit_index() == snapshot_index, "Keeper applied an uncommitted close");
        restart_follower(true);
        require(driver.pending_responses.empty(), "Keeper stopped follower retained deferred responses");
        complete_durability(3);
        complete_durability(leader);
        driver.drain(3);
    }
    succeeded(close_result);
    check(1, "/ephemeral", "", false);
    check(2, "/ephemeral", "", false);
    check(3, "/ephemeral", "ephemeral-902", true);
    if (recovery_scenario)
    {
        for (int attempt = 0; attempt < 8 && machines.at(3)->last_commit_index() != snapshot_index + 1; ++attempt)
        {
            timers.at(leader)->fire(timer_task_type::heartbeat_timer);
            driver.drain();
        }
        require(machines.at(3)->last_commit_index() == snapshot_index + 1, "Keeper retransmission did not restore lost close");
        driver.record("keeper-log-loss-retransmitted-exact");
    }
    driver.drain();
    for (int id = 1; id <= 3; ++id)
    {
        check(id, "/ephemeral", "", false);
        check(id, "/experiment", "updated--47", true);
        require(machines.at(id)->getDeadSessions().empty(), "Keeper closed session remained scheduled");
        require(responses.at(id).size() == 5, "Keeper operation response count differs");
        for (const auto & response : responses.at(id))
            require(response.response->error == Coordination::Error::ZOK, "Keeper operation returned an error");
        require(snapshots.at(id)->empty(), "Keeper unexpectedly queued snapshot work");
    }
    driver.record("keeper-expiry-close-replicated");
    if (generated_workload)
        driver.record("workload-close");
    observe_workload("closed");
    for (const auto & [id, node] : driver.nodes)
        node->shutdown();
    driver.nodes.clear();
    require(driver.events.empty() && driver.pending_responses.empty(), "Keeper retained transport work at shutdown");
    for (const auto & [id, timer] : timers)
        require(timer->empty(), "Keeper retained timer at shutdown");
    machines.clear();
    if (real_logs)
    {
        require(queued_log_tasks.empty(), "Keeper retained scheduled log work");
        for (const auto & [id, store] : disk_logs)
        {
            require(store->last_durable_index() == snapshot_index + 1, "Keeper final log is not durable");
        }
        managers.clear();
        disk_logs.clear();
    }
    require(thread_attempts == 0 && liveThreads() == 1, "Keeper created uncontrolled threads");
    require(virtual_posix_reads > 0, "Keeper POSIX clock instrumentation was ineffective");
    require(host_clock_reads == host_reads_before, "Keeper probe delegated to an uncontrolled host clock");
    driver.finishReplay();
    driver.record("keeper-shutdown-clean");
    std::cout << "MEASURE thread_attempts=" << thread_attempts << " live_threads=" << liveThreads()
        << " virtual_posix_clock_reads=" << virtual_posix_reads
        << " delegated_clock_gettime_calls=" << host_clock_reads
        << " delegated_probe_clock_gettime_calls=" << host_clock_reads - host_reads_before << '\n';
    for (size_t id = 0; id < delegated_posix_reads.size(); ++id)
        if (delegated_posix_reads[id])
            std::cout << "MEASURE delegated_clock_id=" << id << " calls=" << delegated_posix_reads[id] << '\n';
    std::cout << "PASS real-keeper-manual-probe storage=" << (lsmt ? "LSMT" : "Mem")
        << " clock_control=PARTIAL_POSIX persistence=" << (real_logs ? "REAL_LOG_REAL_SNAPSHOT_REAL_METADATA" : "MODELED_LOG_REAL_SNAPSHOT")
        << "; FULL_CLICKHOUSE_DETERMINISTIC_GATE=BLOCKED\n";
}
#endif
}

int main(int argc, char ** argv)
{
#if SIMULATION_KEEPER
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
        std::filesystem::path directory;
        bool lsmt = false;
        bool real_logs = false;
        bool instrumentation_control = false;
        bool unsafe_quorum = false;
        bool unsafe_durability = false;
        bool unsafe_log_loss = false;
        bool unsafe_metadata_loss = false;
        int delay_ms = 0;
        for (int arg = 1; arg < argc; ++arg)
        {
            const std::string option = argv[arg];
            if (option == "--state-dir" && arg + 1 < argc)
                directory = argv[++arg];
            else if (option == "--lsmt")
                lsmt = true;
            else if (option == "--real-log")
                real_logs = true;
            else if (option == "--log-loss")
                recovery_scenario = true;
            else if (option == "--unsafe-log-loss-control")
            {
                recovery_scenario = true;
                unsafe_log_loss = true;
            }
            else if (option == "--unsafe-metadata-loss-control")
                unsafe_metadata_loss = true;
            else if (option == "--metadata-write-failure")
                metadata_write_failure_scenario = true;
            else if (option == "--metadata-fault" && arg + 1 < argc)
            {
                metadata_fault = argv[++arg];
                require(metadata_fault == "open" || metadata_fault == "partial", "Unsupported metadata fault");
                metadata_write_failure_scenario = true;
            }
            else if (option == "--unsafe-metadata-backup-control")
            {
                metadata_write_failure_scenario = true;
                unsafe_metadata_backup_loss = true;
            }
            else if (option == "--snapshot-write-failure")
                snapshot_write_failure_scenario = true;
            else if (option == "--generated-workload")
                generated_workload = true;
            else if (option == "--multi-workload")
            {
                generated_workload = true;
                multi_workload = true;
            }
            else if (option == "--pipelined-workload")
            {
                generated_workload = true;
                multi_workload = true;
                pipelined_workload = true;
            }
            else if (option == "--pipeline-fault" && arg + 1 < argc)
            {
                pipeline_fault = argv[++arg];
                require(pipeline_fault == "reply-loss" || pipeline_fault == "restart" || pipeline_fault == "leader-failover"
                    || pipeline_fault == "leader-partition" || pipeline_fault == "leader-prefix" || pipeline_fault == "leader-prefix-partition",
                    "Unsupported pipeline fault");
                generated_workload = true;
                multi_workload = true;
                pipelined_workload = true;
            }
            else if (option == "--pipeline-fault-batch" && arg + 1 < argc)
            {
                pipeline_fault_batch = std::stoul(argv[++arg]);
                require(pipeline_fault_batch < 8, "Pipeline fault batch must be between 0 and 7");
            }
            else if (option == "--snapshot-fault" && arg + 1 < argc)
            {
                snapshot_fault = argv[++arg];
                require(snapshot_fault == "open" || snapshot_fault == "partial" || snapshot_fault == "sync" || snapshot_fault == "marker",
                    "Unsupported snapshot fault");
                snapshot_write_failure_scenario = true;
            }
            else if (option == "--unsafe-snapshot-write-control")
            {
                snapshot_write_failure_scenario = true;
                unsafe_snapshot_write_control = true;
            }
            else if (option == "--virtual-time")
                virtual_time = true;
            else if (option == "--instrumentation-control")
                instrumentation_control = true;
            else if (option == "--seed" && arg + 1 < argc)
            {
                const auto seed = std::stoul(argv[++arg]);
                require(seed <= std::numeric_limits<uint32_t>::max(), "Seed exceeds uint32 range");
                random_seed = static_cast<uint32_t>(seed);
            }
            else if (option == "--host-delay")
                delay_ms = 1;
            else if (option == "--unsafe-quorum-control")
                unsafe_quorum = true;
            else if (option == "--unsafe-durability-control")
                unsafe_durability = true;
            else if (option == "--record-schedule" && arg + 1 < argc)
                record_schedule_path = argv[++arg];
            else if (option == "--replay-schedule" && arg + 1 < argc)
                replay_schedule_path = argv[++arg];
            else
                throw std::runtime_error("Unknown arguments");
        }
        virtual_time = true;
        if (instrumentation_control)
        {
            const auto before = std::chrono::steady_clock::now();
            const auto system_before = std::chrono::system_clock::now();
            Stopwatch stopwatch;
            logical_ms += 17;
            require(std::chrono::steady_clock::now() - before == std::chrono::milliseconds(17)
                && std::chrono::system_clock::now() - system_before == std::chrono::milliseconds(17),
                "Keeper virtual POSIX clocks did not control chrono");
            require(stopwatch.elapsedMilliseconds() == 17, "Keeper virtual POSIX clock did not control Stopwatch");
            const auto host_reads_before = host_clock_reads.load();
            timespec cpu_time{};
            require(clock_gettime(CLOCK_THREAD_CPUTIME_ID, &cpu_time) == 0, "Keeper host-clock control failed");
            require(host_clock_reads == host_reads_before + 1, "Keeper instrumentation missed host-clock delegation");
            std::thread control([]
            {
            });
            control.join();
            require(thread_attempts == 1 && virtual_posix_reads >= 4, "Keeper instrumentation control failed");
            std::cout << "PASS keeper-instrumentation-control thread_attempts=" << thread_attempts << '\n';
            return 0;
        }
        require(!directory.empty(), "Keeper probe requires --state-dir");
        require(!recovery_scenario || real_logs, "Keeper log-loss scenario requires real logs");
        require(!unsafe_metadata_loss || real_logs, "Keeper metadata-loss control requires real logs");
        require(!metadata_write_failure_scenario || real_logs, "Keeper metadata-write failure requires real logs");
        require(!snapshot_write_failure_scenario || real_logs, "Keeper snapshot-write failure requires real logs");
        require(pipeline_fault.empty() || real_logs, "Keeper pipeline faults require real logs");
        require(pipeline_fault_batch == 0 || pipeline_fault == "leader-partition" || pipeline_fault == "leader-failover"
            || pipeline_fault == "leader-prefix" || pipeline_fault == "leader-prefix-partition",
            "A nonzero pipeline fault batch requires a leader fault");
        schedule_version = real_logs ? "keeper-real-log-schedule-v4" : (lsmt ? "keeper-lsmt-schedule-v2" : "keeper-mem-schedule-v2");
        if (metadata_write_failure_scenario)
            schedule_version += "-metadata-write-failure-" + metadata_fault;
        if (snapshot_write_failure_scenario)
            schedule_version += "-snapshot-write-failure-" + snapshot_fault;
        if (generated_workload)
            schedule_version += "-generated-v1";
        if (multi_workload)
            schedule_version += "-multi-v1";
        if (pipelined_workload)
            schedule_version += "-pipeline-v1";
        if (!pipeline_fault.empty())
            schedule_version += "-fault-" + pipeline_fault + "-v2";
        if (pipeline_fault_batch)
            schedule_version += "-batch-" + std::to_string(pipeline_fault_batch);
        runKeeperProbe(directory, lsmt, real_logs, delay_ms, unsafe_quorum, unsafe_durability, unsafe_log_loss, unsafe_metadata_loss);
        return 0;
    }
    catch (const std::exception & error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
#else
    try
    {
        if (argc == 2 && std::string(argv[1]) == "--instrumentation-control")
        {
            std::thread control([]
            {
            });
            control.join();
            static_cast<void>(std::chrono::steady_clock::now());
            static_cast<void>(std::chrono::system_clock::now());
            require(thread_attempts == 1 && steady_reads > 0 && system_reads > 0, "Instrumentation negative control failed");
            const auto host_reads_before = host_clock_reads.load();
            virtual_time = true;
            const auto before = std::chrono::steady_clock::now();
            logical_ms += 17;
            require(std::chrono::steady_clock::now() - before == std::chrono::milliseconds(17),
                "Virtual steady clock did not follow logical time");
            const auto system_before = std::chrono::system_clock::now();
            logical_ms += 29;
            require(std::chrono::system_clock::now() - system_before == std::chrono::milliseconds(29),
                "Virtual system clock did not follow logical time");
            require(host_clock_reads == host_reads_before, "Virtual control delegated to host clock");
            std::cout << "PASS instrumentation-control thread_attempts=" << thread_attempts << '\n';
        }
        else
        {
            int delay_ms = 0;
            bool unsafe_quorum = false;
            bool unsafe_durability = false;
            bool keeper_expiry = false;
            for (int arg = 1; arg < argc; ++arg)
            {
                const std::string option = argv[arg];
                if (option == "--virtual-time")
                    virtual_time = true;
                else if (option == "--host-delay")
                    delay_ms = 1;
                else if (option == "--unsafe-quorum-control")
                    unsafe_quorum = true;
                else if (option == "--unsafe-durability-control")
                {
                    unsafe_durability = true;
                    recovery_scenario = true;
                }
                else if (option == "--keeper-expiry")
                    keeper_expiry = true;
                else if (option == "--recovery")
                    recovery_scenario = true;
                else if (option == "--lost-reply")
                    lost_reply_scenario = true;
                else if (option == "--record-schedule" && arg + 1 < argc)
                    record_schedule_path = argv[++arg];
                else if (option == "--replay-schedule" && arg + 1 < argc)
                    replay_schedule_path = argv[++arg];
                else if (option == "--seed" && arg + 1 < argc)
                    random_seed = std::stoul(argv[++arg]);
                else
                    throw std::runtime_error("Unknown arguments");
            }
            if (keeper_expiry)
                runKeeperExpiry(delay_ms);
            else
                runProbe(delay_ms, unsafe_quorum, unsafe_durability);
        }
        return 0;
    }
    catch (const std::exception & error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
#endif
}
