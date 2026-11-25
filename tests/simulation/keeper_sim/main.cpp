#include <SimClock.h>
#include <SimCluster.h>
#include <SimEngine.h>
#include <SimFeatureTests.h>
#include <SimSwarm.h>
#include <SimWorkloads.h>

#include <Common/DNSResolver.h>
#include <Common/ZooKeeper/KeeperClientCLI/KeeperClient.h>
#include <Common/ZooKeeper/TestKeeper.h>
#include <Common/ZooKeeper/ZooKeeper.h>
#include <Common/ZooKeeper/ZooKeeperArgs.h>
#include <Common/ZooKeeper/ZooKeeperCommon.h>
#include <Common/ZooKeeper/ZooKeeperIO.h>
#include <Common/ZooKeeper/ZooKeeperNodeCache.h>
#include <Common/ZooKeeper/ZooKeeperWithFaultInjection.h>
#include <Compression/CompressedReadBuffer.h>
#include <Compression/CompressedWriteBuffer.h>
#include <Compression/CompressionFactory.h>
#include <Coordination/FourLetterCommand.h>
#include <Coordination/InMemoryLogStore.h>
#include <Coordination/KeeperConnectionStats.h>
#include <Coordination/KeeperLogStore.h>
#include <Coordination/KeeperMemNodesStorage.h>
#include <Coordination/KeeperReadThreadPool.h>
#include <Coordination/KeeperReconfiguration.h>
#include <Coordination/KeeperStateManager.h>
#include <Coordination/KeeperStorage.h>
#include <Coordination/KeeperStorageImpl.h>
#include <Coordination/ZooKeeperDataReader.h>
#include <Core/ServerUUID.h>
#include <DataTypes/DataTypeEnum.h>
#include <Disks/DiskLocal.h>
#include <IO/ReadBufferFromString.h>
#include <IO/WriteBufferFromFile.h>
#include <IO/WriteBufferFromString.h>
#include <Poco/Event.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Util/MapConfiguration.h>
#include <Poco/Util/XMLConfiguration.h>
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

namespace DB::CoordinationSetting
{
    extern const CoordinationSettingsUInt64 parallel_read_threads;
    extern const CoordinationSettingsUInt64 parallel_read_chunk_size;
    extern const CoordinationSettingsUInt64 parallel_read_min_batch;
}

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
    cfg.num_nodes = (mode == "stale-matched-idx" || mode == "figure8"
        || mode == "vote-resp-split-brain" || mode == "prevote-disrupts-follower") ? 5 : 3;
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
    else if (mode == "prevote-disrupts-follower")
    {
        /// Bug #46b: handle_prevote_resp never checks `role_ == candidate` or `!hb_alive_`,
        /// and handle_append_entries never invalidates in-flight pre_vote_.
        /// In a 5-node cluster (quorum = 3), partition l1 from the 4 followers briefly so a follower
        /// collects 3 accepted pre_vote_responses(term=t1, accepted=true) in flight; heal l1 so the
        /// cluster is completely healthy in term t1 (hb_alive_=true, leader=l1); then when those
        /// delayed pre_vote_responses arrive at the healthy follower, pre_vote_.dead_ reaches quorum 3
        /// and calls initiate_vote(), incrementing term to t1+1 and dethroning healthy leader l1!
        std::set<int> followers;
        for (int id = 1; id <= 5; ++id)
            if (id != l1)
                followers.insert(id);

        std::map<int, int> held_prevotes;
        int prevote_winner = 0;
        net.on_deliver_response_hook = [&](int src, int /*dst*/, nuraft::ptr<nuraft::resp_msg> & resp, nuraft::rpc_handler & cb)
        {
            if (resp && resp->get_type() == nuraft::msg_type::pre_vote_response
                && resp->get_term() == t1 && resp->get_accepted())
            {
                int cnt = ++held_prevotes[src];
                if (cnt >= 3 && prevote_winner == 0)
                    prevote_winner = src;
                auto resp_copy = resp;
                auto cb_copy = cb;
                engine.scheduleAfterMs(700, "delayed-prevote-resp", [resp_copy, cb_copy]() mutable
                {
                    nuraft::ptr<nuraft::rpc_exception> no_err;
                    cb_copy(resp_copy, no_err);
                });
                return;
            }
            nuraft::ptr<nuraft::rpc_exception> no_err;
            cb(resp, no_err);
        };

        /// Cut l1 from all 4 followers just long enough for one follower to get 3 accepted pre_vote_responses in flight.
        net.partitionSymmetric({l1}, followers);
        require(engine.runUntilCondition(5000, [&]() { return prevote_winner != 0; }),
            "No follower accumulated 3 accepted pre_vote_responses in term t1");

        /// Immediately heal the entire cluster! l1 resumes heartbeats to f1 & f2 in term t1.
        net.healAll();
        engine.runUntilMs(simTimeMs() + 200);

        std::cerr << "[" << mode << "] Cluster healthy in term=" << t1
                  << " (l1.is_leader=" << nodes.at(l1).server->is_leader()
                  << " l1.term=" << nodes.at(l1).server->get_term()
                  << ", follower=N" << prevote_winner
                  << " sees leader=N" << nodes.at(prevote_winner).server->get_leader()
                  << " term=" << nodes.at(prevote_winner).server->get_term() << ")\n";
        require(nodes.at(l1).server->is_leader() && nodes.at(l1).server->get_term() == t1,
            "l1 should still be healthy leader in term t1");
        require(!nodes.at(prevote_winner).server->is_leader()
            && nodes.at(prevote_winner).server->get_leader() == l1
            && nodes.at(prevote_winner).server->get_term() == t1,
            "prevote_winner should be a healthy follower of l1 in term t1");

        /// Wait for the delayed pre_vote_responses(term=t1, accepted=true) to arrive at healthy follower `prevote_winner`.
        engine.runUntilMs(simTimeMs() + 900);

        const uint64_t new_term = nodes.at(prevote_winner).server->get_term();
        std::cerr << "[" << mode << "] REPRODUCED! After delayed pre_vote_responses arrived at healthy follower N" << prevote_winner
                  << ": N" << prevote_winner << ".term=" << new_term
                  << " l1.term=" << nodes.at(l1).server->get_term()
                  << " l1.is_leader=" << nodes.at(l1).server->is_leader() << "\n";
        require(new_term > t1, "Healthy follower did not disrupt leader upon delayed pre_vote_response");
    }
    else if (mode == "vote-resp-split-brain")
    {
        /// Bug #46a: become_follower() never sets election_completed_=true, and handle_vote_resp()
        /// never checks `role_ == srv_role::candidate` nor deduplicates votes by `resp.get_src()`.
        /// In a 5-node cluster, N_cand runs in term T (gets self + N_ally = 2 votes), while
        /// the other 3 nodes elect N_real in the SAME term T. N_real sends append_entries(T) to
        /// N_cand, forcing N_cand into become_follower() in term T (role_ == follower, leader_ == N_real,
        /// election_completed_ == false!). A duplicate/delayed request_vote_response(term=T, accepted=true)
        /// from N_ally then elects FOLLOWER N_cand as a SECOND LEADER in the SAME term T!
        std::vector<int> followers;
        for (int id = 1; id <= 5; ++id)
            if (id != l1)
                followers.push_back(id);
        const std::vector<int> grp = {followers[0], followers[1], followers[2]};
        const int n_m1 = l1;
        const int n_m3 = followers[3];

        /// Whichever node in `grp` gets 2 accepted pre_vote_responses in term t1 FIRST becomes `n_cand`!
        /// Right BEFORE its 2nd pre_vote_response triggers initiate_vote(), we cut `n_cand` from one of its
        /// two group peers (`n_m2`), leaving ONLY `n_ally` to vote for `n_cand` in target_term (2/5 votes!).
        /// Meanwhile `n_m2` stays unvoted in term t1 and teams up with {n_m1, n_m3} to elect `n_real` in target_term!
        const uint64_t target_term = t1 + 1;
        int n_cand = 0;
        int n_ally = 0;
        int n_m2 = 0;
        std::map<int, int> prevote_cnt;
        nuraft::ptr<nuraft::resp_msg> captured_vote_resp;
        nuraft::rpc_handler captured_cb;

        net.on_deliver_response_hook = [&](int src, int dst, nuraft::ptr<nuraft::resp_msg> & resp, nuraft::rpc_handler & cb)
        {
            if (n_cand == 0 && resp && resp->get_type() == nuraft::msg_type::pre_vote_response
                && resp->get_term() == t1 && resp->get_accepted()
                && std::find(grp.begin(), grp.end(), src) != grp.end())
            {
                if (++prevote_cnt[src] >= 2)
                {
                    n_cand = src;
                    for (int g : grp)
                    {
                        if (g == n_cand)
                            continue;
                        if (n_ally == 0)
                            n_ally = g;
                        else
                            n_m2 = g;
                    }
                    cut_pair(n_cand, n_m2);
                    cut_pair(n_ally, n_m2);
                }
            }
            if (n_cand != 0 && src == n_cand && dst == n_ally && resp
                && resp->get_type() == nuraft::msg_type::request_vote_response
                && resp->get_term() == target_term && resp->get_accepted()
                && !captured_vote_resp)
            {
                captured_vote_resp = resp;
                captured_cb = cb;
            }
            nuraft::ptr<nuraft::rpc_exception> no_err;
            cb(resp, no_err);
        };

        /// Partition `grp` from {n_m1, n_m3} (and isolate n_m1 from n_m3 so l1 steps down or stays in t1).
        net.partitionSymmetric({grp[0], grp[1], grp[2]}, {n_m1, n_m3});
        cut_pair(n_m1, n_m3);

        /// Wait until n_cand gets n_ally's vote in term t1+1 (2/5 votes -> stuck as candidate in t1+1).
        require(engine.runUntilCondition(8000, [&]()
        {
            return captured_vote_resp != nullptr;
        }), "n_cand did not receive n_ally's vote in term t1+1");

        /// Have old leader n_m1 yield leadership (e.g. stepdown / restart) so {n_m1, n_m2, n_m3}
        /// cleanly elect a new leader n_real in the SAME term target_term (t1+1)!
        nodes.at(n_m1).server->yield_leadership(/*immediate_yield=*/true);
        heal_pair(n_m2, n_m3);
        heal_pair(n_m1, n_m2);
        heal_pair(n_m1, n_m3);
        int n_real = 0;
        require(engine.runUntilCondition(8000, [&]()
        {
            for (int id : {n_m1, n_m2, n_m3})
            {
                if (nodes.at(id).server->is_leader() && nodes.at(id).server->get_term() == target_term)
                {
                    n_real = id;
                    return true;
                }
            }
            return false;
        }), "Majority {m1,m2,m3} did not elect leader in target_term");

        /// Connect n_real <-> n_cand so n_real's append_entries(target_term) forces n_cand to become_follower()!
        heal_pair(n_real, n_cand);
        require(engine.runUntilCondition(3000, [&]()
        {
            return !nodes.at(n_cand).server->is_leader()
                && nodes.at(n_cand).server->get_leader() == n_real
                && nodes.at(n_cand).server->get_term() == target_term;
        }), "n_cand did not step down to follower of n_real in target_term");

        std::cerr << "[" << mode << "] Before duplicate vote_resp: N" << n_real
                  << " is LEADER(term=" << nodes.at(n_real).server->get_term() << "), N" << n_cand
                  << " is FOLLOWER(term=" << nodes.at(n_cand).server->get_term()
                  << ", leader=N" << nodes.at(n_cand).server->get_leader() << ")\n";

        /// Re-deliver n_ally's captured request_vote_response(term=target_term, accepted=true) to FOLLOWER n_cand!
        nuraft::ptr<nuraft::rpc_exception> no_err;
        captured_cb(captured_vote_resp, no_err);

        const bool real_is_leader = nodes.at(n_real).server->is_leader();
        const bool cand_is_leader = nodes.at(n_cand).server->is_leader();
        const uint64_t real_term = nodes.at(n_real).server->get_term();
        const uint64_t cand_term = nodes.at(n_cand).server->get_term();
        std::cerr << "[" << mode << "] REPRODUCED TWO LEADERS IN SAME TERM " << target_term << "! "
                  << "N" << n_real << ".is_leader=" << real_is_leader << "(term=" << real_term << ") "
                  << "AND N" << n_cand << ".is_leader=" << cand_is_leader << "(term=" << cand_term << ")\n";
        require(real_is_leader && cand_is_leader && real_term == cand_term,
            "Failed to elect two concurrent leaders in the same Raft term");
    }
    else if (mode == "priority-conf-corrupt")
    {
        /// Bug #47: set_priority_v2 and handle_priority_change_req_v2:
        /// 1. Do NOT check `if (config_changing_)` -> append concurrent uncommitted `conf` entries (Raft §6 violation)
        /// 2. Never update `cloned_config->set_prev_log_idx(cur_config->get_log_idx())` -> stale prev_log_idx chain
        /// 3. handle_priority_change_req_v2 appends a `conf` entry and sets config_changing_=true even when
        ///    target server ID does not exist (returning accepted=false)!
        std::vector<int> followers;
        for (int id = 1; id <= 3; ++id)
            if (id != l1)
                followers.push_back(id);
        const int f1 = followers[0];
        const int f2 = followers[1];

        /// Isolate l1 so new conf entries stay uncommitted on l1.
        net.partitionSymmetric({l1}, {f1, f2});

        const uint64_t slot_before = nodes.at(l1).log_store->next_slot();
        auto rm_res = nodes.at(l1).server->remove_srv(f2);
        require(rm_res && rm_res->get_accepted(), "remove_srv(f2) rejected");
        require(engine.runUntilCondition(1000, [&]()
        {
            return nodes.at(l1).log_store->next_slot() > slot_before;
        }), "Expected 1st conf entry");
        const uint64_t conf1_idx = slot_before;

        /// Call set_priority_v2 while remove_srv conf entry at conf1_idx is STILL UNCOMMITTED!
        const bool prio_ok = nodes.at(l1).server->set_priority_v2(f1, 10);
        const uint64_t conf2_idx = conf1_idx + 1;
        require(prio_ok && nodes.at(l1).log_store->next_slot() == conf2_idx + 1,
            "Expected set_priority_v2 to append 2nd concurrent uncommitted conf entry");

        auto e1 = nodes.at(l1).log_store->entry_at(conf1_idx);
        auto e2 = nodes.at(l1).log_store->entry_at(conf2_idx);
        auto c1 = nuraft::cluster_config::deserialize(e1->get_buf());
        auto c2 = nuraft::cluster_config::deserialize(e2->get_buf());

        std::cerr << "[" << mode << "] REPRODUCED concurrent uncommitted conf entries (violating Raft §6): "
                  << "conf1(idx=" << c1->get_log_idx() << ", prev=" << c1->get_prev_log_idx() << ") "
                  << "and conf2(idx=" << c2->get_log_idx() << ", prev=" << c2->get_prev_log_idx() << ") "
                  << "both uncommitted (commit_idx=" << nodes.at(l1).machine->last_commit_index() << ")\n";
        require(c1->get_log_idx() == conf1_idx && c2->get_log_idx() == conf2_idx
            && nodes.at(l1).machine->last_commit_index() < conf1_idx,
            "Expected two concurrent uncommitted conf entries");

        /// Also send priority_change_request_v2 for unknown server 999 -> resp declined yet conf3 appended!
        nuraft::req_msg req(
            nodes.at(l1).server->get_term(),
            nuraft::msg_type::priority_change_request_v2,
            f1,
            l1,
            nodes.at(l1).log_store->term_at(conf2_idx),
            conf2_idx,
            nodes.at(l1).machine->last_commit_index());
        auto pbuf = nuraft::buffer::alloc(sizeof(int32_t) * 2);
        nuraft::buffer_serializer bs(pbuf);
        bs.put_i32(999);
        bs.put_i32(42);
        req.log_entries().push_back(nuraft::cs_new<nuraft::log_entry>(
            nodes.at(l1).server->get_term(), pbuf, nuraft::log_val_type::custom));

        const uint64_t slot_before_unknown = nodes.at(l1).log_store->next_slot();
        auto resp = net.process_req(nodes.at(l1).server.get(), req);
        const uint64_t slot_after_unknown = nodes.at(l1).log_store->next_slot();
        std::cerr << "[" << mode << "] REPRODUCED unknown-server priority_change_request(id=999): "
                  << "resp.accepted=" << (resp && resp->get_accepted())
                  << " yet log_store next_slot advanced " << slot_before_unknown
                  << " -> " << slot_after_unknown << " (appended bogus conf entry!)\n";
        require(resp && !resp->get_accepted() && slot_after_unknown == slot_before_unknown + 1,
            "Expected declined response yet appended conf entry for unknown server");
    }
    else if (mode == "out-of-log-null-ctx")
    {
        /// Bug #50b: handle_out_of_log_msg dereferences *msg->ctx_ without null check -> SIGSEGV!
        /// Wire layout of custom_notification_msg: u8 version(0), u8 type(1=out_of_log_range_warning), u32 ctx_len(0).
        int f1 = (l1 == 1) ? 2 : 1;
        auto cbuf = nuraft::buffer::alloc(sizeof(uint8_t) * 2 + sizeof(uint32_t));
        {
            nuraft::buffer_serializer bs(cbuf);
            bs.put_u8(0); // version
            bs.put_u8(1); // out_of_log_range_warning
            bs.put_u32(0); // empty ctx -> ctx_ = nullptr
        }

        nuraft::req_msg req(
            t1,
            nuraft::msg_type::custom_notification_request,
            l1,
            f1,
            nodes.at(l1).log_store->term_at(base_idx),
            base_idx,
            nodes.at(l1).machine->last_commit_index());
        req.log_entries().push_back(nuraft::cs_new<nuraft::log_entry>(t1, cbuf, nuraft::log_val_type::custom));

        std::cerr << "[" << mode << "] Sending out_of_log_range_warning with ctx_=nullptr to N" << f1 << " (expecting SIGSEGV)...\n";
        auto resp = net.process_req(nodes.at(f1).server.get(), req);
        (void)resp;
        throw std::runtime_error("Expected SIGSEGV in handle_out_of_log_msg did not happen");
    }
    else if (mode == "learner-takeover")
    {
        /// Bug #51: handle_leadership_takeover does not check im_learner_ before calling initiate_vote(true),
        /// allowing a non-voting learner to bump its Raft term and initiate an election!
        require(cluster.addDynamicNode(l1, /*as_learner=*/true), "addDynamicNode(as_learner=true) failed");
        require(engine.runUntilCondition(5000, [&]()
        {
            auto c = nodes.at(4).server->get_config();
            auto s4 = c ? c->get_server(4) : nullptr;
            return s4 != nullptr && s4->is_learner() && nodes.at(4).server->get_term() == t1;
        }), "Learner N4 did not join cluster");

        const uint64_t learner_term_before = nodes.at(4).server->get_term();
        /// Wire layout of custom_notification_msg: u8 version(0), u8 type(2=leadership_takeover), u32 ctx_len(0).
        auto cbuf = nuraft::buffer::alloc(sizeof(uint8_t) * 2 + sizeof(uint32_t));
        {
            nuraft::buffer_serializer bs(cbuf);
            bs.put_u8(0); // version
            bs.put_u8(2); // leadership_takeover
            bs.put_u32(0); // empty ctx
        }
        nuraft::req_msg req(
            learner_term_before,
            nuraft::msg_type::custom_notification_request,
            l1,
            4,
            nodes.at(l1).log_store->term_at(base_idx),
            base_idx,
            nodes.at(l1).machine->last_commit_index());
        req.log_entries().push_back(nuraft::cs_new<nuraft::log_entry>(learner_term_before, cbuf, nuraft::log_val_type::custom));

        auto resp = net.process_req(nodes.at(4).server.get(), req);
        (void)resp;
        const uint64_t learner_term_after = nodes.at(4).server->get_term();
        std::cerr << "[" << mode << "] REPRODUCED non-voting learner N4 (is_learner="
                  << nodes.at(4).server->get_config()->get_server(4)->is_learner()
                  << ") ran initiate_vote(true): term bumped " << learner_term_before
                  << " -> " << learner_term_after << "\n";
        require(learner_term_after == learner_term_before + 1,
            "Learner did not increment term on leadership_takeover_request");
    }
    else if (mode == "join-term-rewind")
    {
        /// Bug #60a: process_req does not validate req.get_term() for join_cluster_request, and
        /// handle_join_cluster_req unconditionally overwrites state_->set_term(req.get_term()) + save_state,
        /// rewinding the node's Raft term backwards (Raft §5.1 violation)!
        const int f1 = (l1 == 1) ? 2 : 1;
        const uint64_t high_term = t1 + 4;
        nuraft::req_msg vote_bump(
            high_term,
            nuraft::msg_type::request_vote_request,
            l1,
            f1,
            t1,
            base_idx,
            nodes.at(l1).machine->last_commit_index());
        (void)net.process_req(nodes.at(f1).server.get(), vote_bump);
        const uint64_t term_before = nodes.at(f1).server->get_term();
        require(term_before == high_term, "Failed to advance f1 to high_term");

        auto stale_cfg = nodes.at(l1).server->get_config();
        nuraft::req_msg stale_join(
            t1, // older term < high_term!
            nuraft::msg_type::join_cluster_request,
            l1,
            f1,
            0,
            base_idx,
            nodes.at(l1).machine->last_commit_index());
        stale_join.log_entries().push_back(
            nuraft::cs_new<nuraft::log_entry>(t1, stale_cfg->serialize(), nuraft::log_val_type::conf));

        auto resp = net.process_req(nodes.at(f1).server.get(), stale_join);
        const uint64_t term_after = nodes.at(f1).server->get_term();
        std::cerr << "[" << mode << "] REPRODUCED Raft term rewind on N" << f1
                  << ": stale join_cluster_request(term=" << t1 << ") rewound term "
                  << term_before << " -> " << term_after
                  << " (accepted=" << (resp && resp->get_accepted()) << ")\n";
        require(resp && resp->get_accepted() && term_after < term_before,
            "Expected join_cluster_request from stale term to rewind term");
    }
    else if (mode == "log-sync-empty-oob")
    {
        /// Bug #60b: handle_log_sync_req (handle_join_leave.cxx:411) evaluates entries[0]->get_val_type()
        /// inside p_db(...) BEFORE checking `if (entries.size() != 1 || ...)` -> SIGSEGV when debug logging is on!
        const int f1 = (l1 == 1) ? 2 : 1;
        nuraft::req_msg empty_sync(
            t1,
            nuraft::msg_type::sync_log_request,
            l1,
            f1,
            0,
            base_idx,
            nodes.at(l1).machine->last_commit_index());
        std::cerr << "[" << mode << "] Sending empty sync_log_request to N" << f1
                  << " with nuraft_log_level=" << nuraft_log_level << " (expecting SIGSEGV)...\n";
        auto resp = net.process_req(nodes.at(f1).server.get(), empty_sync);
        (void)resp;
        throw std::runtime_error("Expected SIGSEGV in handle_log_sync_req did not happen");
    }
    else if (mode == "leave-resp-unchecked" || mode == "log-sync-rejected-joins")
    {
        /// Bug #60c/d:
        ///  - handle_leave_cluster_resp ignores role_, leave_flag, and config_changing_ -> removes healthy peer!
        ///  - handle_log_sync_resp ignores resp.get_accepted() == false -> adds rejecting joiner into cluster config!
        const int f1 = (l1 == 1) ? 2 : 1;
        bool injected = false;
        net.on_deliver_response_hook = [&](int src, int /*dst*/, nuraft::ptr<nuraft::resp_msg> & resp, nuraft::rpc_handler & handler)
        {
            nuraft::ptr<nuraft::rpc_exception> no_err;
            if (injected || src != l1 || !resp || resp->get_type() != nuraft::msg_type::join_cluster_response)
            {
                handler(resp, no_err);
                return;
            }
            injected = true;
            if (mode == "leave-resp-unchecked")
            {
                /// Forge a leave_cluster_response(accepted=true) from healthy peer f1 (whose leave_flag is FALSE!)
                /// and deliver it directly to ex_resp_handler_.
                auto forged = nuraft::cs_new<nuraft::resp_msg>(t1, nuraft::msg_type::leave_cluster_response, f1, l1);
                forged->accept(base_idx + 1);
                handler(forged, no_err);
            }
            else
            {
                /// Deliver a REJECTED sync_log_response (accepted=false!) for joining node 4 to ex_resp_handler_.
                /// handle_log_sync_resp ignores get_accepted() == false and immediately appends a conf adding N4!
                auto rejected = nuraft::cs_new<nuraft::resp_msg>(
                    t1, nuraft::msg_type::sync_log_response, 4, l1, nodes.at(l1).log_store->next_slot());
                require(!rejected->get_accepted(), "Expected rejected sync_log_response");
                handler(rejected, no_err);
            }
        };

        const uint64_t next_before = nodes.at(l1).log_store->next_slot();
        require(cluster.addDynamicNode(l1, /*as_learner=*/false), "addDynamicNode failed");
        require(engine.runUntilCondition(2000, [&]() { return injected; }), "Hook was not triggered");
        const uint64_t next_after = nodes.at(l1).log_store->next_slot();
        require(next_after > next_before, "Expected leader to append a conf entry");

        auto last_le = nodes.at(l1).log_store->entry_at(next_after - 1);
        require(last_le && last_le->get_val_type() == nuraft::log_val_type::conf, "Expected conf log entry");
        last_le->get_buf().pos(0);
        auto new_conf = nuraft::cluster_config::deserialize(last_le->get_buf());

        if (mode == "leave-resp-unchecked")
        {
            const bool f1_removed = (new_conf->get_server(f1) == nullptr);
            std::cerr << "[" << mode << "] REPRODUCED unchecked handle_leave_cluster_resp: healthy peer N" << f1
                      << " (never asked to leave!) was removed from conf at idx=" << (next_after - 1)
                      << " (servers=" << new_conf->get_servers().size() << ", has_f1=" << !f1_removed << ")\n";
            require(f1_removed, "Expected healthy peer f1 to be removed by unchecked leave_cluster_response");
        }
        else
        {
            const bool n4_added = (new_conf->get_server(4) != nullptr);
            std::cerr << "[" << mode << "] REPRODUCED handle_log_sync_resp ignoring accepted=false: rejected joiner N4"
                      << " was added into conf at idx=" << (next_after - 1)
                      << " (servers=" << new_conf->get_servers().size() << ", has_N4=" << n4_added << ")\n";
            require(n4_added, "Expected rejecting joiner N4 to be added to cluster config");
        }
        net.on_deliver_response_hook = nullptr;
    }
    else
    {
        throw std::runtime_error("Unknown --repro-nuraft mode: " + mode);
    }
}

void runKeeperLiveRepro(const std::string & mode, const std::filesystem::path & state_dir)
{
    using namespace keeper_sim;

    if (mode == "chroot-root-watch")
    {
        Coordination::ZooKeeperExistsRequest req;
        req.path = "/";
        req.addRootPath("/clickhouse");
        std::cerr << "[" << mode << "] After addRootPath(\"/clickhouse\") on \"/\": req.path=\"" << req.path << "\"\n";
        require(req.path == "/clickhouse", "Unexpected addRootPath result");

        Coordination::ZooKeeperWatchResponse watch_resp;
        watch_resp.path = req.path;
        try
        {
            watch_resp.removeRootPath("/clickhouse");
            throw std::runtime_error("Expected ZDATAINCONSISTENCY exception was not thrown");
        }
        catch (const Coordination::Exception & e)
        {
            std::cerr << "[" << mode << "] REPRODUCED Coordination::Exception code=" << static_cast<int>(e.code)
                      << " msg=\"" << e.what() << "\"\n";
            require(e.code == Coordination::Error::ZDATAINCONSISTENCY, "Unexpected Coordination error code");
        }
    }
    else if (mode == "changelog-shutdown-rotate")
    {
        require(!state_dir.empty(), "--state-dir is required");
        if (std::filesystem::exists(state_dir))
            std::filesystem::remove_all(state_dir);
        std::filesystem::create_directories(state_dir);

        auto settings = std::make_shared<DB::CoordinationSettings>();
        auto ctx = std::make_shared<DB::KeeperContext>(/*standalone=*/true, settings);
        auto disk = std::make_shared<DB::DiskLocal>("repro_log", state_dir.string() + "/");
        ctx->setLogDisk(disk);
        ctx->setLogDisk(disk);

        DB::LogFileSettings log_settings;
        log_settings.rotate_interval = 3;
        log_settings.force_sync = false;
        log_settings.compress_logs = false;
        log_settings.startup_read_max_streams = 1;

        DB::FlushSettings flush_settings;
        flush_settings.max_flush_batch_size = 1;
        flush_settings.min_time_between_fsyncs_ms = 0;

        DB::ReadAheadSettings ra_settings;
        ra_settings.enabled = false;
        ra_settings.commit_window_bytes = 0;

        DB::KeeperLogStore store(log_settings, flush_settings, ra_settings, ctx, DB::Changelog::ExecutionMode::Manual);
        store.init(/*last_commited_log_index=*/0, /*logs_to_keep=*/100);

        auto make_entry = [](uint64_t term, const std::string & payload)
        {
            auto buf = nuraft::buffer::alloc(payload.size());
            buf->put_raw(reinterpret_cast<const nuraft::byte *>(payload.data()), payload.size());
            buf->pos(0);
            return nuraft::cs_new<nuraft::log_entry>(term, buf, nuraft::log_val_type::app_log);
        };

        for (int i = 1; i <= 3; ++i)
        {
            auto e = make_entry(1, "entry_" + std::to_string(i));
            store.append(e);
        }
        store.flush();
        while (store.runTask(DB::Changelog::BackgroundTask::FileOperation))
        {
        }

        /// Append 4th entry (queued in write_operations, NOT yet executed) which crosses rotate_interval=3
        /// and will call rotate(4) -> moveChangelogAsync during shutdown() AFTER changelog_operation_queue was finished!
        auto e4 = make_entry(1, "entry_4_triggers_rotate");
        store.append(e4);

        try
        {
            store.shutdownChangelog();
            throw std::runtime_error("Expected SYSTEM_ERROR on shutdown rotation was not thrown");
        }
        catch (const DB::Exception & e)
        {
            std::cerr << "[" << mode << "] REPRODUCED DB::Exception code=" << e.code()
                      << " msg=\"" << e.message() << "\"\n";
        }
    }
    else if (mode == "readahead-mixed-compress")
    {
        require(!state_dir.empty(), "--state-dir is required");
        if (std::filesystem::exists(state_dir))
            std::filesystem::remove_all(state_dir);
        std::filesystem::create_directories(state_dir);

        auto make_entry = [](uint64_t term, const std::string & payload)
        {
            auto buf = nuraft::buffer::alloc(payload.size());
            buf->put_raw(reinterpret_cast<const nuraft::byte *>(payload.data()), payload.size());
            buf->pos(0);
            return nuraft::cs_new<nuraft::log_entry>(term, buf, nuraft::log_val_type::app_log);
        };

        auto make_ctx = [&]()
        {
            auto settings = std::make_shared<DB::CoordinationSettings>();
            auto ctx = std::make_shared<DB::KeeperContext>(/*standalone=*/true, settings);
            auto disk = std::make_shared<DB::DiskLocal>("repro_log", state_dir.string() + "/");
            ctx->setLogDisk(disk);
            return ctx;
        };

        DB::FlushSettings manual_flush;
        manual_flush.max_flush_batch_size = 1;
        manual_flush.min_time_between_fsyncs_ms = 0;

        DB::ReadAheadSettings manual_ra;
        manual_ra.enabled = false;
        manual_ra.commit_window_bytes = 0;

        /// Phase 1: Write uncompressed changelog_1_3.bin
        {
            DB::LogFileSettings log_settings;
            log_settings.rotate_interval = 3;
            log_settings.force_sync = false;
            log_settings.compress_logs = false;
            log_settings.latest_logs_cache_entry_count_threshold = 2;
            log_settings.startup_read_max_streams = 1;
            DB::KeeperLogStore store(log_settings, manual_flush, manual_ra, make_ctx(), DB::Changelog::ExecutionMode::Manual);
            store.init(0, 100);
            for (int i = 1; i <= 3; ++i)
            {
                auto e = make_entry(1, "uncompressed_" + std::to_string(i));
                store.append(e);
            }
            store.flush();
            while (store.runTask(DB::Changelog::BackgroundTask::FileOperation))
            {
            }
            store.shutdownChangelog();
        }

        /// Phase 2: Enable compress_logs=true and write compressed changelog_4_6.bin.zstd
        {
            DB::LogFileSettings log_settings;
            log_settings.rotate_interval = 3;
            log_settings.force_sync = false;
            log_settings.compress_logs = true;
            log_settings.latest_logs_cache_entry_count_threshold = 2;
            log_settings.startup_read_max_streams = 1;
            DB::KeeperLogStore store(log_settings, manual_flush, manual_ra, make_ctx(), DB::Changelog::ExecutionMode::Manual);
            store.init(0, 100);
            for (int i = 4; i <= 6; ++i)
            {
                auto e = make_entry(1, "compressed_" + std::to_string(i));
                store.append(e);
            }
            store.flush();
            while (store.runTask(DB::Changelog::BackgroundTask::FileOperation))
            {
            }
            store.shutdownChangelog();
        }

        /// Phase 3: Restart in normal Background mode with bounded latest_logs_cache (holds {5,6}) and ReadAhead enabled.
        /// Reading UNCOMPRESSED entry 2 from changelog_1_3.bin triggers appendNextFileCursors(4) into
        /// changelog_4_6.bin.zstd -> assertNotCompressed throws NOT_IMPLEMENTED!
        {
            DB::LogFileSettings log_settings;
            log_settings.rotate_interval = 3;
            log_settings.force_sync = false;
            log_settings.compress_logs = true;
            log_settings.latest_logs_cache_entry_count_threshold = 2;
            log_settings.startup_read_max_streams = 1;
            DB::ReadAheadSettings ra_settings;
            ra_settings.enabled = true;
            ra_settings.commit_window_bytes = 64 * 1024;
            DB::KeeperLogStore store(log_settings, DB::FlushSettings{}, ra_settings, make_ctx(), DB::Changelog::ExecutionMode::Threaded);
            store.init(0, 100);

            bool commit_threw = false;
            try
            {
                auto e = store.entry_at_ext(2, /*for_commit=*/true);
                (void)e;
            }
            catch (const DB::Exception & e)
            {
                commit_threw = true;
                std::cerr << "[" << mode << "] REPRODUCED entry_at_ext(2, for_commit=true) threw code=" << e.code()
                          << " msg=\"" << e.message() << "\"\n";
            }
            require(commit_threw, "Expected NOT_IMPLEMENTED from entry_at_ext(2, true)");

            auto peer_entries = store.log_entries_ext(2, 4, 0, /*peer_id=*/2);
            std::cerr << "[" << mode << "] REPRODUCED log_entries_ext(2, 4, peer_id=2) returned "
                      << (peer_entries ? std::to_string(peer_entries->size()) : std::string("nullptr")) << "\n";
            require(peer_entries == nullptr, "Expected nullptr from log_entries_ext(2, 4, peer_id=2)");

            store.shutdownChangelog();
        }
    }
    else if (mode == "read-pool-grow-underflow")
    {
        /// Bug #43: Increasing parallel_read_threads on a small batch (< parallel_read_min_batch)
        /// after batch_idx >= 1 spawns new workers with seen_batch_idx = 0 != batch_idx.
        /// They immediately execute phantom batch_idx without incrementing busy_threads,
        /// underflowing busy_threads (chassert crash in debug) or decrementing the NEXT batch's
        /// busy_threads prematurely so execute() returns while another worker is still inside func() (UAF)!
        bool observed_premature_return = false;
        for (int attempt = 0; attempt < 200 && !observed_premature_return; ++attempt)
        {
            DB::KeeperReadThreadPool pool;
            DB::CoordinationSettings s1;
            s1[DB::CoordinationSetting::parallel_read_threads] = 2;
            s1[DB::CoordinationSetting::parallel_read_min_batch] = 10;
            s1[DB::CoordinationSetting::parallel_read_chunk_size] = 4;

            /// Batch 1: advances batch_idx to 1 with 2 running threads.
            pool.execute(16, s1, [](size_t, size_t) {});

            /// Batch 2: hot-reload increases parallel_read_threads to 6, but count=1 < min_batch=10!
            /// Spawns 4 new threads with seen_batch_idx=0 while batch_idx=1 and returns early.
            DB::CoordinationSettings s2 = s1;
            s2[DB::CoordinationSetting::parallel_read_threads] = 6;
            pool.execute(1, s2, [](size_t, size_t) {});

            /// Batch 3: immediately runs a batch where one chunk takes 5ms.
            /// If a newly spawned thread from Batch 2 decrements Batch 3's busy_threads for phantom batch 1
            /// AND again for batch 2, execute() returns BEFORE all workers exit the lambda!
            auto execute_returned = std::make_shared<std::atomic<bool>>(false);
            auto active_workers = std::make_shared<std::atomic<int>>(0);
            auto saw_uaf = std::make_shared<std::atomic<bool>>(false);

            pool.execute(24, s2, [execute_returned, active_workers, saw_uaf](size_t begin, size_t)
            {
                active_workers->fetch_add(1, std::memory_order_acq_rel);
                if (begin == 0)
                    std::this_thread::sleep_for(std::chrono::milliseconds(5));
                if (execute_returned->load(std::memory_order_acquire))
                    saw_uaf->store(true, std::memory_order_release);
                active_workers->fetch_sub(1, std::memory_order_acq_rel);
            });
            execute_returned->store(true, std::memory_order_release);
            if (active_workers->load(std::memory_order_acquire) > 0)
                observed_premature_return = true;

            /// Wait for any straggling worker before shutting down pool cleanly.
            while (active_workers->load(std::memory_order_acquire) > 0)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            if (saw_uaf->load(std::memory_order_acquire))
                observed_premature_return = true;

            if (observed_premature_return)
                std::cerr << "[" << mode << "] REPRODUCED premature execute() return (worker still running caller lambda after return!) on attempt " << attempt << "\n";
            pool.shutdown();
        }
        require(observed_premature_return, "Did not observe premature execute() return");
    }
    else if (mode == "multi-check-session-error")
    {
        /// Bug #44: ZooKeeper::multiImpl(..., check_session_valid=true) calls getFailedOpIndex(code, responses)
        /// for any non-ZOK, non-hardware error (e.g. ZBADARGUMENTS / ZUNIMPLEMENTED / ZAPIERROR / ZCLOSING)
        /// even when the server rejected the Multi at header level (all sub-responses ZOK) -> throws LOGICAL_ERROR!
        struct MultiHeaderErrorKeeper final : public Coordination::IKeeper
        {
            std::unique_ptr<Coordination::TestKeeper> inner;
            bool inject_multi_error = false;
            explicit MultiHeaderErrorKeeper(const zkutil::ZooKeeperArgs & a) : inner(std::make_unique<Coordination::TestKeeper>(a)) {}
            bool isExpired() const override { return inner->isExpired(); }
            silk::FiberEvent & getExpiredEvent() override { return inner->getExpiredEvent(); }
            std::optional<int8_t> getConnectedNodeIdx() const override { return inner->getConnectedNodeIdx(); }
            String getConnectedHostPort() const override { return inner->getConnectedHostPort(); }
            int64_t getConnectionXid() const override { return inner->getConnectionXid(); }
            int64_t getSessionID() const override { return inner->getSessionID(); }
            int64_t getLastZXIDSeen() const override { return inner->getLastZXIDSeen(); }
            bool isFeatureEnabled(DB::KeeperFeatureFlag f) const override { return inner->isFeatureEnabled(f); }
            const DB::KeeperFeatureFlags * getKeeperFeatureFlags() const override { return inner->getKeeperFeatureFlags(); }
            void create(const String & p, const String & d, bool e, bool s, const Coordination::ACLs & a, Coordination::CreateCallback cb) override { inner->create(p, d, e, s, a, std::move(cb)); }
            void remove(const String & p, int32_t v, Coordination::RemoveCallback cb) override { inner->remove(p, v, std::move(cb)); }
            void removeRecursive(const String & p, uint32_t l, Coordination::RemoveRecursiveCallback cb) override { inner->removeRecursive(p, l, std::move(cb)); }
            void listRecursive(const String & p, uint32_t l, Coordination::ListRecursiveCallback cb) override { inner->listRecursive(p, l, std::move(cb)); }
            void listWithOptions(const String & p, const Coordination::ListOptions & o, Coordination::ListWithOptionsCallback cb, Coordination::WatchCallbackPtrOrEventPtr w) override { inner->listWithOptions(p, o, std::move(cb), std::move(w)); }
            void exists(const String & p, Coordination::ExistsCallback cb, Coordination::WatchCallbackPtrOrEventPtr w) override { inner->exists(p, std::move(cb), std::move(w)); }
            void get(const String & p, Coordination::GetCallback cb, Coordination::WatchCallbackPtrOrEventPtr w) override { inner->get(p, std::move(cb), std::move(w)); }
            void set(const String & p, const String & d, int32_t v, Coordination::SetCallback cb) override { inner->set(p, d, v, std::move(cb)); }
            void list(const String & p, Coordination::ListRequestType t, Coordination::ListCallback cb, Coordination::WatchCallbackPtrOrEventPtr w, bool ws, bool wd) override { inner->list(p, t, std::move(cb), std::move(w), ws, wd); }
            void check(const String & p, int32_t v, Coordination::CheckCallback cb) override { inner->check(p, v, std::move(cb)); }
            void sync(const String & p, Coordination::SyncCallback cb) override { inner->sync(p, std::move(cb)); }
            void reconfig(std::string_view j, std::string_view l, std::string_view n, int32_t v, Coordination::ReconfigCallback cb) override { inner->reconfig(j, l, n, v, std::move(cb)); }
            void getACL(const String & p, Coordination::GetACLCallback cb) override { inner->getACL(p, std::move(cb)); }
            void finalize(const String & r) override { inner->finalize(r); }
            void multi(const Coordination::Requests & reqs, Coordination::MultiCallback cb) override { multi(std::span(reqs), std::move(cb)); }
            void multi(std::span<const Coordination::RequestPtr> reqs, Coordination::MultiCallback cb) override
            {
                if (!inject_multi_error)
                {
                    inner->multi(reqs, std::move(cb));
                    return;
                }
                /// Matches ZooKeeperImpl::receiveEvent on header-level non-ZOK error:
                /// response = makeResponse() (all sub-responses default ZOK), response->error = ZBADARGUMENTS.
                Coordination::ZooKeeperMultiRequest zk_req(reqs, {});
                auto resp = zk_req.makeResponse();
                resp->error = Coordination::Error::ZBADARGUMENTS;
                cb(dynamic_cast<const Coordination::MultiResponse &>(*resp));
            }
        };

        MultiHeaderErrorKeeper * raw_keeper = nullptr;
        DB::ServerUUID::setRandomForUnitTests();
        auto zk = zkutil::ZooKeeper::createFromImpl([&]() -> std::unique_ptr<Coordination::IKeeper>
        {
            zkutil::ZooKeeperArgs args("localhost:2181");
            auto k = std::make_unique<MultiHeaderErrorKeeper>(args);
            raw_keeper = k.get();
            return k;
        });

        raw_keeper->inject_multi_error = true;
        Coordination::Requests ops{zkutil::makeCreateRequest("/x", "v", zkutil::CreateMode::Persistent)};
        Coordination::Responses responses;
        try
        {
            zk->tryMulti(ops, responses, /*check_session_valid=*/true);
            throw std::runtime_error("Expected DB::Exception(LOGICAL_ERROR) was not thrown");
        }
        catch (const DB::Exception & e)
        {
            std::cerr << "[" << mode << "] REPRODUCED DB::Exception code=" << e.code()
                      << " msg=\"" << e.message() << "\"\n";
            require(e.code() == DB::ErrorCodes::LOGICAL_ERROR, "Expected ErrorCodes::LOGICAL_ERROR");
        }
    }
    else if (mode == "fault-injection-async-bug")
    {
        /// Bug #45: ZooKeeperWithFaultInjection::injectFailureBeforeOperationPromise
        /// forgets `return true;` when `!keeper`, falling through to set_exception a SECOND time
        /// (std::future_error) or dereferencing nullptr `keeper->impl`!
        auto faulty = std::make_shared<DB::ZooKeeperWithFaultInjection>(
            /*keeper_=*/nullptr,
            /*fault_injection_probability=*/1.0,
            /*fault_injection_seed=*/42,
            "repro",
            nullptr);
        try
        {
            auto fut = faulty->asyncTryGet("/test");
            (void)fut;
            throw std::runtime_error("Expected std::future_error from double set_exception");
        }
        catch (const std::future_error & e)
        {
            std::cerr << "[" << mode << "] REPRODUCED std::future_error from missing return in injectFailureBeforeOperationPromise: \""
                      << e.what() << "\"\n";
        }
    }
    else if (mode == "nuraft-buffer-oob")
    {
        /// Bug #50a: nuraft::buffer::get_str() at EOF advances pos() past size() (pos = size + 1),
        /// causing `size() - pos()` to underflow to SIZE_MAX and bypassing ALL subsequent bounds checks!
        auto buf = nuraft::buffer::alloc(4);
        buf->put(static_cast<nuraft::int32>(0x12345678));
        require(buf->pos() == 4 && buf->size() == 4, "Expected buffer at EOF");

        const char * s = buf->get_str();
        std::cerr << "[" << mode << "] After get_str() at EOF: ret=" << (s ? s : "nullptr")
                  << " pos=" << buf->pos() << " size=" << buf->size()
                  << " (size - pos = " << (buf->size() - buf->pos()) << " UNDERFLOW!)\n";
        require(buf->pos() == 5 && buf->pos() > buf->size(), "Expected pos() > size() after get_str() at EOF");

        /// Because `size() - pos()` underflowed to SIZE_MAX, get_byte() DOES NOT THROW overflow_error
        /// and reads past the end of the heap buffer!
        nuraft::byte oob_byte = buf->get_byte();
        std::cerr << "[" << mode << "] REPRODUCED OOB read: get_byte() succeeded past EOF! byte="
                  << static_cast<int>(oob_byte) << " new_pos=" << buf->pos() << "\n";
    }
    else if (mode == "keepermap-drop-lock-badversion")
    {
        /// Bug #53: StorageKeeperMap::StorageKeeperMap (lines 575-579) passes `drop_lock_version`
        /// (the version of persistent `zk_dropped_lock_version_path`, always >= 1) as the expected
        /// version for `makeRemoveRequest(zk_dropped_lock_path, drop_lock_version)` (an ephemeral node
        /// whose version is always 0) -> retry multi unconditionally fails with ZBADVERSION!
        DB::ServerUUID::setRandomForUnitTests();
        auto zk = zkutil::ZooKeeper::createFromImpl([]() -> std::unique_ptr<Coordination::IKeeper>
        {
            zkutil::ZooKeeperArgs args("localhost:2181");
            return std::make_unique<Coordination::TestKeeper>(args);
        });

        zk->createAncestors("/km/metadata/dropped/lock");
        zk->create("/km/metadata/drop_lock_version", "", zkutil::CreateMode::Persistent);

        /// Step 1: Exact initial drop lock acquisition from StorageKeeperMap.cpp:613-637.
        Coordination::Requests init_ops{
            zkutil::makeCreateRequest("/km/metadata/dropped/lock", "", zkutil::CreateMode::Ephemeral),
            zkutil::makeSetRequest("/km/metadata/drop_lock_version", "table_1", -1),
        };
        Coordination::Responses init_resps;
        auto init_code = zk->tryMulti(init_ops, init_resps);
        require(init_code == Coordination::Error::ZOK, "Initial drop lock multi failed");
        int32_t drop_lock_version = dynamic_cast<const Coordination::SetResponse &>(*init_resps[1]).stat.version;
        std::cerr << "[" << mode << "] Initial drop_lock_version=" << drop_lock_version << "\n";
        require(drop_lock_version == 1, "Expected drop_lock_version == 1");

        /// Step 2: Exact retry multi from StorageKeeperMap.cpp:575-583.
        Coordination::Requests retry_ops{
            zkutil::makeSetRequest("/km/metadata/drop_lock_version", "table_1", drop_lock_version),
            zkutil::makeRemoveRequest("/km/metadata/dropped/lock", drop_lock_version),
            zkutil::makeCreateRequest("/km/metadata/dropped/lock", "", zkutil::CreateMode::Ephemeral),
        };
        Coordination::Responses retry_resps;
        auto retry_code = zk->tryMulti(retry_ops, retry_resps);
        std::cerr << "[" << mode << "] REPRODUCED StorageKeeperMap retry multi failed with code="
                  << static_cast<int>(retry_code)
                  << " (op0=" << static_cast<int>(retry_resps[0]->error)
                  << ", op1=" << static_cast<int>(retry_resps[1]->error) << " ZBADVERSION on ephemeral lock v0!)\n";
        require(retry_code == Coordination::Error::ZBADVERSION
            && retry_resps[1]->error == Coordination::Error::ZBADVERSION,
            "Expected ZBADVERSION on ephemeral drop lock removal");
    }
    else if (mode == "rcfg-null-deref")
    {
        /// Bug #57: KeeperDispatcher::checkReconfigCommandActions / checkReconfigCommandPreconditions
        /// call Poco::JSON::Object::getArray("actions") / getObject("preconditions") and dereference the
        /// returned Poco::SharedPtr without null checks -> remote SIGSEGV on malformed JSON like `rcfg {"actions": null}`!
        Poco::JSON::Parser parser;
        auto parsed = parser.parse(R"({"preconditions": null, "actions": null})");
        auto reconfig_command = parsed.extract<Poco::JSON::Object::Ptr>();
        require(reconfig_command->has("preconditions") && reconfig_command->has("actions"), "Expected keys present");

        auto preconditions = reconfig_command->getObject("preconditions");
        auto actions = reconfig_command->getArray("actions");
        std::cerr << "[" << mode << "] preconditions.isNull()=" << preconditions.isNull()
                  << " actions.isNull()=" << actions.isNull() << "\n";
        bool threw_null_ptr = false;
        try
        {
            for (const auto & action : *actions)
                (void)action;
        }
        catch (const Poco::NullPointerException & e)
        {
            threw_null_ptr = true;
            std::cerr << "[" << mode << "] REPRODUCED Poco::NullPointerException: " << e.displayText() << "\n";
        }
        require(threw_null_ptr, "Expected Poco::NullPointerException on null Poco::JSON::Array::Ptr");
    }
    else if (mode == "converter-session-digest-bug")
    {
        /// Bug #58: ZooKeeperDataReader::deserializeStorageData never updates storage.nodes_digest while
        /// KeeperConverter enables digestEnabled(true), AND deserializeTxn handles CreateSession by calling
        /// storage.getSessionID(timeout) instead of storage.addSessionID(session_id, timeout)!
        require(!state_dir.empty(), "--state-dir is required for converter-session-digest-bug");
        if (std::filesystem::exists(state_dir))
            std::filesystem::remove_all(state_dir);
        std::filesystem::create_directories(state_dir);

        const auto snap_path = (state_dir / "snapshot.5").string();
        {
            DB::WriteBufferFromFile wb(snap_path);
            Coordination::write(static_cast<int32_t>(1514885966), wb); // "ZKSN"
            Coordination::write(static_cast<int32_t>(2), wb);          // version 2
            Coordination::write(static_cast<int64_t>(0), wb);          // dbid
            Coordination::write(static_cast<int32_t>(0), wb);          // 0 sessions
            Coordination::write(static_cast<int32_t>(0), wb);          // 0 ACLs
            /// Single persistent node "/snap_node" with data "hello"
            Coordination::write(std::string("/snap_node"), wb);
            Coordination::write(std::string("hello"), wb);
            Coordination::write(static_cast<int64_t>(0), wb); // acl_id
            Coordination::write(static_cast<int64_t>(2), wb); // czxid
            Coordination::write(static_cast<int64_t>(2), wb); // mzxid
            Coordination::write(static_cast<int64_t>(1000), wb); // ctime
            Coordination::write(static_cast<int64_t>(1000), wb); // mtime
            Coordination::write(static_cast<int32_t>(0), wb); // version
            Coordination::write(static_cast<int32_t>(0), wb); // cversion
            Coordination::write(static_cast<int32_t>(0), wb); // aversion
            Coordination::write(static_cast<int64_t>(0), wb); // ephemeralOwner
            Coordination::write(static_cast<int64_t>(2), wb); // pzxid
            /// Terminator "/"
            Coordination::write(std::string("/"), wb);
            wb.finalize();
        }

        const int64_t zk_session_id = 0x10005;
        const auto log_path = (state_dir / "log.6").string();
        {
            DB::WriteBufferFromFile wb(log_path);
            Coordination::write(static_cast<int32_t>(1514884167), wb); // "ZKLG"
            Coordination::write(static_cast<int32_t>(2), wb);          // version 2
            Coordination::write(static_cast<int64_t>(0), wb);          // dbid

            auto write_txn = [&](int64_t sid, int32_t xid, int64_t zxid, int64_t time, int32_t type, const std::string & body)
            {
                DB::WriteBufferFromOwnString rec;
                Coordination::write(sid, rec);
                Coordination::write(xid, rec);
                Coordination::write(zxid, rec);
                Coordination::write(time, rec);
                Coordination::write(type, rec);
                rec.write(body.data(), body.size());
                const std::string payload = rec.str();
                Coordination::write(static_cast<int64_t>(1), wb); // non-zero checksum
                Coordination::write(static_cast<int32_t>(payload.size()), wb);
                wb.write(payload.data(), payload.size());
                Coordination::write(static_cast<int8_t>(0x42), wb); // ZK txn trailing 'B' sentinel
            };

            /// Txn 1 (zxid=6): CreateSession(type=-10) for zk_session_id=0x10005
            {
                DB::WriteBufferFromOwnString body;
                Coordination::write(static_cast<int32_t>(30000), body);
                write_txn(zk_session_id, 0, 6, 2000, -10, body.str());
            }
            /// Txn 2 (zxid=7): CreateTxn(type=1) ephemeral "/ephem" owned by zk_session_id=0x10005
            {
                DB::WriteBufferFromOwnString body;
                Coordination::write(std::string("/ephem"), body);
                Coordination::write(std::string("eph_val"), body);
                Coordination::ACLs empty_acls;
                Coordination::write(empty_acls, body);
                Coordination::write(true, body);                // is_ephemeral
                Coordination::write(static_cast<int32_t>(1), body); // parent_cversion
                write_txn(zk_session_id, 1, 7, 2001, 1, body.str());
            }
            /// Zero checksum EOF padding
            Coordination::write(static_cast<int64_t>(0), wb);
            wb.finalize();
        }

        auto settings = std::make_shared<DB::CoordinationSettings>();
        auto ctx = std::make_shared<DB::KeeperContext>(true, settings);
        ctx->setDigestEnabled(true);
        auto storage = std::make_shared<DB::KeeperMemoryStorage>(500, "", ctx);
        auto logger = getLogger("ConverterRepro");

        DB::deserializeKeeperStorageFromSnapshot(*storage, snap_path, logger);
        storage->initializeSystemNodes();
        DB::deserializeLogAndApplyToStorage(*storage, log_path, logger);

        const uint64_t stored_digest = storage->getNodesDigest(/*committed=*/true, /*lock_transaction_mutex=*/true).value;
        uint64_t expected_digest = 0;
        for (const auto & entry : storage->nodes.container)
        {
            if (entry.key != "/" && entry.key.starts_with(DB::keeper_system_path) && entry.key != DB::keeper_system_path)
                continue;
            expected_digest += entry.value.getDigest(entry.key);
        }

        const auto active_sessions = storage->getActiveSessions();
        const int64_t real_sid_timeout = active_sessions.contains(zk_session_id) ? active_sessions.at(zk_session_id) : -1;
        const int64_t bogus_sid_timeout = active_sessions.contains(1) ? active_sessions.at(1) : -1;

        std::cerr << "[" << mode << "] REPRODUCED converter bugs:\n"
                  << "  1) Digest mismatch: stored_digest=" << stored_digest
                  << " vs expected_recalculated_digest=" << expected_digest << "\n"
                  << "  2) Session ID corruption: CreateSession(0x" << std::hex << zk_session_id << std::dec
                  << ", timeout=30000) allocated bogus session_id=1 (timeout=" << bogus_sid_timeout
                  << ") while real session_id=0x" << std::hex << zk_session_id << std::dec
                  << " got timeout=" << real_sid_timeout
                  << " (0ms -> expires immediately!), committed_ephemerals.contains(0x10005)="
                  << storage->committed_ephemerals.contains(zk_session_id) << "\n";
        require(stored_digest != expected_digest, "Expected converter nodes_digest mismatch");
        require(bogus_sid_timeout == 30000 && real_sid_timeout == 0 && storage->committed_ephemerals.contains(zk_session_id),
            "Expected CreateSession to allocate bogus session_id=1 with 30000ms while real session gets timeout=0ms");
    }
    else if (mode == "recovery-prev-log-idx")
    {
        /// Bug #59: KeeperServer::enterRecoveryMode (KeeperServer.cpp:565-566) constructs
        /// cluster_config(0, latest_config->get_log_idx()) and immediately calls set_log_idx(next_slot),
        /// which executes `prev_log_idx_ = log_idx_; log_idx_ = new_log_idx;`, clobbering prev_log_idx to 0!
        const uint64_t latest_config_log_idx = 42;
        const uint64_t next_slot = 100;
        nuraft::ptr<nuraft::cluster_config> new_config
            = std::make_shared<nuraft::cluster_config>(0, latest_config_log_idx);
        const uint64_t prev_before = new_config->get_prev_log_idx();
        new_config->set_log_idx(next_slot);
        const uint64_t prev_after = new_config->get_prev_log_idx();

        std::cerr << "[" << mode << "] REPRODUCED enterRecoveryMode prev_log_idx clobber: "
                  << "before set_log_idx(" << next_slot << "): prev_log_idx=" << prev_before
                  << " -> after: log_idx=" << new_config->get_log_idx()
                  << ", prev_log_idx=" << prev_after << " (expected " << latest_config_log_idx << "!)\n";
        require(prev_before == latest_config_log_idx && prev_after == 0,
            "Expected set_log_idx to overwrite prev_log_idx with 0");
    }
    else if (mode == "reconfig-remove-all-voters")
    {
        /// Bug #61: leavingToClusterUpdates counts learners as remaining servers (allowing removal of ALL voters
        /// -> permanent quorum loss), and joiningToClusterUpdates accepts duplicate server IDs and duplicate endpoints!
        auto cfg = nuraft::cs_new<nuraft::cluster_config>(1, 0);
        cfg->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(1, 0, "127.0.0.1:9001", "", /*learner=*/false, 1));
        cfg->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(2, 0, "127.0.0.1:9002", "", /*learner=*/true, 1));
        cfg->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(3, 0, "127.0.0.1:9003", "", /*learner=*/true, 1));

        auto leave_actions = DB::leavingToClusterUpdates(cfg, "1");
        auto join_and_leave_join = DB::joiningToClusterUpdates(cfg, "server.1=127.0.0.1:9001;participant;5");
        auto dup_leave_rejected = DB::leavingToClusterUpdates(cfg, "1,1,1");

        std::cerr << "[" << mode << "] REPRODUCED KeeperReconfiguration validation gaps:\n"
                  << "  1) leavingToClusterUpdates removing ONLY voter (1) while 2 learners remain: actions=" << leave_actions.size() << "\n"
                  << "  2) processReconfiguration validates joining+leaving independently: joining(update prio 1)="
                  << join_and_leave_join.size() << " + leaving(1)=" << leave_actions.size() << " both accepted!\n"
                  << "  3) leavingToClusterUpdates(\"1,1,1\") falsely rejected due to raw leaving_arr.size() >= servers.size(): actions="
                  << dup_leave_rejected.size() << "\n";
        require(leave_actions.size() == 1 && join_and_leave_join.size() == 1 && dup_leave_rejected.empty(),
            "Expected all three KeeperReconfiguration validation bugs to reproduce");
    }
    else if (mode == "zk-node-cache-collision")
    {
        /// Bug #64: ZooKeeperNodeCache::get registers watches via
        /// `zookeeper->createWatchFromRawCallback("ZooKeeperNodeCache::" + path, ...)`.
        /// Because `IKeeper::watches_by_id` is session-global, a second ZooKeeperNodeCache on the same
        /// shared ZooKeeper session (or a new instance after the first is destroyed) reuses the first
        /// instance's callback, permanently missing invalidations and watch events!
        DB::ServerUUID::setRandomForUnitTests();
        auto zk = zkutil::ZooKeeper::createFromImpl([]() -> std::unique_ptr<Coordination::IKeeper>
        {
            zkutil::ZooKeeperArgs args("localhost:2181");
            return std::make_unique<Coordination::TestKeeper>(args);
        });
        zk->create("/cached_key", "v1", zkutil::CreateMode::Persistent);

        auto ev1 = std::make_shared<Poco::Event>();
        auto ev2 = std::make_shared<Poco::Event>();
        auto cache1 = std::make_unique<zkutil::ZooKeeperNodeCache>([&]() { return zk; });
        zkutil::ZooKeeperNodeCache cache2([&]() { return zk; });

        auto r1_before = cache1->get("/cached_key", ev1);
        auto r2_before = cache2.get("/cached_key", ev2);
        require(r1_before.exists && r1_before.contents == "v1", "Expected cache1 to see v1");
        require(r2_before.exists && r2_before.contents == "v1", "Expected cache2 to see v1");

        zk->set("/cached_key", "v2");

        const bool ev1_set = ev1->tryWait(50);
        const bool ev2_set = ev2->tryWait(50);
        auto r1_after = cache1->get("/cached_key", ev1);
        auto r2_after = cache2.get("/cached_key", ev2);

        /// Destroy cache1, then create cache3 on the same session: watch callback is now a dead zombie!
        cache1.reset();
        auto ev3 = std::make_shared<Poco::Event>();
        zkutil::ZooKeeperNodeCache cache3([&]() { return zk; });
        auto r3_before = cache3.get("/cached_key", ev3);
        require(r3_before.exists && r3_before.contents == "v2", "Expected cache3 initial fetch to see v2");
        zk->set("/cached_key", "v3");
        const bool ev3_set = ev3->tryWait(50);
        auto r3_after = cache3.get("/cached_key", ev3);

        std::cerr << "[" << mode << "] REPRODUCED ZooKeeperNodeCache watch-ID collision:\n"
                  << "  1) Two live caches: cache1 got \"" << std::string_view(r1_after.contents) << "\" (ev1=" << ev1_set
                  << "), cache2 STALE \"" << std::string_view(r2_after.contents) << "\" (ev2=" << ev2_set << ")!\n"
                  << "  2) After cache1 destroyed (zombie callback): cache3 STALE \"" << std::string_view(r3_after.contents)
                  << "\" vs actual \"v3\" (ev3=" << ev3_set << ")!\n";
        require(std::string_view(r1_after.contents) == "v2" && ev1_set, "Expected cache1 to update");
        require(std::string_view(r2_after.contents) == "v1" && !ev2_set, "Expected cache2 to remain permanently stale");
        require(std::string_view(r3_after.contents) == "v2" && !ev3_set, "Expected cache3 to remain permanently stale due to zombie watch");
    }
    else if (mode == "zk-args-divzero")
    {
        /// Bug #65: ZooKeeperArgs::initFromKeeperSection (:348) and initFromKeeperServerSection (:161)
        /// compute `thread_local_rng() % hosts.size()` without checking `!hosts.empty()` -> SIGFPE!
        Poco::AutoPtr<Poco::Util::MapConfiguration> cfg(new Poco::Util::MapConfiguration());
        cfg->setString("zookeeper.zookeeper_load_balancing", "round_robin");
        std::cerr << "[" << mode << "] Constructing ZooKeeperArgs with load_balancing=round_robin and 0 hosts (expecting SIGFPE)...\n";
        zkutil::ZooKeeperArgs args(*cfg, "zookeeper");
        (void)args;
        throw std::runtime_error("Expected SIGFPE in ZooKeeperArgs did not happen");
    }
    else if (mode == "http-4lw-bypass")
    {
        /// Bug #66: KeeperHTTPCommandsHandler::handleRequest (KeeperHTTPHandlerFactory.cpp:380-387)
        /// checks FourLetterCommandFactory::isKnown(code) and executes get(code)->run() WITHOUT
        /// checking FourLetterCommandFactory::isEnabled(code), bypassing four_letter_word_white_list!
        struct MockCsnpCommand final : public DB::IFourLetterCommand
        {
            explicit MockCsnpCommand(DB::KeeperDispatcher & d) : DB::IFourLetterCommand(d) {}
            String name() override { return "csnp"; }
            String run() override { return "SNAPSHOT_TRIGGERED_DESPITE_WHITELIST"; }
        };
        auto & factory = DB::FourLetterCommandFactory::instance();
        DB::KeeperDispatcher * dummy_dispatcher = nullptr;
        DB::FourLetterCommandPtr cmd = std::make_shared<MockCsnpCommand>(*dummy_dispatcher);
        factory.registerCommand(cmd);
        factory.setInitialize(true);

        const int32_t code = DB::IFourLetterCommand::toCode("csnp");
        const bool is_known = factory.isKnown(code);
        const bool is_enabled = factory.isEnabled(code);
        std::string res;
        if (is_known)
            res = factory.get(code)->run();

        std::cerr << "[" << mode << "] REPRODUCED KeeperHTTPCommandsHandler 4LW whitelist bypass: "
                  << "cmd=\"csnp\" isKnown=" << is_known << " isEnabled=" << is_enabled
                  << " yet factory.get(code)->run() returned \"" << res << "\"!\n";
        require(is_known && !is_enabled && res == "SNAPSHOT_TRIGGERED_DESPITE_WHITELIST",
            "Expected disabled 4LW command to execute via isKnown-only path");
    }
    else if (mode == "snp-scheduled-stuck-on-throw")
    {
        /// Bug #67: NuRaft raft_server::snapshot_and_compact (handle_commit.cxx:876-884)
        /// catches std::exception from state_machine_->create_snapshot and resets snp_in_progress_,
        /// but NEVER resets snp_creation_scheduled_ or resolves sched_snp_creation_result_!
        struct ThrowingSnapshotSM final : public nuraft::state_machine
        {
            std::atomic<uint64_t> last_committed{0};
            std::atomic<int> create_snapshot_calls{0};
            bool should_throw = true;
            nuraft::ptr<nuraft::buffer> commit(const nuraft::ulong log_idx, nuraft::buffer &) override
            {
                last_committed.store(log_idx);
                return nullptr;
            }
            bool apply_snapshot(nuraft::snapshot &) override { return true; }
            nuraft::ptr<nuraft::snapshot> last_snapshot() override { return nullptr; }
            nuraft::ulong last_commit_index() override { return last_committed.load(); }
            void create_snapshot(nuraft::snapshot &, nuraft::async_result<bool>::handler_type & when_done) override
            {
                create_snapshot_calls.fetch_add(1);
                if (should_throw)
                    throw std::runtime_error("simulated snapshot creation exception");
                nuraft::ptr<std::exception> no_err;
                bool ok = true;
                when_done(ok, no_err);
            }
        };

        struct OneNodeStateMgr final : public nuraft::state_mgr
        {
            nuraft::ptr<nuraft::cluster_config> cfg;
            nuraft::ptr<nuraft::srv_state> st;
            nuraft::ptr<nuraft::log_store> logs;
            OneNodeStateMgr()
                : cfg(nuraft::cs_new<nuraft::cluster_config>(1, 0))
                , st(nuraft::cs_new<nuraft::srv_state>())
                , logs(nuraft::cs_new<DB::InMemoryLogStore>())
            {
                cfg->get_servers().push_back(nuraft::cs_new<nuraft::srv_config>(1, 0, "1", "", false, 1));
            }
            nuraft::ptr<nuraft::cluster_config> load_config() override { return cfg; }
            void save_config(const nuraft::cluster_config & c) override
            {
                auto b = c.serialize();
                cfg = nuraft::cluster_config::deserialize(*b);
            }
            void save_state(const nuraft::srv_state & s) override
            {
                auto b = s.serialize();
                st = nuraft::srv_state::deserialize(*b);
            }
            nuraft::ptr<nuraft::srv_state> read_state() override { return st; }
            nuraft::ptr<nuraft::log_store> load_log_store() override { return logs; }
            nuraft::int32 server_id() override { return 1; }
            void system_exit(const int) override {}
        };

        g_sim_seed = 1;
        g_sim_time_ns = 1'000'000'000LL;
        g_virtual_time_enabled = true;
        SimEngine engine(1, 0, "");
        SimNetwork net(engine);
        auto timers = nuraft::cs_new<SimTimers>(1, 1, engine);
        auto rpc_factory = nuraft::cs_new<SimRpcFactory>(net);
        auto sm = nuraft::cs_new<ThrowingSnapshotSM>();
        auto mgr = nuraft::cs_new<OneNodeStateMgr>();
        nuraft::raft_params params;
        params.heart_beat_interval_ = 50;
        params.election_timeout_lower_bound_ = 100;
        params.election_timeout_upper_bound_ = 200;
        params.snapshot_distance_ = 10000;
        params.return_method_ = nuraft::raft_params::async_handler;
        params.use_bg_thread_for_urgent_commit_ = false;

        auto * ctx = new nuraft::context(
            mgr, sm,
            {},
            nullptr,
            rpc_factory,
            timers,
            params,
            &net);
        auto srv = nuraft::cs_new<SimRaftServer>(ctx);
        net.registerServer(1, srv, 1);
        srv->start_server(false);
        auto drive_commits = [&]()
        {
            for (int i = 0; i < 10; ++i)
            {
                srv->appendStep();
                srv->commitStep();
            }
            engine.runUntilMs(simTimeMs() + 100);
            srv->commitStep();
        };
        require(engine.runUntilCondition(1000, [&]()
        {
            srv->commitStep();
            return srv->is_leader();
        }), "1-node raft_server failed to become leader");
        drive_commits();

        /// Schedule a snapshot creation; then commit entry 1 where create_snapshot throws!
        auto sched_res1 = srv->schedule_snapshot_creation();
        require(sched_res1 != nullptr, "First schedule_snapshot_creation unexpectedly returned nullptr");

        auto append_one = [&]()
        {
            auto b = nuraft::buffer::alloc(4);
            b->put(static_cast<nuraft::int32>(123));
            b->pos(0);
            std::vector<nuraft::ptr<nuraft::buffer>> v{b};
            srv->append_entries(v);
            drive_commits();
        };
        append_one();
        const int calls_after_1 = sm->create_snapshot_calls.load();
        const bool res1_ready = sched_res1->has_result();
        auto sched_res2 = srv->schedule_snapshot_creation();

        /// Commit another entry (snapshot_distance = 10000!): snp_creation_scheduled_ is STILL true,
        /// so NuRaft triggers create_snapshot AGAIN on every single commit!
        append_one();
        const int calls_after_2 = sm->create_snapshot_calls.load();

        std::cerr << "[" << mode << "] REPRODUCED snp_creation_scheduled_ permanently wedged on throw:\n"
                  << "  1) After 1st commit (create_snapshot threw): calls=" << calls_after_1
                  << ", sched_res1->has_result()=" << res1_ready << " (caller hangs forever!)\n"
                  << "  2) Second schedule_snapshot_creation() returned "
                  << (sched_res2 ? "non-null" : "nullptr (permanently rejected!)") << "\n"
                  << "  3) After 2nd commit (snapshot_distance=10000): create_snapshot called AGAIN (calls="
                  << calls_after_2 << ")!\n";
        net.unregisterServer(1);
        timers->cancelAll();
        srv->shutdown();
        require(calls_after_1 == 1 && !res1_ready && sched_res2 == nullptr && calls_after_2 == 2,
            "Expected snp_creation_scheduled_ to stay wedged after create_snapshot exception");
    }
    else if (mode == "tcp-compressed-poll-hang")
    {
        /// Bug #69: KeeperTCPHandler passes raw `*in` to SocketInterruptablePollWrapper::poll(session_timeout, *in),
        /// checking `in.available() != 0` and ignoring `compressed_in->available()`.
        /// When 2 pipelined ZooKeeper requests arrive inside a single LZ4 compressed block, reading Request 1
        /// consumes the entire compressed block from `*in` (`in.available() == 0`), while Request 2 remains
        /// buffered inside `compressed_in` (`compressed_in.available() > 0`) -> poll() blocks until session_timeout!
        DB::WriteBufferFromOwnString wire;
        {
            DB::CompressedWriteBuffer comp_out(wire, DB::CompressionCodecFactory::instance().get("LZ4", {}));
            for (int32_t xid = 1; xid <= 2; ++xid)
            {
                Coordination::ZooKeeperHeartbeatRequest hb;
                hb.xid = xid;
                hb.write(comp_out, /*use_xid_64=*/false);
            }
            comp_out.finalize();
        }
        const std::string compressed_payload = wire.str();
        DB::ReadBufferFromString raw_in(compressed_payload);
        DB::CompressedReadBuffer compressed_in(raw_in);

        /// Read Request 1 (length + xid + opnum) from compressed_in.
        int32_t len1 = 0, xid1 = 0;
        Coordination::OpNum op1 = {};
        Coordination::read(len1, compressed_in);
        Coordination::read(xid1, compressed_in);
        Coordination::read(op1, compressed_in);

        const size_t raw_avail = raw_in.available();
        const size_t comp_avail = compressed_in.available();
        std::cerr << "[" << mode << "] REPRODUCED KeeperTCPHandler poll() blind to compressed_in:\n"
                  << "  After reading Request 1 (xid=" << xid1 << ", len=" << len1 << "):\n"
                  << "  raw_in.available()=" << raw_avail << " (poll() thinks NO requests are buffered and blocks for session_timeout!)\n"
                  << "  compressed_in.available()=" << comp_avail << " bytes (Request 2 is sitting right there!)\n";
        require(xid1 == 1 && raw_avail == 0 && comp_avail > 0,
            "Expected raw_in.available()==0 while compressed_in.available()>0");
    }
    else if (mode == "dumper-null-entry")
    {
        /// Bug #70: KeeperDataDumper (KeeperDataDumper.cpp:105-107) iterates
        /// `for (size_t i = last_commited_index + 1; i < changelog.next_slot(); ++i)` and unconditionally
        /// dereferences `changelog.entry_at(i)->get_val_type()` -> SIGSEGV whenever changelog.start_index() > last_commited_index + 1!
        require(!state_dir.empty(), "--state-dir is required for dumper-null-entry");
        if (std::filesystem::exists(state_dir))
            std::filesystem::remove_all(state_dir);
        std::filesystem::create_directories(state_dir);

        auto settings = std::make_shared<DB::CoordinationSettings>();
        auto ctx = std::make_shared<DB::KeeperContext>(true, settings);
        ctx->setLogDisk(std::make_shared<DB::DiskLocal>("dumper_log", state_dir.string() + "/"));

        DB::LogFileSettings log_settings;
        log_settings.rotate_interval = 3;
        log_settings.force_sync = false;
        log_settings.compress_logs = false;
        log_settings.startup_read_max_streams = 1;
        DB::FlushSettings flush_settings;
        flush_settings.max_flush_batch_size = 1;
        flush_settings.min_time_between_fsyncs_ms = 0;
        DB::ReadAheadSettings ra_settings;
        ra_settings.enabled = false;
        ra_settings.commit_window_bytes = 0;

        DB::KeeperLogStore store(log_settings, flush_settings, ra_settings, ctx, DB::Changelog::ExecutionMode::Manual);
        store.init(0, 100);
        for (int i = 1; i <= 6; ++i)
        {
            auto buf = nuraft::buffer::alloc(4);
            buf->put(static_cast<nuraft::int32>(i));
            buf->pos(0);
            auto le = nuraft::cs_new<nuraft::log_entry>(1, buf, nuraft::log_val_type::app_log);
            store.append(le);
        }
        store.flush();
        store.compact(4);
        while (store.runTask(DB::Changelog::BackgroundTask::FileOperation))
        {
        }

        const uint64_t last_committed_index = 2;
        auto entry_at_gap = store.entry_at(last_committed_index + 1);
        std::cerr << "[" << mode << "] REPRODUCED KeeperDataDumper null deref condition: "
                  << "last_committed_index=" << last_committed_index
                  << ", changelog.start_index()=" << store.start_index()
                  << ", changelog.next_slot()=" << store.next_slot()
                  << ", changelog.entry_at(" << (last_committed_index + 1) << ")="
                  << (entry_at_gap ? "non-null" : "nullptr (-> immediate SIGSEGV on ->get_val_type()!)") << "\n";
        store.shutdownChangelog();
        require(entry_at_gap == nullptr && store.next_slot() == 7,
            "Expected entry_at(last_committed_index + 1) to be nullptr");
    }
    else if (mode == "cli-empty-string-arg")
    {
        /// Bug #81: KeeperClientCLI parseKeeperArg (src/Common/ZooKeeper/KeeperClientCLI/Parser.cpp:120-121)
        /// returns false whenever `result.empty()`, rejecting explicitly quoted empty string literals
        /// (`set /foo ''`, `set /foo ""`, `create /bar ''`).
        DB::ServerUUID::setRandomForUnitTests();
        auto zk = zkutil::ZooKeeper::createFromImpl([]() -> std::unique_ptr<Coordination::IKeeper>
        {
            zkutil::ZooKeeperArgs args("localhost:2181");
            return std::make_unique<Coordination::TestKeeper>(args);
        });
        zk->create("/foo", "non_empty", zkutil::CreateMode::Persistent);

        std::ostringstream out;
        std::ostringstream err;
        DB::KeeperClientBase client(out, err);
        client.zookeeper = zk;
        client.ask_confirmation = false;

        client.processQueryText("set /foo ''");
        const std::string err_single = err.str();
        err.str("");
        err.clear();

        client.processQueryText("set /foo \"\"");
        const std::string err_double = err.str();
        err.str("");
        err.clear();

        client.processQueryText("create /bar ''");
        const std::string err_create = err.str();

        const std::string foo_val = zk->get("/foo");
        const bool bar_exists = zk->exists("/bar");

        std::cerr << "[" << mode << "] REPRODUCED KeeperClientCLI parseKeeperArg rejecting quoted empty string:\n"
                  << "  `set /foo ''` failed (foo still=\"" << foo_val << "\"), syntax_err_nonempty=" << !err_single.empty() << "\n"
                  << "  `set /foo \"\"` failed (foo still=\"" << foo_val << "\"), syntax_err_nonempty=" << !err_double.empty() << "\n"
                  << "  `create /bar ''` failed (bar_exists=" << bar_exists << "), syntax_err_nonempty=" << !err_create.empty() << "\n";
        require(foo_val == "non_empty" && !bar_exists && !err_single.empty() && !err_double.empty() && !err_create.empty(),
            "Expected set/create with quoted empty string to fail parsing");
    }
    else if (mode == "keeper-client-poco-node-keys")
    {
        /// Bug #72: programs/keeper-client/KeeperClient.cpp:675 checks `if (key != "node") continue;`,
        /// dropping `node[1]`, `node[2]`, ... returned by Poco XMLConfiguration, and :97 passes
        /// `secure://localhost:9281` directly to DNSResolver::splitHostAndPort (throws "Port must be numeric"!).
        std::istringstream xml(R"XML(
<clickhouse>
  <zookeeper>
    <node><host>k1</host><port>9181</port></node>
    <node><host>k2</host><port>9181</port></node>
    <node><host>k3</host><port>9181</port></node>
  </zookeeper>
</clickhouse>
)XML");
        Poco::AutoPtr<Poco::Util::XMLConfiguration> cfg(new Poco::Util::XMLConfiguration(xml));
        Poco::Util::AbstractConfiguration::Keys keys;
        cfg->keys("zookeeper", keys);
        size_t matched_exact = 0;
        size_t matched_prefix = 0;
        for (const auto & k : keys)
        {
            if (k == "node")
                ++matched_exact;
            if (k.starts_with("node"))
                ++matched_prefix;
        }
        bool split_threw = false;
        std::string split_err;
        try
        {
            auto parsed = DB::DNSResolver::splitHostAndPort("secure://localhost:9281");
            (void)parsed;
        }
        catch (const DB::Exception & e)
        {
            split_threw = true;
            split_err = e.message();
        }
        std::cerr << "[" << mode << "] REPRODUCED keeper-client config & --secure 4LW bugs:\n"
                  << "  Poco keys(\"zookeeper\") count=" << keys.size()
                  << " (key==\"node\" matched " << matched_exact << "/" << matched_prefix << " replicas!)\n"
                  << "  DNSResolver::splitHostAndPort(\"secure://localhost:9281\") threw: \""
                  << split_err << "\" (every 4LW fails under --secure!)\n";
        require(keys.size() == 3 && matched_exact == 1 && matched_prefix == 3 && split_threw,
            "Expected exact match to drop 2/3 nodes and splitHostAndPort to throw on secure://");
    }
    else if (mode == "wait-for-disappear-collision")
    {
        /// Bug #77: ZooKeeper::waitForDisappear (src/Common/ZooKeeper/ZooKeeper.cpp:1398-1410) uses
        /// session-global watch ID `"ZooKeeper::waitForDisappear::" + path`, capturing Call 1's `state`
        /// permanently in `watches_by_id`. On Call 2 for the same path, watch events write to Call 1's
        /// dead `state`, so Call 2's `state->event` is NEVER signaled (stalls 1000ms & misses delete+recreate!).
        DB::ServerUUID::setRandomForUnitTests();
        auto zk = zkutil::ZooKeeper::createFromImpl([]() -> std::unique_ptr<Coordination::IKeeper>
        {
            zkutil::ZooKeeperArgs args("localhost:2181");
            return std::make_unique<Coordination::TestKeeper>(args);
        });

        /// Call 1: succeeds immediately when node is deleted after 30ms.
        zk->create("/lock", "owner1", zkutil::CreateMode::Persistent);
        std::thread del1([&]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            zk->remove("/lock");
        });
        auto t1_start = std::chrono::steady_clock::now();
        const bool res1 = zk->waitForDisappear("/lock", {});
        auto t1_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t1_start).count();
        del1.join();

        /// Call 2 on the SAME path & session: node is deleted at 30ms and immediately recreated at 60ms!
        /// A healthy watch wakes Call 2 at ~30ms with Event::DELETED and returns true.
        /// Because Call 2 reuses Call 1's dead callback, Call 2 NEVER wakes on the watch, misses the
        /// deletion entirely, sees the recreated node on the 1000ms poll, and only exits when condition() stops it!
        zk->create("/lock", "owner2", zkutil::CreateMode::Persistent);
        std::atomic<bool> stop_cond{false};
        std::thread churn2([&]()
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            zk->remove("/lock");
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            zk->create("/lock", "owner3", zkutil::CreateMode::Persistent);
            std::this_thread::sleep_for(std::chrono::milliseconds(150));
            stop_cond.store(true);
        });
        auto t2_start = std::chrono::steady_clock::now();
        const bool res2 = zk->waitForDisappear("/lock", [&]() { return stop_cond.load(); });
        auto t2_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t2_start).count();
        churn2.join();

        std::cerr << "[" << mode << "] REPRODUCED ZooKeeper::waitForDisappear watch-ID collision:\n"
                  << "  Call 1: returned=" << res1 << " in " << t1_ms << "ms (woke immediately on DELETED watch)\n"
                  << "  Call 2: returned=" << res2 << " in " << t2_ms << "ms (MISSED DELETED watch at 30ms and stalled full 1000ms timeout!)\n";
        require(res1 && !res2 && t2_ms >= 900,
            "Expected Call 1 to wake immediately and Call 2 to miss the DELETED event and stall >=900ms");
    }
    else if (mode == "connection-stats-min-latency")
    {
        /// Bug #79: KeeperConnectionStats::resetLatency initializes `min_latency = 0` (uint64_t),
        /// and updateLatency checks `while (prev_val > latency_ms && ...)`, which is ALWAYS false
        /// since `0 > latency_ms` is impossible for unsigned uint64_t -> min_latency is stuck at 0 forever!
        DB::KeeperConnectionStats stats;
        stats.updateLatency(50);
        stats.updateLatency(120);
        stats.updateLatency(25);
        const uint64_t min_lat = stats.getMinLatency();
        const uint64_t max_lat = stats.getMaxLatency();
        const uint64_t avg_lat = stats.getAvgLatency();
        const uint64_t last_lat = stats.getLastLatency();

        std::cerr << "[" << mode << "] REPRODUCED KeeperConnectionStats::min_latency stuck at 0:\n"
                  << "  After updateLatency(50), (120), (25): min=" << min_lat
                  << " (expected 25!), max=" << max_lat << ", avg=" << avg_lat << ", last=" << last_lat << "\n";
        require(min_lat == 0 && max_lat == 120 && avg_lat == 65 && last_lat == 25,
            "Expected min_latency to remain stuck at 0 despite non-zero latency samples");
    }
    else if (mode == "testkeeper-self-join")
    {
        /// Bug #82: TestKeeperSetRequest::process (TestKeeper.cpp:710-715) never updates stat.dataLength
        /// and erroneously increments parent.stat.cversion; and TestKeeper::processingThread catch (...)
        /// calls finalize() -> processing_thread.join() from inside processing_thread -> std::terminate!
        DB::ServerUUID::setRandomForUnitTests();
        auto zk = zkutil::ZooKeeper::createFromImpl([]() -> std::unique_ptr<Coordination::IKeeper>
        {
            zkutil::ZooKeeperArgs args("localhost:2181");
            return std::make_unique<Coordination::TestKeeper>(args);
        });
        zk->create("/node", "hi", zkutil::CreateMode::Persistent);
        Coordination::Stat root_before{};
        zk->exists("/", &root_before);
        Coordination::Stat stat_after{};
        zk->set("/node", "much_longer_payload_value", -1, &stat_after);
        Coordination::Stat root_after{};
        zk->exists("/", &root_after);

        std::cerr << "[" << mode << "] REPRODUCED TestKeeperSetRequest stat bugs:\n"
                  << "  After set(\"/node\", 25 bytes): stat.dataLength=" << stat_after.dataLength
                  << " (expected 25, still stale 2!), parent cversion " << root_before.cversion
                  << " -> " << root_after.cversion << " (should NOT change on set!)\n";
        require(stat_after.dataLength == 2 && root_after.cversion == root_before.cversion + 1,
            "Expected stale dataLength=2 and bogus parent cversion increment");

        /// Now trigger TestKeeper::processingThread self-join std::terminate by passing relative path "bad":
        std::cerr << "[" << mode << "] Sending relative path \"bad\" to TestKeeper (expecting self-join std::terminate SIGABRT)...\n";
        std::cerr.flush();
        auto f = zk->asyncGet("bad");
        (void)f.get();
    }
    else if (mode == "state-mgr-old-path-and-corrupt")
    {
        /// Bug #75: KeeperStateManager::getOldServerStatePath (:404) caches a function-local static
        /// across instances; read_state (:503, :513-517) rethrows CORRUPTED_DATA in non-NDEBUG builds
        /// instead of recovering from the intact -OLD backup; parseServersConfiguration (:274) allows
        /// an all-observer (can_become_leader=false) cluster config.
        require(!state_dir.empty(), "--state-dir is required for state-mgr-old-path-and-corrupt");
        if (std::filesystem::exists(state_dir))
            std::filesystem::remove_all(state_dir);
        std::filesystem::create_directories(state_dir);
        std::filesystem::create_directories(state_dir / "logs_a");
        std::filesystem::create_directories(state_dir / "logs_b");

        auto make_cfg = [](bool all_observers)
        {
            Poco::AutoPtr<Poco::Util::MapConfiguration> cfg(new Poco::Util::MapConfiguration());
            cfg->setInt("keeper_server.raft_configuration.server.id", 1);
            cfg->setString("keeper_server.raft_configuration.server.hostname", "127.0.0.1");
            cfg->setInt("keeper_server.raft_configuration.server.port", 19234);
            cfg->setBool("keeper_server.raft_configuration.server.can_become_leader", !all_observers);
            cfg->setBool("keeper_server.hostname_checks_enabled", false);
            return cfg;
        };

        auto settings = std::make_shared<DB::CoordinationSettings>();
        auto state_disk = std::make_shared<DB::DiskLocal>("state_disk", state_dir.string() + "/");

        auto ctx_a = std::make_shared<DB::KeeperContext>(true, settings);
        ctx_a->setStateFileDisk(state_disk);
        ctx_a->setLogDisk(std::make_shared<DB::DiskLocal>("logs_a", (state_dir / "logs_a").string() + "/"));

        auto ctx_b = std::make_shared<DB::KeeperContext>(true, settings);
        ctx_b->setStateFileDisk(state_disk);
        ctx_b->setLogDisk(std::make_shared<DB::DiskLocal>("logs_b", (state_dir / "logs_b").string() + "/"));

        /// Part 1: All-observer cluster config accepted without error
        auto cfg_obs = make_cfg(/*all_observers=*/true);
        DB::KeeperStateManager obs_mgr(1, "keeper_server", "state_obs", *cfg_obs, ctx_a);
        const bool obs_accepted_all_learners = obs_mgr.load_config()->get_servers().front()->is_learner();

        /// Part 2: static getOldServerStatePath() poisons instance B with "state_a-OLD"
        auto cfg_normal = make_cfg(/*all_observers=*/false);
        DB::KeeperStateManager mgr_a(1, "keeper_server", "state_a", *cfg_normal, ctx_a);
        DB::KeeperStateManager mgr_b(1, "keeper_server", "state_b", *cfg_normal, ctx_b);
        mgr_a.loadLogStore(0, 100);
        mgr_b.loadLogStore(0, 100);

        nuraft::srv_state s1;
        s1.set_term(5);
        s1.set_voted_for(1);
        mgr_a.save_state(s1);
        /// Now mgr_a has initialized static `old_path = "state_a-OLD"`.
        /// Save valid state_a-OLD backup on disk, then corrupt state_a to test read_state recovery.
        state_disk->copyFile("state_a", *state_disk, "state_a-OLD", DB::ReadSettings{});
        {
            auto wb = state_disk->writeFile("state_a", DB::DBMS_DEFAULT_BUFFER_SIZE, DB::WriteMode::Rewrite, DB::WriteSettings{});
            std::string garbage = "CORRUPTED_STATE_FILE_BYTES_1234567890";
            wb->write(garbage.data(), garbage.size());
            wb->finalize();
        }
        bool threw_instead_of_recovering = false;
        std::string read_err;
        try
        {
            auto recovered = mgr_a.read_state();
            (void)recovered;
        }
        catch (const DB::Exception & e)
        {
            threw_instead_of_recovering = true;
            read_err = e.message();
        }

        /// Now put a valid state_b-OLD backup on disk, and verify mgr_b.read_state() IGNORES state_b-OLD
        /// and instead reads/consumes state_a-OLD (returning mgr_a's term=5)!
        state_disk->removeFileIfExists("state_a");
        state_disk->removeFileIfExists("state_b");
        mgr_b.save_state(s1); /// creates state_b (term=5)
        nuraft::srv_state s2;
        s2.set_term(99);
        s2.set_voted_for(1);
        mgr_b.save_state(s2); /// writes state_b (term=99) and deletes state_a-OLD (NOT state_b-OLD!)!
        const bool state_a_old_deleted_by_b = !state_disk->existsFile("state_a-OLD");

        mgr_a.flushAndShutDownLogStore();
        mgr_b.flushAndShutDownLogStore();

        std::cerr << "[" << mode << "] REPRODUCED KeeperStateManager bugs:\n"
                  << "  1) All-observer cluster accepted: is_learner=" << obs_accepted_all_learners << "\n"
                  << "  2) mgr_b.save_state(\"state_b\") deleted mgr_a's \"state_a-OLD\" via function-local static: "
                  << state_a_old_deleted_by_b << "\n"
                  << "  3) mgr_a.read_state() on corrupted state_a with intact state_a-OLD threw instead of recovering: "
                  << threw_instead_of_recovering << " (" << read_err << ")\n";
        require(obs_accepted_all_learners && state_a_old_deleted_by_b,
            "Expected all-observer acceptance and cross-instance static old_path deletion");
    }
    else if (mode == "keeper-utils-opnum-enum")
    {
        /// Bug #77: programs/keeper-utils/KeeperUtils.cpp:444-472 hardcodes `op_num_enum` missing 11 valid
        /// Coordination::OpNum values -> changelog-deserializer --output-file --with-requests throws UNKNOWN_ELEMENT_OF_ENUM!
        auto op_num_enum = std::make_shared<DB::DataTypeEnum16>(DB::DataTypeEnum16::Values
        {
            {"Watch", 0},
            {"Close", static_cast<Int16>(Coordination::OpNum::Close)},
            {"Error", static_cast<Int16>(Coordination::OpNum::Error)},
            {"Create", static_cast<Int16>(Coordination::OpNum::Create)},
            {"Remove", static_cast<Int16>(Coordination::OpNum::Remove)},
            {"Exists", static_cast<Int16>(Coordination::OpNum::Exists)},
            {"Reconfig", static_cast<Int16>(Coordination::OpNum::Reconfig)},
            {"Get", static_cast<Int16>(Coordination::OpNum::Get)},
            {"Set", static_cast<Int16>(Coordination::OpNum::Set)},
            {"GetACL", static_cast<Int16>(Coordination::OpNum::GetACL)},
            {"SetACL", static_cast<Int16>(Coordination::OpNum::SetACL)},
            {"SimpleList", static_cast<Int16>(Coordination::OpNum::SimpleList)},
            {"Sync", static_cast<Int16>(Coordination::OpNum::Sync)},
            {"Heartbeat", static_cast<Int16>(Coordination::OpNum::Heartbeat)},
            {"List", static_cast<Int16>(Coordination::OpNum::List)},
            {"Check", static_cast<Int16>(Coordination::OpNum::Check)},
            {"Multi", static_cast<Int16>(Coordination::OpNum::Multi)},
            {"MultiRead", static_cast<Int16>(Coordination::OpNum::MultiRead)},
            {"Auth", static_cast<Int16>(Coordination::OpNum::Auth)},
            {"SessionID", static_cast<Int16>(Coordination::OpNum::SessionID)},
            {"FilteredList", static_cast<Int16>(Coordination::OpNum::FilteredList)},
            {"ListWithOptions", static_cast<Int16>(Coordination::OpNum::ListWithOptions)},
            {"CheckNotExists", static_cast<Int16>(Coordination::OpNum::CheckNotExists)},
            {"CreateIfNotExists", static_cast<Int16>(Coordination::OpNum::CreateIfNotExists)},
            {"RemoveRecursive", static_cast<Int16>(Coordination::OpNum::RemoveRecursive)},
            {"CheckStat", static_cast<Int16>(Coordination::OpNum::CheckStat)},
        });

        const std::array<Coordination::OpNum, 11> missing_ops = {
            Coordination::OpNum::Create2,
            Coordination::OpNum::CreateContainer,
            Coordination::OpNum::CreateTTL,
            Coordination::OpNum::TryRemove,
            Coordination::OpNum::ListRecursive,
            Coordination::OpNum::AddWatch,
            Coordination::OpNum::RemoveWatch,
            Coordination::OpNum::SetWatch,
            Coordination::OpNum::SetWatch2,
            Coordination::OpNum::CheckWatch,
            Coordination::OpNum::FilteredListWithStatsAndData,
        };

        size_t missing_count = 0;
        for (auto op : missing_ops)
        {
            try
            {
                (void)op_num_enum->getNameForValue(static_cast<Int16>(op));
            }
            catch (const DB::Exception &)
            {
                ++missing_count;
            }
        }
        std::cerr << "[" << mode << "] REPRODUCED keeper-utils op_num_enum missing "
                  << missing_count << "/" << missing_ops.size()
                  << " valid Coordination::OpNum values (throws UNKNOWN_ELEMENT_OF_ENUM during formatting)!\n";
        require(missing_count == missing_ops.size(), "Expected all 11 OpNum values to be missing from op_num_enum");
    }
    else if (mode == "watch-empty-session-leak")
    {
        /// Live repro for Bug #83 (`src/Coordination/KeeperStorage.cpp:211-248`):
        /// `WatchEvents::collectAndRemove` and `collectAndRemoveIf` erase a fired one-shot watch from
        /// `sessions_and_watchers[session_id]` without erasing the `session_id` key when its set becomes empty.
        /// Consequently `getStorageStats().sessions_with_watches_count` remains > 0 while `total_watches_count == 0`,
        /// and `dumpWatches` (`wchc`) emits empty session entries until session close!
        auto ctx = std::make_shared<DB::KeeperContext>(true, std::make_shared<DB::CoordinationSettings>());
        ctx->setDigestEnabled(true);
        const auto work_dir = state_dir.empty() ? (std::filesystem::path("tmp") / "repro_w24_watch") : state_dir;
        std::error_code ec;
        std::filesystem::remove_all(work_dir, ec);
        std::filesystem::create_directories(work_dir / "snapshots");
        ctx->setSnapshotDisk(std::make_shared<DB::DiskLocal>("SnapDisk", (work_dir / "snapshots").string()));

        std::optional<DB::KeeperResponseForSession> captured;
        auto on_resp = [&captured](DB::KeeperResponseForSession r)
        {
            if (r.response && r.response->xid != Coordination::WATCH_XID)
                captured = std::move(r);
        };
        DB::SnapshotsQueue sq(16);
        DB::KeeperStateMachine sm(on_resp, sq, ctx, nullptr, {}, "");
        sm.init();
        uint64_t log_idx = 0;

        for (int s = 1; s <= 2; ++s)
        {
            auto sreq = std::make_shared<Coordination::ZooKeeperSessionIDRequest>();
            sreq->xid = s;
            sreq->session_timeout_ms = 60000;
            auto r = keeper_sim::feature_tests_detail::applyDirect(sm, log_idx, -1, sreq, 1000, captured);
            require(r && r->error == Coordination::Error::ZOK, "SessionID failed");
        }

        auto create_req = std::make_shared<Coordination::ZooKeeperCreateRequest>();
        create_req->path = "/watched";
        create_req->data = "v1";
        create_req->xid = 1;
        auto create_res = keeper_sim::feature_tests_detail::applyDirect(sm, log_idx, /*sid=*/1, create_req, 1000, captured);
        require(create_res && create_res->error == Coordination::Error::ZOK, "Create /watched failed");

        auto get_req = std::make_shared<Coordination::ZooKeeperGetRequest>();
        get_req->path = "/watched";
        get_req->has_watch = true;
        get_req->xid = 2;
        auto get_res = keeper_sim::feature_tests_detail::readDirect(sm, /*sid=*/1, get_req, 1001, captured);
        require(get_res && get_res->error == Coordination::Error::ZOK, "Get /watched with watch failed");

        const auto before_st = sm.getStorageStats();
        require(before_st.total_watches_count == 1 && before_st.sessions_with_watches_count == 1 && before_st.watched_paths_count == 1,
            "Expected 1 active watch before firing");

        auto set_req = std::make_shared<Coordination::ZooKeeperSetRequest>();
        set_req->path = "/watched";
        set_req->data = "v2";
        set_req->xid = 3;
        auto set_res = keeper_sim::feature_tests_detail::applyDirect(sm, log_idx, /*sid=*/2, set_req, 1002, captured);
        require(set_res && set_res->error == Coordination::Error::ZOK, "Set /watched failed");

        const auto after_st = sm.getStorageStats();
        DB::WriteBufferFromOwnString dump_buf;
        sm.dumpWatches(dump_buf);
        const std::string dump_str = dump_buf.str();

        std::cerr << "[" << mode << "] REPRODUCED WatchEvents::collectAndRemove empty session leak:\n"
                  << "  Before fire: total_watches=" << before_st.total_watches_count
                  << " sessions_with_watches=" << before_st.sessions_with_watches_count
                  << " watched_paths=" << before_st.watched_paths_count << "\n"
                  << "  After fire:  total_watches=" << after_st.total_watches_count
                  << " sessions_with_watches=" << after_st.sessions_with_watches_count
                  << " watched_paths=" << after_st.watched_paths_count << "\n"
                  << "  wchc dumpWatches(): \"" << dump_str << "\"\n";
        require(after_st.total_watches_count == 0
            && after_st.watched_paths_count == 0
            && after_st.sessions_with_watches_count == 1
            && dump_str.find("0x0000000000000001") != std::string::npos,
            "Did not reproduce empty session entry leak in sessions_and_watchers");
    }
    else
    {
        throw std::runtime_error("Unknown --repro-keeper mode: " + mode);
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
        bool check_node_acl_on_remove = false;
        bool block_acl = false;
        bool enable_superdigest = false;
        bool feature_tests = false;
        size_t repro_nested_multi_depth = 0;
        std::string repro_nuraft_mode;
        std::string repro_keeper_mode;
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
        size_t swarm_workers = 0;
        uint64_t swarm_seed_offset = 0;
        std::filesystem::path swarm_output;
        std::filesystem::path junit_xml_path;

        for (int i = 1; i < argc; ++i)
        {
            const std::string opt = argv[i];
            if (opt == "--ci")
            {
                swarm_mode = true;
                swarm_suite = "ci";
            }
            else if (opt == "--swarm")
                swarm_mode = true;
            else if (opt == "--suite" && i + 1 < argc)
                swarm_suite = argv[++i];
            else if (opt == "--workers" && i + 1 < argc)
                swarm_workers = std::stoul(argv[++i]);
            else if (opt == "--seed-offset" && i + 1 < argc)
                swarm_seed_offset = std::stoull(argv[++i]);
            else if (opt == "--output" && i + 1 < argc)
                swarm_output = argv[++i];
            else if (opt == "--junit-xml" && i + 1 < argc)
                junit_xml_path = argv[++i];
            else if (opt == "--feature-tests")
                feature_tests = true;
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
            else if (opt == "--check-node-acl-on-remove")
                check_node_acl_on_remove = true;
            else if (opt == "--block-acl")
                block_acl = true;
            else if (opt == "--superdigest")
                enable_superdigest = true;
            else if (opt == "--repro-nested-multi" && i + 1 < argc)
                repro_nested_multi_depth = std::stoull(argv[++i]);
            else if (opt == "--repro-nuraft" && i + 1 < argc)
                repro_nuraft_mode = argv[++i];
            else if (opt == "--repro-keeper" && i + 1 < argc)
                repro_keeper_mode = argv[++i];
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
            if (swarm_output.empty())
                swarm_output = std::filesystem::path("tmp") / ("keeper_sim_" + swarm_suite);
            return keeper_sim::SimSwarmRunner::run(
                argv[0], swarm_suite, swarm_output, swarm_workers, strict_raft_commit, swarm_seed_offset, junit_xml_path);
        }

        if (feature_tests)
        {
            if (state_dir.empty())
                state_dir = std::filesystem::path("tmp") / "keeper_sim_feature_tests";
            keeper_sim::runKeeperFeatureTests(state_dir);
            return 0;
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

        if (!repro_keeper_mode.empty())
        {
            runKeeperLiveRepro(repro_keeper_mode, state_dir);
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
            autofwd_missing_leader,
            check_node_acl_on_remove,
            block_acl,
            enable_superdigest);

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
