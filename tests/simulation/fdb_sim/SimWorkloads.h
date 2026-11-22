#pragma once

#include <SimCluster.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fdb_sim
{

/// FoundationDB-inspired Concurrent Multi-Session ZooKeeper Workload Actor.
///
/// Runs multiple concurrent client sessions issuing:
///   - Persistent, Ephemeral, and Sequential `Create`
///   - Versioned CAS and unconditional `Set`
///   - Versioned and unconditional `Remove`
///   - `Check`
///   - Atomic `Multi` transactions (both committing and rolling back mid-transaction!)
///   - Local `Get` / `List` / `Exists` reads with real `Watches`!
///   - Session heartbeats, explicit closes, AND deliberate session expiration!
///   - Handles leader failovers & partitions gracefully, recording `Acked` vs `Unknown` outcomes
///     for linearizability & durability checking.
class ZooKeeperWorkload
{
public:
    struct ClientSession
    {
        int64_t session_id = 0;
        int64_t next_xid = 100;
        int64_t timeout_ms = 8000;
        bool alive = false;
        bool letting_expire = false;
    };

    ZooKeeperWorkload(SimEngine & engine_, SimCluster & cluster_, int64_t workload_end_ms_)
        : engine(engine_)
        , cluster(cluster_)
        , workload_end_ms(workload_end_ms_)
    {
    }

    void start()
    {
        scheduleNextStep(250);
        schedulePeriodicVerify(2500);
    }

    void stop()
    {
        stopped = true;
    }

    /// Submit a final barrier write during quieting to prove liveness & trigger full catch-up.
    bool submitFinalBarrier(int leader_id)
    {
        auto req = std::make_shared<Coordination::ZooKeeperCreateRequest>();
        req->xid = 900000 + (++barrier_counter);
        req->path = "/sim/final_barrier_" + std::to_string(barrier_counter);
        req->data = "barrier-" + std::to_string(engine.rng.nextU32());
        req->acls = {{31, "world", "anyone"}};
        const int64_t sid = pickAnyAliveSession();
        if (sid == 0)
            return false;

        auto res = cluster.submitRequest(leader_id, req, sid);
        if (!res || !res->get_accepted())
            return false;

        cluster.getKeeperOracle().recordClientInvoke(sid, req->xid, g_sim_time_ns);
        const int64_t xid = req->xid;
        res->when_ready(
            [this, sid, xid](nuraft::cmd_result<nuraft::ptr<nuraft::buffer>> & r, nuraft::ptr<std::exception> & err)
            {
                const bool ok = !err && r.get_result_code() == nuraft::cmd_result_code::OK;
                cluster.getKeeperOracle().recordClientOutcome(sid, xid, ok, g_sim_time_ns);
                if (ok)
                    final_barrier_acked = true;
            });
        return true;
    }

    bool isFinalBarrierAcked() const { return final_barrier_acked; }

    uint64_t ops_submitted = 0;
    uint64_t ops_rejected_no_leader = 0;
    uint64_t reads_executed = 0;
    uint64_t linearizable_reg_writes_acked = 0;
    uint64_t linearizable_reg_checks = 0;

    /// Final strict linearizability check on `/sim/lin_reg` after convergence.
    void verifyLinearizabilityRegister()
    {
        if (last_acked_lin_val == 0)
            return;
        for (const auto & [id, node] : cluster.getNodes())
        {
            if (!node.online || !node.machine)
                continue;
            std::string data;
            const bool exists = node.machine->getStorageUnsafe().nodes_storage->getCommittedNodeSimple("/sim/lin_reg", nullptr, &data);
            require(exists, "LINEARIZABILITY_REGISTER: /sim/lin_reg missing on node " + std::to_string(id));
            const uint64_t actual = std::stoull(data);
            require(actual >= last_acked_lin_val,
                "LINEARIZABILITY_REGISTER: stale value on node " + std::to_string(id)
                + ": actual=" + std::to_string(actual) + " < last_acked=" + std::to_string(last_acked_lin_val));
            ++linearizable_reg_checks;
        }
        engine.coverage.hit("linearizability_register_verified");
    }

private:
    void scheduleNextStep(int64_t delay_ms)
    {
        engine.scheduleAfterMs(
            delay_ms,
            "workload-tick",
            [this]()
            {
                if (stopped || simTimeMs() >= workload_end_ms)
                    return;
                runOneWorkloadStep();
                const int64_t next_delay = engine.rng.rangeInt(15, 75);
                scheduleNextStep(next_delay);
            });
    }

    void schedulePeriodicVerify(int64_t interval_ms)
    {
        engine.scheduleAfterMs(
            interval_ms,
            "oracle-periodic-tree-check",
            [this, interval_ms]()
            {
                if (stopped || simTimeMs() >= workload_end_ms)
                    return;
                cluster.verifyAllOnlineNodes();
                schedulePeriodicVerify(interval_ms);
            });
    }

    int64_t pickAnyAliveSession() const
    {
        for (const auto & [sid, s] : sessions)
        {
            if (s.alive && !s.letting_expire)
                return sid;
        }
        return 0;
    }

    void runOneWorkloadStep()
    {
        const int leader = cluster.findLeader();
        if (leader == 0)
        {
            ++ops_rejected_no_leader;
            return;
        }

        /// Bootstrap initial sessions & `/sim` namespace root first.
        if (sessions.size() < 3 && !pending_session_alloc)
        {
            allocateSession(leader);
            return;
        }
        if (!root_created && !pending_root_create)
        {
            const int64_t sid = pickAnyAliveSession();
            if (sid != 0)
            {
                createSimRoot(leader, sid);
                return;
            }
        }
        if (!root_created)
            return;

        /// Periodically send heartbeats for active sessions that are NOT deliberately expiring.
        for (auto & [sid, sess] : sessions)
        {
            if (sess.alive && !sess.letting_expire && engine.rng.chance(0.18))
            {
                auto hb = std::make_shared<Coordination::ZooKeeperHeartbeatRequest>();
                hb->xid = sess.next_xid++;
                cluster.submitRequest(leader, hb, sid);
            }
        }

        /// Occasionally let a secondary session expire naturally to test virtual-time SessionExpiryQueue + ephemeral cleanup!
        if (sessions.size() >= 3 && engine.rng.chance(0.03))
        {
            for (auto & [sid, sess] : sessions)
            {
                if (sid > 1 && sess.alive && !sess.letting_expire)
                {
                    sess.letting_expire = true;
                    engine.trace("WORKLOAD_LET_SESSION_EXPIRE sid=" + std::to_string(sid));
                    break;
                }
            }
        }

        /// Re-allocate a replacement session if we have fewer than 2 non-expiring alive sessions.
        size_t active_non_expiring = 0;
        for (const auto & [sid, sess] : sessions)
            if (sess.alive && !sess.letting_expire)
                ++active_non_expiring;
        if (active_non_expiring < 2 && !pending_session_alloc)
        {
            allocateSession(leader);
            return;
        }

        /// Mix of linearizability register updates, general writes (Single + Multi), and local Reads + Watches!
        if (engine.rng.chance(0.12))
        {
            const int64_t lin_sid = pickAnyAliveSession();
            if (lin_sid != 0)
            {
                advanceLinearizabilityRegister(leader, lin_sid);
                return;
            }
        }
        if (engine.rng.chance(0.28))
        {
            issueReadWithWatch();
            return;
        }

        std::vector<int64_t> candidates;
        for (const auto & [sid, sess] : sessions)
            if (sess.alive && !sess.letting_expire)
                candidates.push_back(sid);
        if (candidates.empty())
            return;

        const int64_t sid = engine.rng.pick(candidates);
        auto & sess = sessions.at(sid);

        /// With 35% probability, submit a pipelined burst of 2-3 writes!
        const size_t burst = engine.rng.chance(0.35) ? engine.rng.rangeSize(2, 3) : 1;
        for (size_t b = 0; b < burst; ++b)
        {
            Coordination::ZooKeeperRequestPtr req;
            if (engine.rng.chance(0.35))
                req = buildMultiRequest(sess);
            else
                req = buildSingleWriteRequest(sess);

            req->xid = sess.next_xid++;
            const int64_t xid = req->xid;
            auto res = cluster.submitRequest(leader, req, sid);
            if (!res || !res->get_accepted())
            {
                ++ops_rejected_no_leader;
                break;
            }
            ++ops_submitted;
            cluster.getKeeperOracle().recordClientInvoke(sid, xid, g_sim_time_ns);
            res->when_ready(
                [this, sid, xid](nuraft::cmd_result<nuraft::ptr<nuraft::buffer>> & r, nuraft::ptr<std::exception> & err)
                {
                    const bool ok = !err && r.get_result_code() == nuraft::cmd_result_code::OK;
                    cluster.getKeeperOracle().recordClientOutcome(sid, xid, ok, g_sim_time_ns);
                });
        }
    }

    void allocateSession(int leader)
    {
        pending_session_alloc = true;
        auto req = std::make_shared<Coordination::ZooKeeperSessionIDRequest>();
        req->xid = 10 + (++session_alloc_seq);
        req->internal_id = 100 + session_alloc_seq;
        req->server_id = leader;
        /// Short enough virtual timeout (4–9s) so session expiry genuinely triggers during a 20–40s run!
        req->session_timeout_ms = (session_alloc_seq == 1) ? 120000 : engine.rng.rangeInt(3500, 6500);
        const int64_t timeout = req->session_timeout_ms;

        auto res = cluster.submitRequest(leader, req, -1);
        if (!res || !res->get_accepted())
        {
            pending_session_alloc = false;
            return;
        }
        res->when_ready(
            [this, timeout](nuraft::cmd_result<nuraft::ptr<nuraft::buffer>> & r, nuraft::ptr<std::exception> & err)
            {
                pending_session_alloc = false;
                if (!err && r.get_result_code() == nuraft::cmd_result_code::OK)
                {
                    /// Sync session list with the oracle's active sessions.
                    for (int64_t sid : cluster.getKeeperOracle().latestTree().active_sessions)
                    {
                        if (!sessions.contains(sid))
                            sessions[sid] = ClientSession{sid, 100, timeout, true, false};
                    }
                }
            });
    }

    void createSimRoot(int leader, int64_t sid)
    {
        pending_root_create = true;
        auto req = std::make_shared<Coordination::ZooKeeperCreateRequest>();
        req->xid = sessions.at(sid).next_xid++;
        req->path = "/sim";
        req->data = "sim-root";
        req->acls = {{31, "world", "anyone"}};
        const int64_t xid = req->xid;
        auto res = cluster.submitRequest(leader, req, sid);
        if (!res || !res->get_accepted())
        {
            pending_root_create = false;
            return;
        }
        cluster.getKeeperOracle().recordClientInvoke(sid, xid, g_sim_time_ns);
        res->when_ready(
            [this, sid, xid](nuraft::cmd_result<nuraft::ptr<nuraft::buffer>> & r, nuraft::ptr<std::exception> & err)
            {
                pending_root_create = false;
                const bool ok = !err && r.get_result_code() == nuraft::cmd_result_code::OK;
                cluster.getKeeperOracle().recordClientOutcome(sid, xid, ok, g_sim_time_ns);
                if (cluster.getKeeperOracle().latestTree().nodes.contains("/sim"))
                    root_created = true;
            });
    }

    std::string pickPath()
    {
        const int64_t bucket = engine.rng.rangeInt(0, 7);
        std::string p = "/sim/k" + std::to_string(bucket);
        if (engine.rng.chance(0.35))
            p += "/c" + std::to_string(engine.rng.rangeInt(0, 2));
        return p;
    }

    Coordination::ZooKeeperRequestPtr buildSingleWriteRequest(ClientSession & /*sess*/)
    {
        const int64_t kind = engine.rng.rangeInt(0, 9);
        if (kind <= 3)
        {
            /// Create (persistent, ephemeral, or sequential)
            auto req = std::make_shared<Coordination::ZooKeeperCreateRequest>();
            const bool seq = (kind == 3);
            req->path = seq ? ("/sim/seq-" + std::to_string(engine.rng.rangeInt(0, 1)) + "-") : pickPath();
            req->data = "v-" + std::to_string(engine.rng.nextU32());
            req->is_ephemeral = (!seq && engine.rng.chance(0.35));
            req->is_sequential = seq;
            req->acls = {{31, "world", "anyone"}};
            return req;
        }
        if (kind <= 6)
        {
            /// Set (unconditional or versioned CAS)
            auto req = std::make_shared<Coordination::ZooKeeperSetRequest>();
            req->path = pickPath();
            req->data = "upd-" + std::to_string(engine.rng.nextU32());
            if (engine.rng.chance(0.5))
            {
                const auto & tree = cluster.getKeeperOracle().latestTree().nodes;
                auto it = tree.find(req->path);
                if (it != tree.end() && engine.rng.chance(0.7))
                    req->version = it->second.version;
                else
                    req->version = static_cast<int32_t>(engine.rng.rangeInt(0, 3));
            }
            else
            {
                req->version = -1;
            }
            return req;
        }
        if (kind <= 8)
        {
            /// Remove (versioned or unconditional)
            auto req = std::make_shared<Coordination::ZooKeeperRemoveRequest>();
            req->path = pickPath();
            req->version = engine.rng.chance(0.5) ? -1 : static_cast<int32_t>(engine.rng.rangeInt(0, 2));
            return req;
        }
        auto req = std::make_shared<Coordination::ZooKeeperCheckRequest>();
        req->path = pickPath();
        req->version = engine.rng.chance(0.5) ? -1 : static_cast<int32_t>(engine.rng.rangeInt(0, 2));
        return req;
    }

    Coordination::ZooKeeperRequestPtr buildMultiRequest(ClientSession & sess)
    {
        auto multi = std::make_shared<Coordination::ZooKeeperMultiRequest>();
        const int64_t pattern = engine.rng.rangeInt(0, 3);
        const std::string base = "/sim/m" + std::to_string(engine.rng.rangeInt(0, 4));
        if (pattern == 0)
        {
            /// Atomic create parent + child + set child (commits when base doesn't exist, rolls back if exists).
            auto c1 = std::make_shared<Coordination::ZooKeeperCreateRequest>();
            c1->path = base;
            c1->data = "mp-" + std::to_string(engine.rng.nextU32());
            c1->acls = {{31, "world", "anyone"}};
            auto c2 = std::make_shared<Coordination::ZooKeeperCreateRequest>();
            c2->path = base + "/ch";
            c2->data = "mc-0";
            c2->is_ephemeral = engine.rng.chance(0.4);
            c2->acls = {{31, "world", "anyone"}};
            auto s1 = std::make_shared<Coordination::ZooKeeperSetRequest>();
            s1->path = base + "/ch";
            s1->data = "mc-1-" + std::to_string(engine.rng.nextU32());
            s1->version = 0;
            multi->requests = {c1, c2, s1};
        }
        else if (pattern == 1)
        {
            /// Intentional mid-transaction failure -> MUST rollback earlier mutation!
            auto s1 = std::make_shared<Coordination::ZooKeeperSetRequest>();
            s1->path = "/sim";
            s1->data = "must-rollback-" + std::to_string(engine.rng.nextU32());
            s1->version = -1;
            auto chk = std::make_shared<Coordination::ZooKeeperCheckRequest>();
            chk->path = "/sim/never_exists_xyz";
            chk->version = 0;
            auto c1 = std::make_shared<Coordination::ZooKeeperCreateRequest>();
            c1->path = base + "/unreached";
            c1->data = "bad";
            c1->acls = {{31, "world", "anyone"}};
            multi->requests = {s1, chk, c1};
        }
        else if (pattern == 2)
        {
            /// Atomic remove child + remove parent.
            auto r1 = std::make_shared<Coordination::ZooKeeperRemoveRequest>();
            r1->path = base + "/ch";
            r1->version = -1;
            auto r2 = std::make_shared<Coordination::ZooKeeperRemoveRequest>();
            r2->path = base;
            r2->version = -1;
            multi->requests = {r1, r2};
        }
        else
        {
            /// Random 2-3 op mix.
            const size_t n = engine.rng.rangeSize(2, 3);
            for (size_t i = 0; i < n; ++i)
                multi->requests.push_back(buildSingleWriteRequest(sess));
        }
        return multi;
    }

    void issueReadWithWatch()
    {
        auto online = cluster.onlineNodes();
        if (online.empty())
            return;
        const int64_t sid = pickAnyAliveSession();
        if (sid == 0)
            return;
        const int target_node = engine.rng.pick(online);
        const std::string path = engine.rng.chance(0.3) ? "/sim" : pickPath();

        const int64_t kind = engine.rng.rangeInt(0, 2);
        Coordination::ZooKeeperRequestPtr req;
        if (kind == 0)
        {
            auto r = std::make_shared<Coordination::ZooKeeperGetRequest>();
            r->path = path;
            r->has_watch = true;
            req = r;
        }
        else if (kind == 1)
        {
            auto r = std::make_shared<Coordination::ZooKeeperListRequest>();
            r->path = path;
            r->has_watch = true;
            req = r;
        }
        else
        {
            auto r = std::make_shared<Coordination::ZooKeeperExistsRequest>();
            r->path = path;
            r->has_watch = true;
            req = r;
        }
        req->xid = 500000 + static_cast<int64_t>(++reads_executed);
        cluster.executeLocalRead(target_node, req, sid);
    }

    void advanceLinearizabilityRegister(int leader, int64_t sid)
    {
        if (lin_write_in_flight)
            return;
        lin_write_in_flight = true;
        const uint64_t next_val = ++lin_seq_counter;
        auto & sess = sessions.at(sid);

        Coordination::ZooKeeperRequestPtr req;
        if (!lin_reg_created)
        {
            auto creq = std::make_shared<Coordination::ZooKeeperCreateRequest>();
            creq->path = "/sim/lin_reg";
            creq->data = std::to_string(next_val);
            creq->acls = {{31, "world", "anyone"}};
            req = creq;
        }
        else
        {
            auto sreq = std::make_shared<Coordination::ZooKeeperSetRequest>();
            sreq->path = "/sim/lin_reg";
            sreq->data = std::to_string(next_val);
            sreq->version = -1;
            req = sreq;
        }
        req->xid = sess.next_xid++;
        const int64_t xid = req->xid;
        auto res = cluster.submitRequest(leader, req, sid);
        if (!res || !res->get_accepted())
        {
            lin_write_in_flight = false;
            return;
        }
        ++ops_submitted;
        cluster.getKeeperOracle().recordClientInvoke(sid, xid, g_sim_time_ns);
        res->when_ready(
            [this, sid, xid, next_val](nuraft::cmd_result<nuraft::ptr<nuraft::buffer>> & r, nuraft::ptr<std::exception> & err)
            {
                lin_write_in_flight = false;
                const bool ok = !err && r.get_result_code() == nuraft::cmd_result_code::OK;
                cluster.getKeeperOracle().recordClientOutcome(sid, xid, ok, g_sim_time_ns);
                if (ok)
                {
                    lin_reg_created = true;
                    last_acked_lin_val = std::max(last_acked_lin_val, next_val);
                    ++linearizable_reg_writes_acked;
                }
                else if (cluster.getKeeperOracle().latestTree().nodes.contains("/sim/lin_reg"))
                {
                    lin_reg_created = true;
                }
            });
    }

    SimEngine & engine;
    SimCluster & cluster;
    int64_t workload_end_ms;
    bool stopped = false;
    bool root_created = false;
    bool pending_root_create = false;
    bool pending_session_alloc = false;
    int64_t session_alloc_seq = 0;
    int64_t barrier_counter = 0;
    bool final_barrier_acked = false;
    bool lin_reg_created = false;
    bool lin_write_in_flight = false;
    uint64_t lin_seq_counter = 0;
    uint64_t last_acked_lin_val = 0;
    std::map<int64_t, ClientSession> sessions;
};

/// FoundationDB-inspired Continuous Chaos / Fault-Injection Actor.
/// Combines FDB's `SwizzleClogger`, `Attrition`, `Rollback` (power-loss), `SnapshotStorm`,
/// and `TransientDiskFaults` into a single deterministic virtual-time fault actor!
class ChaosActor
{
public:
    ChaosActor(SimEngine & engine_, SimCluster & cluster_, std::string profile_, int64_t chaos_end_ms_)
        : engine(engine_)
        , cluster(cluster_)
        , profile(std::move(profile_))
        , chaos_end_ms(chaos_end_ms_)
    {
    }

    void start()
    {
        if (profile == "clean")
            return;
        scheduleNextChaos(engine.rng.rangeInt(800, 1500));
    }

    void stopAndHeal()
    {
        stopped = true;
        cluster.getNetwork().healAll();
        for (int id = 1; id <= cluster.getConfig().num_nodes; ++id)
        {
            if (!cluster.isOnline(id))
                cluster.startNode(id);
        }
        engine.trace("CHAOS_STOP_AND_HEAL");
    }

    uint64_t fault_actions = 0;

private:
    void scheduleNextChaos(int64_t delay_ms)
    {
        engine.scheduleAfterMs(
            delay_ms,
            "chaos-tick",
            [this]()
            {
                if (stopped || simTimeMs() >= chaos_end_ms)
                    return;
                runChaosAction();
                const int64_t min_gap = (profile == "chaos" || profile == "partition-storm") ? 500 : 900;
                const int64_t max_gap = (profile == "chaos" || profile == "partition-storm") ? 1800 : 2800;
                scheduleNextChaos(engine.rng.rangeInt(min_gap, max_gap));
            });
    }

    void runChaosAction()
    {
        ++fault_actions;
        const int n = cluster.getConfig().num_nodes;
        const int quorum = n / 2 + 1;

        /// Always heal stale partitions periodically so the cluster gets real progress windows
        /// (FDB calls this "clog/unclog rhythm": chaos must create AND heal faults to test recovery!).
        if (engine.rng.chance(0.28))
        {
            cluster.getNetwork().healAll();
            return;
        }

        /// Choose fault category weighted by profile.
        const int64_t roll = engine.rng.rangeInt(0, 99);

        if (profile == "snapshot-storm" && roll < 50)
        {
            injectSnapshotAndWipeFault(n, quorum);
            return;
        }
        if (profile == "power-loss" && roll < 55)
        {
            injectNodeCrash(n, quorum, KillMode::PowerLoss);
            return;
        }
        if (profile == "partition-storm" && roll < 65)
        {
            injectNetworkFault(n);
            return;
        }
        if (profile == "figure8-hunt")
        {
            injectFigure8HuntFault(n);
            return;
        }

        /// Balanced / Chaos / LSMT mixed FDB fault menu:
        if (roll < 30)
            injectNetworkFault(n);
        else if (roll < 55)
            injectNodeCrash(n, quorum, engine.rng.chance(0.75) ? KillMode::PowerLoss : KillMode::Graceful);
        else if (roll < 72)
            injectDiskIoFault(n);
        else if (roll < 88)
            injectSnapshotAndWipeFault(n, quorum);
        else
            injectSwizzleClog(n);
    }

    void injectNetworkFault(int n)
    {
        cluster.getNetwork().healAll();
        const int leader = cluster.findLeader();

        if (engine.buggify.isActive("asymmetric_partitions") && engine.rng.chance(0.45))
        {
            /// Asymmetric one-way link cut: e.g., leader can send to peer or peer cannot reply!
            const int a = (leader != 0 && engine.rng.chance(0.6)) ? leader : static_cast<int>(engine.rng.rangeInt(1, n));
            int b = static_cast<int>(engine.rng.rangeInt(1, n));
            if (b == a)
                b = (a % n) + 1;
            cluster.getNetwork().partitionAsymmetric(a, b);
            return;
        }

        /// Symmetric partition: isolate current leader OR split into minority + majority.
        std::set<int> side_a;
        std::set<int> side_b;
        if (leader != 0 && engine.rng.chance(0.65))
        {
            side_a.insert(leader);
            if (n == 5 && engine.rng.chance(0.5))
            {
                int partner = (leader % n) + 1;
                side_a.insert(partner);
            }
            for (int id = 1; id <= n; ++id)
                if (!side_a.contains(id))
                    side_b.insert(id);
        }
        else
        {
            const int cut_node = static_cast<int>(engine.rng.rangeInt(1, n));
            side_a.insert(cut_node);
            for (int id = 1; id <= n; ++id)
                if (id != cut_node)
                    side_b.insert(id);
        }
        cluster.getNetwork().partitionSymmetric(side_a, side_b);
    }

    void injectSwizzleClog(int n)
    {
        /// FoundationDB SwizzleClogger: degrade 1..3 directed links with high latency, packet drops, and reply drops.
        const size_t count = engine.rng.rangeSize(1, 3);
        for (size_t i = 0; i < count; ++i)
        {
            const int a = static_cast<int>(engine.rng.rangeInt(1, n));
            int b = static_cast<int>(engine.rng.rangeInt(1, n));
            if (b == a)
                b = (a % n) + 1;
            const int64_t extra_ms = engine.rng.rangeInt(25, 180);
            const double drop_p = engine.rng.chance(0.5) ? 0.25 : 0.05;
            const double resp_drop_p = engine.rng.chance(0.5) ? 0.30 : 0.05;
            cluster.getNetwork().clogLink(a, b, extra_ms, drop_p, resp_drop_p);
        }
    }

    void injectNodeCrash(int n, int quorum, KillMode mode)
    {
        auto online = cluster.onlineNodes();
        /// Keep at least a quorum alive (except occasionally allow temporary quorum loss for graceful/power-loss!).
        const bool allow_temp_quorum_loss = (mode != KillMode::WipeAndRejoin && engine.rng.chance(0.15));
        const int min_online = allow_temp_quorum_loss ? std::max(1, quorum - 1) : quorum;
        if (static_cast<int>(online.size()) <= min_online)
            return;

        /// Prefer crashing the leader 50% of the time to stress failover & uncommitted tail handling!
        const int leader = cluster.findLeader();
        int victim = 0;
        if (leader != 0 && engine.rng.chance(0.50))
            victim = leader;
        else
            victim = engine.rng.pick(online);

        /// Never wipe a node unless at least a quorum of OTHER online nodes already hold
        /// the entire committed prefix on disk (otherwise wiping destroys Raft quorum durability).
        if (mode == KillMode::WipeAndRejoin)
        {
            if (static_cast<int>(online.size()) < n)
                return;
            const uint64_t highest_committed = cluster.getRaftOracle().highestCommittedIndex();
            std::vector<int> safe_victims;
            for (int candidate : online)
            {
                if (candidate == leader && n == 3)
                    continue;
                int other_up_to_date = 0;
                for (int peer : online)
                {
                    if (peer == candidate)
                        continue;
                    const auto & pnode = cluster.getNodes().at(peer);
                    if (pnode.online && pnode.log_store && pnode.log_store->last_durable_index() >= highest_committed)
                        ++other_up_to_date;
                }
                if (other_up_to_date >= quorum)
                    safe_victims.push_back(candidate);
            }
            if (safe_victims.empty())
                return;
            victim = engine.rng.pick(safe_victims);
        }

        cluster.crashNode(victim, mode);
        const int64_t downtime_ms = engine.rng.rangeInt(350, 1800);
        engine.scheduleAfterMs(
            downtime_ms,
            "node-reboot",
            [this, victim]()
            {
                if (!cluster.isOnline(victim))
                    cluster.startNode(victim);
            });
    }

    void injectDiskIoFault(int n)
    {
        if (!engine.buggify.isActive("transient_disk_io_errors"))
            return;
        auto online = cluster.onlineNodes();
        if (online.empty())
            return;
        const int target = engine.rng.pick(online);
        auto & node = cluster.getNodes().at(target);

        /// Arm transient snapshot fault (open / partial / sync / marker remove) and trigger a snapshot!
        static const std::vector<std::pair<std::string, SimCrashDisk::TransientFaultKind>> faults = {
            {"snapshot_", SimCrashDisk::TransientFaultKind::Open},
            {"snapshot_", SimCrashDisk::TransientFaultKind::PartialWrite},
            {"snapshot_", SimCrashDisk::TransientFaultKind::SyncError},
            {"tmp_snapshot_", SimCrashDisk::TransientFaultKind::RemoveError},
        };
        const auto & [prefix, kind] = engine.rng.pick(faults);
        node.snapshot_disk->armTransientFault(prefix, kind);
        cluster.triggerSnapshot(target);
        (void)n;
    }

    void injectSnapshotAndWipeFault(int n, int quorum)
    {
        auto online = cluster.onlineNodes();
        if (online.empty())
            return;

        /// Trigger snapshot on leader or random online node, and occasionally wipe a lagging follower
        /// so NuRaft must execute a real multi-chunk network snapshot installation!
        const int leader = cluster.findLeader();
        if (leader != 0)
            cluster.triggerSnapshot(leader);
        else
            cluster.triggerSnapshot(engine.rng.pick(online));

        if (engine.rng.chance(0.45) && cluster.getKeeperOracle().highestModeledIndex() >= 10)
            injectNodeCrash(n, quorum, KillMode::WipeAndRejoin);
    }

    void injectFigure8HuntFault(int n)
    {
        /// High-churn asymmetric & split partitions on 5-node clusters designed to organically
        /// explore overlapping elections and old-term entry replication (Raft Figure 8 space).
        cluster.getNetwork().healAll();
        const int leader = cluster.findLeader();
        if (leader == 0)
            return;
        if (engine.rng.chance(0.55))
        {
            /// Split 2 vs 3 (minority containing current leader).
            std::set<int> minority{leader};
            int helper = static_cast<int>(engine.rng.rangeInt(1, n));
            if (helper == leader)
                helper = (leader % n) + 1;
            minority.insert(helper);
            std::set<int> majority;
            for (int id = 1; id <= n; ++id)
                if (!minority.contains(id))
                    majority.insert(id);
            cluster.getNetwork().partitionSymmetric(minority, majority);
        }
        else
        {
            injectSwizzleClog(n);
        }
    }

    SimEngine & engine;
    SimCluster & cluster;
    std::string profile;
    int64_t chaos_end_ms;
    bool stopped = false;
};

} // namespace fdb_sim
