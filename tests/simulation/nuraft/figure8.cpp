#define main existingProbeMain
#include "probe.cpp"
#undef main

namespace
{
std::set<int> connected{1, 2, 3, 4, 5};
bool elections_only = false;
int selected_candidate = 1;
uint64_t blocked_endpoint = 0;
uint64_t next_rpc_id = 0;
std::map<int, uint64_t> acknowledged;
std::string phase = "initial";

std::string payload(const ptr<log_entry> & entry)
{
    static constexpr char digits[] = "0123456789abcdef";
    const auto & data = entry->get_buf();
    std::string result;
    for (size_t offset = 0; offset < data.size(); ++offset)
    {
        const auto byte = data.data_begin()[offset];
        result += digits[byte >> 4];
        result += digits[byte & 15];
    }
    return result;
}

class FigureClient : public rpc_client
{
public:
    FigureClient(Driver & driver_, uint64_t id_) : driver(driver_), id(id_)
    {
    }
    uint64_t get_id() const override
    {
        return id;
    }
    bool is_abandoned() const override
    {
        return false;
    }
    void send(ptr<req_msg> & request, rpc_handler & callback, uint64_t) override
    {
        const auto rpc_id = ++next_rpc_id;
        driver.enqueue("figure-request", request->get_src(), request->get_dst(), [this, request, callback, rpc_id]() mutable
        {
            const auto endpoint = request->get_last_log_idx() + request->log_entries().size();
            const bool append = request->get_type() == msg_type::append_entries_request;
            // Keep C's pre-vote connection to A alive for the higher-term vote request.
            const bool term_notice = elections_only && selected_candidate == 3 && request->get_src() == 3
                && request->get_dst() == 1 && (request->get_type() == msg_type::request_vote_request
                    || request->get_type() == msg_type::pre_vote_request);
            const bool available = term_notice || (connected.count(request->get_src()) && connected.count(request->get_dst())
                && (!elections_only || (!append && request->get_src() == selected_candidate))
                // Missing-prefix probes must reach the follower to get a real rejection.
                // They cannot store the withheld entry, since their predecessor is absent.
                && (!append || !blocked_endpoint || endpoint < blocked_endpoint
                    || request->get_last_log_idx() > driver.nodes.at(request->get_dst())->get_last_log_idx()));
            driver.record("rpc-request " + std::to_string(request->get_src()) + " " + std::to_string(request->get_dst())
                + " type=" + std::to_string(request->get_type()) + " term=" + std::to_string(request->get_term())
                + " prev=" + std::to_string(request->get_last_log_idx()) + " end=" + std::to_string(endpoint)
                + " commit=" + std::to_string(request->get_commit_idx()) + " deliver=" + std::to_string(available));
            for (size_t offset = 0; offset < request->log_entries().size(); ++offset)
                driver.record("rpc-entry " + std::to_string(request->get_last_log_idx() + offset + 1)
                    + " term=" + std::to_string(request->log_entries()[offset]->get_term()));
            ptr<resp_msg> response;
            ptr<rpc_exception> error;
            if (available)
            {
                response = driver.process_req(driver.nodes.at(request->get_dst()).get(), *request);
                require(response && !response->has_async_cb() && !response->has_cb(), "Unexpected asynchronous response");
            }
            else
                error = cs_new<rpc_exception>("Figure 8 selected network drop", request);
            driver.enqueue("figure-response", request->get_dst(), request->get_src(),
                [this, callback, response, error, request, endpoint, rpc_id]() mutable
            {
                if (response)
                {
                    driver.record("rpc-response " + std::to_string(response->get_src()) + " " + std::to_string(response->get_dst())
                        + " term=" + std::to_string(response->get_term()) + " accepted=" + std::to_string(response->get_accepted())
                        + " next=" + std::to_string(response->get_next_idx()));
                    std::cout << "RAFT {\"kind\":\"response\",\"event\":" << driver.executed
                        << ",\"rpc\":" << rpc_id << ",\"type\":" << int(request->get_type())
                        << ",\"from\":" << request->get_dst() << ",\"to\":" << request->get_src()
                        << ",\"request_term\":" << request->get_term() << ",\"term\":" << response->get_term()
                        << ",\"accepted\":" << int(response->get_accepted()) << ",\"end\":" << endpoint
                        << ",\"next\":" << response->get_next_idx() << "}\n";
                    if (request->get_type() == msg_type::append_entries_request && response->get_accepted())
                    {
                        require(response->get_next_idx() == endpoint + 1, "SETUP response endpoint differs");
                        if (phase == "old-term-quorum" && request->get_src() == 1)
                            acknowledged[request->get_dst()] = endpoint;
                    }
                }
                callback(response, error);
            });
        });
    }
private:
    Driver & driver;
    uint64_t id;
};

class FigureFactory : public rpc_client_factory
{
public:
    explicit FigureFactory(Driver & driver_) : driver(driver_)
    {
    }
    ptr<rpc_client> create_client(const std::string &) override
    {
        clients.push_back(cs_new<FigureClient>(driver, ++next_id));
        return clients.back();
    }
private:
    Driver & driver;
    uint64_t next_id = 0;
    std::vector<ptr<rpc_client>> clients;
};
}

int main(int argc, char ** argv)
{
    try
    {
        bool current_term_control = false;
        for (int arg = 1; arg < argc; ++arg)
        {
            const std::string option = argv[arg];
            if (option == "--current-term-control")
                current_term_control = true;
            else if (option == "--record" && arg + 1 < argc)
                record_schedule_path = argv[++arg];
            else if (option == "--replay" && arg + 1 < argc)
                replay_schedule_path = argv[++arg];
            else
                throw std::runtime_error("SETUP unknown or incomplete option: " + option);
        }
        virtual_time = true;
        Driver driver;
        std::map<int, ptr<MemoryManager>> managers;
        std::map<int, ptr<Machine>> machines;
        std::map<int, ptr<ManualTimers>> timers;
        auto factory = cs_new<FigureFactory>(driver);
        cluster_config config;
        for (int id = 1; id <= 5; ++id)
            config.get_servers().push_back(cs_new<srv_config>(id, std::to_string(id)));
        raft_params params;
        params.election_timeout_lower_bound_ = 150;
        params.election_timeout_upper_bound_ = 300;
        params.heart_beat_interval_ = 50;
        params.rpc_failure_backoff_ = 0;
        params.return_method_ = raft_params::async_handler;
        params.use_bg_thread_for_urgent_commit_ = true;
        params.use_bg_thread_for_snapshot_io_ = false;
        params.snapshot_distance_ = 0;
        params.max_append_size_ = 1;
        for (int id = 1; id <= 5; ++id)
        {
            managers[id] = cs_new<MemoryManager>(id);
            managers[id]->save_config(config);
            machines[id] = cs_new<Machine>(id, driver, std::make_shared<MachineImage>());
            timers[id] = cs_new<ManualTimers>(id, driver);
            driver.incarnations[id] = 1;
            driver.nodes[id] = cs_new<ManualServer>(new context(managers[id], machines[id], {}, nullptr,
                factory, timers[id], params, &driver));
        }
        for (const auto & [id, node] : driver.nodes)
            node->start_server(false);
        uint64_t observation = 0;
        auto observe = [&]
        {
            std::cout << "RAFT {\"kind\":\"state\",\"version\":1,\"observation\":" << ++observation
                << ",\"event\":" << driver.executed << ",\"phase\":\"" << phase << "\",\"nodes\":[";
            bool first_node = true;
            for (const auto & [id, node] : driver.nodes)
            {
                auto log = managers.at(id)->log;
                auto saved = managers.at(id)->read_state();
                require(log->start_index() == 1, "SETUP compacted log outside trace scope");
                if (!first_node)
                    std::cout << ',';
                first_node = false;
                std::cout << "{\"id\":" << id << ",\"incarnation\":" << driver.incarnations.at(id)
                    << ",\"term\":" << node->get_term() << ",\"vote\":" << (saved ? saved->get_voted_for() : -1)
                    << ",\"leader\":" << int(node->is_leader())
                    << ",\"commit\":" << node->get_target_committed_log_idx()
                    << ",\"applied\":" << node->get_committed_log_idx()
                    << ",\"durable\":" << log->last_durable_index() << ",\"log\":[";
                for (uint64_t index = 1; index < log->next_slot(); ++index)
                {
                    const auto entry = log->entry_at(index);
                    if (index != 1)
                        std::cout << ',';
                    std::cout << '[' << entry->get_term() << ',' << int(entry->get_val_type())
                        << ",\"" << payload(entry) << "\"]";
                }
                std::cout << "],\"values\":[";
                const auto & values = machines.at(id)->values;
                for (size_t i = 0; i < values.size(); ++i)
                    std::cout << (i ? "," : "") << values[i];
                std::cout << "]}";
            }
            std::cout << "]}\n";
        };
        auto drain = [&]
        {
            while (driver.step())
                observe();
        };
        auto fire = [&](int id, int type)
        {
            timers.at(id)->fire(type);
            observe();
        };
        observe();
        fire(1, timer_task_type::election_timer);
        drain();
        require(driver.nodes.at(1)->is_leader(), "SETUP initial leader missing");
        const auto base = driver.nodes.at(1)->get_committed_log_idx();
        require(base == 1, "SETUP initial configuration uncommitted");
        for (const auto & [id, node] : driver.nodes)
            require(node->get_committed_log_idx() == base && managers.at(id)->log->next_slot() == base + 1,
                "SETUP common prefix missing");
        phase = "minority";
        connected = {1, 2};
        auto value = buffer::alloc(sizeof(int32_t));
        value->put(int32_t{731});
        value->pos(0);
        auto result = driver.nodes.at(1)->append_entries({value});
        require(result->get_accepted(), "SETUP append not accepted");
        observe();
        drain();
        const auto index = base + 1;
        require(managers.at(1)->log->next_slot() == index + 1 && managers.at(2)->log->next_slot() == index + 1,
            "SETUP old-term entry missing on minority");
        require(driver.nodes.at(1)->get_target_committed_log_idx() == base, "CONSENSUS minority committed entry");
        const auto old_term = managers.at(1)->log->entry_at(index)->get_term();
        const auto old_payload = payload(managers.at(1)->log->entry_at(index));
        if (current_term_control)
        {
            phase = "current-term-control";
            connected = {1, 2, 3, 4, 5};
            for (int attempt = 0; attempt < 16; ++attempt)
            {
                fire(1, timer_task_type::heartbeat_timer);
                drain();
            }
            for (const auto & [id, node] : driver.nodes)
                require(node->get_committed_log_idx() == index && machines.at(id)->values == std::vector<int32_t>{731},
                    "SETUP current-term positive control did not apply");
            driver.finishReplay();
            std::cout << "PASS current-term quorum committed and applied on all five nodes\n";
            return 0;
        }
        auto elect = [&](int leader, std::set<int> voters)
        {
            require(!driver.nodes.at(leader)->is_leader(), "SETUP candidate still leader in old term");
            connected = std::move(voters);
            selected_candidate = leader;
            elections_only = true;
            for (int id : connected)
                if (id != leader)
                    fire(id, timer_task_type::election_timer);
            drain();
            for (int attempt = 0; attempt < 8 && !driver.nodes.at(leader)->is_leader(); ++attempt)
            {
                fire(leader, timer_task_type::election_timer);
                drain();
            }
            require(driver.nodes.at(leader)->is_leader(), "SETUP requested replacement election failed");
            driver.record("selected-leader " + std::to_string(leader) + " term=" + std::to_string(driver.nodes.at(leader)->get_term()));
        };
        phase = "conflicting-election";
        elect(3, {3, 4, 5});
        require(managers.at(3)->log->is_conf(index), "SETUP conflicting new-term configuration missing");
        require(managers.at(4)->log->next_slot() == index && managers.at(5)->log->next_slot() == index,
            "SETUP conflicting configuration replicated unexpectedly");
        const auto middle_term = driver.nodes.at(3)->get_term();
        phase = "returning-election";
        elect(1, {1, 2, 4});
        require(old_term < middle_term && driver.nodes.at(1)->get_term() > middle_term,
            "SETUP original leader did not enter newer term");
        require(managers.at(1)->log->is_conf(index + 1)
            && managers.at(1)->log->entry_at(index + 1)->get_term() == driver.nodes.at(1)->get_term(),
            "SETUP new leader barrier missing");
        require(driver.nodes.at(1)->get_target_committed_log_idx() == base,
            "CONSENSUS entry committed during election-only stage");
        phase = "old-term-quorum";
        elections_only = false;
        connected = {1, 4, 5};
        blocked_endpoint = index + 1;
        for (int attempt = 0; attempt < 16; ++attempt)
        {
            fire(1, timer_task_type::heartbeat_timer);
            drain();
        }
        driver.record("figure8-observation old_term=" + std::to_string(old_term)
            + " current_term=" + std::to_string(driver.nodes.at(1)->get_term())
            + " old_index=" + std::to_string(index)
            + " target_commit=" + std::to_string(driver.nodes.at(1)->get_target_committed_log_idx()));
        for (int id : {4, 5})
            require(managers.at(id)->log->next_slot() == index + 1
                && managers.at(id)->log->entry_at(index)->get_term() == old_term
                && payload(managers.at(id)->log->entry_at(index)) == old_payload
                && managers.at(id)->log->last_durable_index() == index && acknowledged[id] == index,
                "SETUP catch-up did not acknowledge exactly the old-term entry");
        for (int id : {2, 3, 4, 5})
            require(managers.at(id)->log->next_slot() == index + 1,
                "SETUP current-term barrier reached a follower");
        require(thread_attempts == 0 && host_clock_reads == 0, "SETUP uncontrolled execution");
        driver.record("figure8-setup-complete");
        driver.finishReplay();
        require(driver.nodes.at(1)->get_target_committed_log_idx() < index,
            "CONSENSUS FIGURE8 old-term entry committed without current-term barrier quorum");
        std::cout << "PASS Figure8 old-term-only quorum did not commit\n";
        return 0;
    }
    catch (const std::exception & error)
    {
        std::cerr << "FAIL " << error.what() << '\n';
        return 1;
    }
}
