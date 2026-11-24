#include <SimClock.h>
#include <SimCluster.h>
#include <SimEngine.h>
#include <SimSwarm.h>
#include <SimWorkloads.h>

#include <Common/ZooKeeper/ZooKeeperCommon.h>
#include <Common/ZooKeeper/ZooKeeperIO.h>
#include <IO/ReadBufferFromString.h>
#include <IO/WriteBufferFromString.h>
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

void runNestedMultiStackOverflowRepro(size_t depth)
{
    DB::WriteBufferFromOwnString wb;
    for (size_t i = 0; i < depth; ++i)
    {
        Coordination::write(static_cast<int32_t>(Coordination::OpNum::Multi), wb);
        Coordination::write(false, wb);
        Coordination::write(static_cast<int32_t>(-1), wb);
    }
    Coordination::write(static_cast<int32_t>(-1), wb);
    Coordination::write(true, wb);
    Coordination::write(static_cast<int32_t>(-1), wb);
    const std::string payload = wb.str();
    std::cerr << "Testing ZooKeeperMultiRequest::readImpl with depth=" << depth
              << " payload_bytes=" << payload.size() << "\n";
    DB::ReadBufferFromString rb(payload);
    Coordination::ZooKeeperMultiRequest req;
    req.readImpl(rb);
    std::cerr << "Returned without crashing (requests=" << req.requests.size() << ")\n";
}

void runNuRaftLiveRepro(const std::string & mode, const std::filesystem::path & state_dir, uint64_t seed, int nuraft_log_level)
{
    using namespace keeper_sim;
    require(!state_dir.empty(), "--state-dir is required for --repro-*");
    if (std::filesystem::exists(state_dir))
        std::filesystem::remove_all(state_dir);

    g_sim_seed = seed;
    g_sim_time_ns = 1'000'000'000LL;
    g_virtual_time_enabled = true;
    seedProcessRng(seed);

    SimEngine engine(seed, 0, "");
    engine.buggify.initialize(engine.rng, /*enable=*/false, "balanced");
    engine.buggify.snapshot_distance = 100000;
    engine.buggify.max_log_gap_in_stream = 0;
    engine.buggify.parallel_log_appending = false;
    if (mode == "figure8")
        engine.buggify.max_append_size = 1;
    else
        engine.buggify.max_append_size = 100;

    ClusterConfig cfg;
    cfg.num_nodes = (mode == "stale-matched-idx" || mode == "figure8") ? 5 : 3;
    cfg.strict_raft_commit = false;
    cfg.nuraft_log_level = nuraft_log_level;
    cfg.root_dir = state_dir;

    SimCluster cluster(engine, cfg);
    cluster.startAll();

    auto & net = cluster.getNetwork();
    auto & nodes = cluster.getNodes();

    auto make_create = [](const std::string & path, const std::string & data, int64_t xid)
    {
        auto req = std::make_shared<Coordination::ZooKeeperCreateRequest>();
        req->xid = xid;
        req->path = path;
        req->data = data;
        return req;
    };

    auto cut_pair = [&](int a, int b)
    {
        net.link(a, b).cut = true;
        net.link(b, a).cut = true;
    };

    auto heal_pair = [&](int a, int b)
    {
        net.link(a, b).cut = false;
        net.link(b, a).cut = false;
        net.link(a, b).clog_extra_ms = 0;
        net.link(b, a).clog_extra_ms = 0;
    };

    /// Wait for initial leader.
    require(engine.runUntilCondition(15000, [&]() { return cluster.findLeader() != 0; }), "No initial leader");
    const int l1 = cluster.findLeader();
    const uint64_t t1 = nodes.at(l1).server->get_term();

    /// Commit baseline entry and replicate to all nodes.
    auto base_res = cluster.submitRequest(l1, make_create("/base", "ok", 1), 1);
    require(base_res && base_res->get_accepted(), "Baseline create rejected");
    require(engine.runUntilCondition(5000, [&]()
    {
        for (int id = 1; id <= cfg.num_nodes; ++id)
        {
            if (nodes.at(id).machine->last_commit_index() < 2)
                return false;
        }
        return true;
    }), "Baseline did not commit on all nodes");

    const uint64_t base_idx = nodes.at(l1).log_store->next_slot() - 1;
    std::cerr << "[" << mode << "] Initial leader=N" << l1 << " term=" << t1 << " base_idx=" << base_idx << "\n";

    if (mode == "stale-matched-idx")
    {
        /// Pick 4 followers: p2 stays with l1; {p3, p4, p5} form a majority partition.
        std::vector<int> followers;
        for (int id = 1; id <= 5; ++id)
        {
            if (id != l1)
                followers.push_back(id);
        }
        const int p2 = followers[0];
        const int p3 = followers[1];
        const int p4 = followers[2];
        const int p5 = followers[3];

        /// Step 1: Partition {l1, p2} from {p3, p4, p5}.
        net.partitionSymmetric({l1, p2}, {p3, p4, p5});

        /// Step 2: Submit uncommitted entry `k` on l1 in term t1; replicate ONLY to p2 (2/5 -> NOT committed).
        const uint64_t k = base_idx + 1;
        auto x_res = cluster.submitRequest(l1, make_create("/committed_then_lost", "secret_v1", 2), 1);
        require(x_res && x_res->get_accepted(), "Write X rejected on l1");

        require(engine.runUntilCondition(5000, [&]()
        {
            return nodes.at(p2).log_store->next_slot() > k
                && nodes.at(l1).server->get_target_committed_log_idx() < k;
        }), "p2 did not receive uncommitted entry k");
        /// Give l1 time to receive p2's AppendEntries response (setting peer[p2].matched_idx = k).
        engine.runUntilMs(simTimeMs() + 150);

        /// Step 3: Wait for {p3, p4, p5} to elect a new leader `l2` in term `t2 > t1`
        /// (which appends its own new-term config at index `k` in term `t2`).
        int l2 = 0;
        require(engine.runUntilCondition(15000, [&]()
        {
            for (int id : {p3, p4, p5})
            {
                if (nodes.at(id).server->is_leader() && nodes.at(id).server->get_term() > t1)
                {
                    l2 = id;
                    return true;
                }
            }
            return false;
        }), "Majority {p3,p4,p5} failed to elect l2");
        const uint64_t t2 = nodes.at(l2).server->get_term();
        std::vector<int> maj_followers;
        for (int id : {p3, p4, p5})
        {
            if (id != l2)
                maj_followers.push_back(id);
        }
        const int f_a = maj_followers[0];
        const int f_b = maj_followers[1];

        /// Immediately isolate l2 from {f_a, f_b} BEFORE l2 replicates k:t2 to them,
        /// and connect l2 <-> p2 so l2 overwrites k:t1 on p2 with k:t2!
        /// Keep l1 <-> p2 CLOGGED (not hard-cut) so l1 does NOT tear down p2's RPC client
        /// and reset peer[p2].matched_idx = 0 (real TCP half-open / slow link!).
        cut_pair(l2, f_a);
        cut_pair(l2, f_b);
        net.link(l1, p2).clog_extra_ms = 20000;
        net.link(p2, l1).clog_extra_ms = 20000;
        heal_pair(l2, p2);

        require(engine.runUntilCondition(5000, [&]()
        {
            return nodes.at(p2).log_store->next_slot() > k
                && nodes.at(p2).log_store->term_at(k) == t2;
        }), "p2 did not truncate k:t1 and accept k:t2 from l2");
        std::cerr << "[" << mode << "] p2=N" << p2 << " TRUNCATED k=" << k
                  << " from term " << t1 << " -> term " << t2
                  << " (f_a last=" << (nodes.at(f_a).log_store->next_slot() - 1)
                  << ", f_b last=" << (nodes.at(f_b).log_store->next_slot() - 1) << ")\n";

        /// Step 4: Connect {l1, f_a, f_b} (keep l2 isolated; keep l1<->p2 clogged, NOT cut!).
        /// l1 has (k, t1), while f_a and f_b only have (base_idx, t1) and term t2.
        /// Note: `l1` wins election in t3 from {l1, f_a, f_b} while STILL retaining stale
        /// `peer[p2].matched_idx == k` from term t1 (even though p2 ALREADY truncated k:t1 -> k:t2)!
        heal_pair(l1, f_a);
        heal_pair(l1, f_b);
        heal_pair(f_a, f_b);
        net.link(l1, f_b).clog_extra_ms = 120;

        require(engine.runUntilCondition(15000, [&]()
        {
            return nodes.at(l1).server->is_leader() && nodes.at(l1).server->get_term() > t2;
        }), "l1 failed to win election in t3 from {l1, f_a, f_b}");
        const uint64_t t3 = nodes.at(l1).server->get_term();

        /// Immediately cut l1 <-> f_b (and keep l1<->p2 clogged!) so ONLY f_a receives l1's AppendEntries([k:t1, k+1:t3])!
        cut_pair(l1, f_b);
        std::cerr << "[" << mode << "] l1=N" << l1 << " re-elected in term=" << t3
                  << "; cut l1<->N" << f_b << " (f_b.last=" << (nodes.at(f_b).log_store->next_slot() - 1)
                  << ") so ONLY N" << f_a << " gets [k:t1, k+1:t3]\n";

        /// Wait for l1 to commit k:t1 using STALE peer[p2].matched_idx == k
        /// (ONLY 2/5 nodes {l1, f_a} actually hold k:t1!!).
        const bool illegally_committed = engine.runUntilCondition(3000, [&]()
        {
            return nodes.at(l1).machine->last_commit_index() >= k;
        });
        std::cerr << "[" << mode << "] AFTER REPLICATION TO ONLY N" << f_a << ": "
                  << "l1.applied=" << nodes.at(l1).machine->last_commit_index()
                  << " l1.target_commit=" << nodes.at(l1).server->get_target_committed_log_idx()
                  << " l1.term_at(k)=" << nodes.at(l1).log_store->term_at(k)
                  << " f_a.last=" << (nodes.at(f_a).log_store->next_slot() - 1)
                  << " f_b.last=" << (nodes.at(f_b).log_store->next_slot() - 1)
                  << " p2.term_at(k)=" << nodes.at(p2).log_store->term_at(k)
                  << " l2.term_at(k)=" << nodes.at(l2).log_store->term_at(k) << "\n";
        require(illegally_committed, "l1 did not commit k:t1 via stale matched_idx");
        require(nodes.at(f_b).log_store->next_slot() - 1 == base_idx, "f_b should NOT have k:t1");

        /// Step 5: Now isolate {l1, f_a} and heal {l2, p2, f_b} (3/5 majority, NONE of which has k:t1)!
        /// l2 or p2 wins election in t4 > t3 and commits k:t2 -> STATE MACHINE SAFETY VIOLATION!
        net.partitionSymmetric({l1, f_a}, {l2, p2, f_b});
        heal_pair(l2, p2);
        heal_pair(l2, f_b);
        heal_pair(p2, f_b);

        require(engine.runUntilCondition(15000, [&]()
        {
            for (int id : {l2, p2, f_b})
            {
                if (nodes.at(id).machine->last_commit_index() >= k)
                    return true;
            }
            return false;
        }), "Majority {l2, p2, f_b} did not commit k:t2");

        std::cerr << "[" << mode << "] REPRODUCED COMMITTED-ENTRY DIVERGENCE! "
                  << "l1 committed k=" << k << " term=" << nodes.at(l1).log_store->term_at(k)
                  << " while l2/p2/f_b committed k=" << k << " term=" << nodes.at(l2).log_store->term_at(k) << "\n";
    }
    else if (mode == "figure8")
    {
        /// Pure Raft §5.4.2 Figure 8 with max_append_size = 1 (no stale matched_idx needed!).
        std::vector<int> followers;
        for (int id = 1; id <= 5; ++id)
        {
            if (id != l1)
                followers.push_back(id);
        }
        const int p2 = followers[0];
        const int p3 = followers[1];
        const int p4 = followers[2];
        const int p5 = followers[3];

        /// Partition {l1, p2} from {p3, p4, p5}.
        net.partitionSymmetric({l1, p2}, {p3, p4, p5});

        /// Append uncommitted k:t1 on l1 and replicate to p2 ONLY.
        const uint64_t k = base_idx + 1;
        auto x_res = cluster.submitRequest(l1, make_create("/figure8_lost", "val_t1", 2), 1);
        require(x_res && x_res->get_accepted(), "Write X rejected");
        require(engine.runUntilCondition(5000, [&]() { return nodes.at(p2).log_store->next_slot() > k; }), "p2 did not get k:t1");

        /// Let {p3, p4, p5} elect l2 in t2 > t1 (appending k:t2), then IMMEDIATELY isolate l2 before it replicates k:t2!
        int l2 = 0;
        require(engine.runUntilCondition(15000, [&]()
        {
            for (int id : {p3, p4, p5})
            {
                if (nodes.at(id).server->is_leader() && nodes.at(id).server->get_term() > t1)
                {
                    l2 = id;
                    return true;
                }
            }
            return false;
        }), "No l2 elected");
        const uint64_t t2 = nodes.at(l2).server->get_term();
        std::vector<int> others;
        for (int id : {p3, p4, p5})
        {
            if (id != l2)
                others.push_back(id);
        }
        const int f_a = others[0];
        const int f_b = others[1];
        /// Isolate l2 completely (so l2 holds [base, k:t2], while f_a and f_b still only have [base]!).
        net.partitionSymmetric({l2}, {l1, p2, f_a, f_b});
        heal_pair(l1, p2);
        heal_pair(l1, f_a);
        heal_pair(l1, f_b);
        heal_pair(p2, f_a);
        heal_pair(p2, f_b);
        heal_pair(f_a, f_b);

        /// Clog l1->p2 so p2 doesn't get k+1:t3 before f_a gets k:t1.
        require(engine.runUntilCondition(15000, [&]()
        {
            return nodes.at(l1).server->is_leader() && nodes.at(l1).server->get_term() > t2;
        }), "l1 did not win election in t3");
        const uint64_t t3 = nodes.at(l1).server->get_term();
        /// Immediately cut l1<->p2 and l1<->f_b so p2 stays at k:t1 and f_a receives ONLY k:t1 first (max_append_size=1)!
        cut_pair(l1, p2);
        cut_pair(l1, f_b);

        /// Wait until f_a has k:t1 (NOT k+1:t3!) and l1 commits k:t1!
        require(engine.runUntilCondition(5000, [&]()
        {
            if (nodes.at(f_a).log_store->next_slot() > k)
                cut_pair(l1, f_a);
            return nodes.at(l1).machine->last_commit_index() >= k;
        }), "l1 did not commit old-term k:t1 before k+1:t3 reached quorum");

        std::cerr << "[" << mode << "] FIGURE-8 VIOLATION: l1 (term=" << t3 << ") committed k=" << k
                  << " (term=" << nodes.at(l1).log_store->term_at(k) << ") while"
                  << " p2.last=" << (nodes.at(p2).log_store->next_slot() - 1)
                  << " f_a.last=" << (nodes.at(f_a).log_store->next_slot() - 1)
                  << " f_b.last=" << (nodes.at(f_b).log_store->next_slot() - 1)
                  << " l2.last=" << (nodes.at(l2).log_store->next_slot() - 1)
                  << " (l2.term_at(k)=" << nodes.at(l2).log_store->term_at(k) << ")\n";

        /// Isolate l1, heal {l2, p2, f_a, f_b}: l2 (term t2 > t1 at idx k) wins election from ALL 4 and overwrites k:t1 with k:t2!
        net.partitionSymmetric({l1}, {l2, p2, f_a, f_b});
        for (int a : {l2, p2, f_a, f_b})
            for (int b : {l2, p2, f_a, f_b})
                if (a < b)
                    heal_pair(a, b);

        require(engine.runUntilCondition(15000, [&]()
        {
            return nodes.at(l2).server->is_leader() && nodes.at(l2).machine->last_commit_index() >= k;
        }), "l2 failed to overwrite committed k:t1 with k:t2");
        std::cerr << "[" << mode << "] l2 OVERWROTE COMMITTED k=" << k << " WITH term=" << t2 << "!\n";
    }
    else if (mode == "stale-ae-resp")
    {
        /// 3-node cluster: delayed stale-term append_entries_response causes FOLLOWER l1 to commit uncommitted entry!
        std::vector<int> followers;
        for (int id = 1; id <= 3; ++id)
            if (id != l1)
                followers.push_back(id);
        const int p2 = followers[0];
        const int p3 = followers[1];

        /// Cut l1 <-> p3, delay responses p2 -> l1 by 2600ms (arrives AFTER l1 steps down to follower at ~2236ms
        /// and BEFORE l1 reconnects peer p2 for prevote at ~3788ms!).
        cut_pair(l1, p3);
        net.link(p2, l1).clog_extra_ms = 2600;

        /// Submit uncommitted write k:t1 on l1 -> sent to p2, p2 appends k and queues reply (delayed ~2000-3500ms)!
        const uint64_t k = base_idx + 1;
        auto res = cluster.submitRequest(l1, make_create("/follower_commit_bug", "stale", 2), 1);
        require(res && res->get_accepted(), "Write rejected");
        require(engine.runUntilCondition(2000, [&]() { return nodes.at(p2).log_store->next_slot() > k; }), "p2 did not append k");

        /// Wait 10ms so p2's async reply event is scheduled on SimEngine (delayed ~1750-3500ms on p2->l1).
        engine.runUntilMs(simTimeMs() + 10);

        /// Reset p2 -> l1 clog to 0 for future packets (the term-1 append_entries_response is ALREADY in flight
        /// with ~1750-3500ms delay!) and clog l1 -> p2 so l1 keeps p2's rpc_ client alive while p2 stops getting heartbeats.
        net.link(p2, l1).clog_extra_ms = 0;
        net.link(l1, p2).clog_extra_ms = 20000;
        heal_pair(p2, p3);

        /// Wait until l1 steps down to FOLLOWER (either via leadership expiry or higher-term RequestVote).
        require(engine.runUntilCondition(4000, [&]()
        {
            return !nodes.at(l1).server->is_leader();
        }), "l1 did not step down to follower");
        /// Immediately clog p2 -> l1 and cut l1 <-> p3 so NO term-2 AppendEntries reaches l1!
        /// The already-scheduled term-1 append_entries_response from p2 -> l1 is STILL in flight!
        net.link(p2, l1).clog_extra_ms = 20000;
        cut_pair(l1, p3);

        const uint64_t t_after = nodes.at(l1).server->get_term();
        const uint64_t commit_before = nodes.at(l1).server->get_target_committed_log_idx();
        std::cerr << "[" << mode << "] l1=N" << l1 << " stepped down to FOLLOWER at term=" << t_after
                  << " (was " << t1 << "), target_commit=" << commit_before << " (k=" << k << "), is_leader="
                  << nodes.at(l1).server->is_leader() << "\n";
        require(commit_before < k, "l1 already committed k before stale response arrived");

        /// Now wait for the delayed stale-term append_entries_response(term=t1, next_idx=k+1) from p2 to arrive at FOLLOWER l1!
        const bool follower_committed = engine.runUntilCondition(5000, [&]()
        {
            return !nodes.at(l1).server->is_leader() && nodes.at(l1).server->get_target_committed_log_idx() >= k;
        });
        std::cerr << "[" << mode << "] AFTER STALE RESP: l1.is_leader=" << nodes.at(l1).server->is_leader()
                  << " term=" << nodes.at(l1).server->get_term()
                  << " target_commit=" << nodes.at(l1).server->get_target_committed_log_idx()
                  << " applied=" << nodes.at(l1).machine->last_commit_index() << "\n";
        require(follower_committed, "Follower l1 did not commit via stale append_entries_response");
    }
    else if (mode == "stale-join-term")
    {
        /// 3-node cluster + dynamic joiner N4:
        /// Delay join_cluster_request(term=t1) from l1 -> N4 while cluster elects l2 in term t2 > t1
        /// and adds N4 at term t2; then l1's stale join_cluster_request(term=t1) arrives at N4
        /// and REGRESSES N4's persisted Raft term backwards from t2 -> t1!
        net.clogLink(l1, 4, 3000, 0.0, 0.0);
        require(cluster.addDynamicNode(l1, /*as_learner=*/false), "addDynamicNode(l1) failed");
        /// Let join_cluster_request(term=t1) get queued in flight from l1 -> 4.
        engine.runUntilMs(simTimeMs() + 20);

        /// Isolate l1 from {others} so remaining 2 nodes elect a new leader l2 in term t2 > t1!
        std::set<int> rest;
        for (int id = 1; id <= 3; ++id)
            if (id != l1)
                rest.insert(id);
        net.partitionSymmetric({l1}, rest);

        int l2 = 0;
        require(engine.runUntilCondition(10000, [&]()
        {
            for (int id : rest)
            {
                if (nodes.at(id).server->is_leader() && nodes.at(id).server->get_term() > t1)
                {
                    l2 = id;
                    return true;
                }
            }
            return false;
        }), "No l2 elected in higher term");
        const uint64_t t2 = nodes.at(l2).server->get_term();

        /// Wait for l2's initial become_leader config entry to commit so add_srv(4) is accepted!
        require(engine.runUntilCondition(2000, [&]()
        {
            return nodes.at(l2).machine->last_commit_index() >= nodes.at(l2).log_store->next_slot() - 1;
        }), "l2 initial config did not commit");

        /// Add N4 via l2 (fast link) so N4 persists term = t2 > t1!
        nuraft::srv_config srv4(4, 0, "4", "", false, 1);
        auto add2 = nodes.at(l2).server->add_srv(srv4);
        require(add2 && add2->get_accepted(), "add_srv(4) on l2 rejected");
        require(engine.runUntilCondition(2000, [&]() { return nodes.at(4).server->get_term() >= t2; }),
            "N4 did not reach term t2");
        std::cerr << "[" << mode << "] N4 reached term=" << nodes.at(4).server->get_term()
                  << " (l1 stale join in flight has term=" << t1 << ")\n";

        /// Wait for l1's delayed join_cluster_request(term=t1) to arrive at N4!
        /// SimOracle's continuous term-monotonicity check will immediately catch term regression t2 -> t1!
        engine.runUntilMs(simTimeMs() + 4000);
        std::cerr << "[" << mode << "] N4 final term=" << nodes.at(4).server->get_term() << "\n";
    }
    else if (mode == "retry-join-hang")
    {
        /// 3-node cluster + dynamic joiner N4:
        /// Inject a SINGLE transient RPC error on l1 -> N4 during add_srv(4), then IMMEDIATELY heal l1 <-> N4.
        /// peer::handle_rpc_result resets rpc_ = nullptr; handle_ext_resp_err schedules on_retryable_req_err;
        /// when on_retryable_req_err fires, it calls p->send_req without reconnecting rpc_, so peer::send_req
        /// silently returns without invoking ex_resp_handler_ -> ZERO retries sent and add_srv is wedged forever!
        net.link(l1, 4).drop_prob = 1.0;
        require(cluster.addDynamicNode(l1, /*as_learner=*/false), "addDynamicNode(l1) failed");
        engine.runUntilMs(simTimeMs() + 30);

        /// Heal l1 <-> N4 completely so any retry would immediately succeed.
        net.link(l1, 4).drop_prob = 0.0;
        const auto dropped_before = net.getStats().requests_dropped_loss;
        std::cerr << "[" << mode << "] Transient drop injected (dropped=" << dropped_before
                  << "); healed l1<->N4; waiting 8000ms for NuRaft retry...\n";

        engine.runUntilMs(simTimeMs() + 8000);
        auto cur_cfg = nodes.at(l1).server->get_config();
        const bool joined = (cur_cfg && cur_cfg->get_server(4) != nullptr);
        std::cerr << "[" << mode << "] After 8000ms on healed link: N4 in config=" << joined
                  << " N4.term=" << nodes.at(4).server->get_term()
                  << " N4.applied=" << nodes.at(4).machine->last_commit_index() << "\n";
        require(!joined, "N4 unexpectedly joined after transient error");
    }
    else
    {
        throw std::runtime_error("Unknown --repro-nuraft mode: " + mode);
    }
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
        bool lazy_startup = false;
        bool snapshot_vote_safety = false;
        bool huge_lsmt_values = false;
        bool same_path_multi_churn = false;
        bool snapshot_versions_mix = false;
        bool autofwd_missing_leader = false;
        size_t repro_nested_multi_depth = 0;
        std::string repro_nuraft_mode;
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
            else if (opt == "--lazy-startup")
                lazy_startup = true;
            else if (opt == "--snapshot-vote-safety")
                snapshot_vote_safety = true;
            else if (opt == "--huge-lsmt-values")
                huge_lsmt_values = true;
            else if (opt == "--same-path-multi-churn")
                same_path_multi_churn = true;
            else if (opt == "--snapshot-versions-mix")
                snapshot_versions_mix = true;
            else if (opt == "--autofwd-missing-leader")
                autofwd_missing_leader = true;
            else if (opt == "--repro-nested-multi" && i + 1 < argc)
                repro_nested_multi_depth = std::stoull(argv[++i]);
            else if (opt == "--repro-nuraft" && i + 1 < argc)
                repro_nuraft_mode = argv[++i];
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

        if (repro_nested_multi_depth > 0)
        {
            runNestedMultiStackOverflowRepro(repro_nested_multi_depth);
            return 0;
        }

        if (!repro_nuraft_mode.empty())
        {
            runNuRaftLiveRepro(repro_nuraft_mode, state_dir, seed, nuraft_log_level);
            return 0;
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
            broken_tail_writeat,
            lazy_startup,
            snapshot_vote_safety,
            huge_lsmt_values,
            same_path_multi_churn,
            snapshot_versions_mix,
            autofwd_missing_leader);

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
