#pragma once

#include <SimCluster.h>

#include <algorithm>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace keeper_sim
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
        bool auth_submitted = false;
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
        final_barrier_in_flight = true;
        const int64_t xid = req->xid;
        res->when_ready(
            [this, sid, xid](nuraft::cmd_result<nuraft::ptr<nuraft::buffer>> & r, nuraft::ptr<std::exception> & err)
            {
                final_barrier_in_flight = false;
                const bool ok = !err && r.get_result_code() == nuraft::cmd_result_code::OK;
                cluster.getKeeperOracle().recordClientOutcome(sid, xid, ok, g_sim_time_ns);
                if (ok)
                    final_barrier_acked = true;
            });
        return true;
    }

    bool isFinalBarrierAcked() const { return final_barrier_acked; }
    bool isFinalBarrierInFlight() const { return final_barrier_in_flight; }

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
        for (int id : cluster.currentClusterMemberIds())
        {
            const auto & node = cluster.getNodes().at(id);
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
            if (sess.alive && !sess.letting_expire && !sess.auth_submitted && (sid == 1 || engine.rng.chance(0.55)))
            {
                sess.auth_submitted = true;
                auto auth = std::make_shared<Coordination::ZooKeeperAuthRequest>();
                auth->xid = sess.next_xid++;
                auth->scheme = "digest";
                if (engine.buggify.acl_torture)
                {
                    const int64_t uid = (sid == 1) ? 0 : engine.rng.rangeInt(0, 29);
                    auth->data = "u" + std::to_string(uid) + ":p" + std::to_string(uid);
                }
                else
                {
                    auth->data = (sid % 2 == 1) ? "simuser:simpass" : "simuser2:pass2";
                }
                cluster.submitRequest(leader, auth, sid);
            }
            if (engine.buggify.acl_torture && sess.alive && !sess.letting_expire && engine.rng.chance(0.08))
            {
                auto auth = std::make_shared<Coordination::ZooKeeperAuthRequest>();
                auth->xid = sess.next_xid++;
                auth->scheme = "digest";
                const int64_t uid = engine.rng.rangeInt(0, 29);
                auth->data = "u" + std::to_string(uid) + ":p" + std::to_string(uid);
                cluster.submitRequest(leader, auth, sid);
            }
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

        /// Late in-flight request race after session Close (models follower-forwarded write or local read
        /// arriving after leader's sessionCleanerTask commits Close(sid)).
        if (engine.buggify.enable_late_session_race
            && !cluster.getRecentlyClosedSessions().empty()
            && engine.rng.chance(0.35))
        {
            const int64_t closed_sid = engine.rng.pick(cluster.getRecentlyClosedSessions());
            if (engine.rng.chance(0.50))
            {
                auto online = cluster.onlineNodes();
                if (!online.empty())
                {
                    auto g = std::make_shared<Coordination::ZooKeeperGetRequest>();
                    g->path = "/sim";
                    g->xid = 550000 + static_cast<int64_t>(++reads_executed);
                    cluster.executeLocalRead(engine.rng.pick(online), g, closed_sid);
                    engine.coverage.hit("late_closed_session_read");
                }
            }
            else
            {
                auto hb = std::make_shared<Coordination::ZooKeeperHeartbeatRequest>();
                hb->xid = 560000 + static_cast<int64_t>(++ops_submitted);
                cluster.submitRequest(leader, hb, closed_sid);
                engine.coverage.hit("late_closed_session_write");
            }
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

        /// When `--client-stream` is active, pipeline writes via NuRaft's `open_client_req_stream`
        /// on BOTH followers and the leader!
        if (engine.buggify.client_stream && engine.rng.chance(0.60))
        {
            auto online = cluster.onlineNodes();
            if (!online.empty())
            {
                const int stream_node = engine.rng.pick(online);
                const int64_t sid = engine.rng.pick(candidates);
                auto & sess = sessions.at(sid);
                const size_t n = engine.rng.rangeSize(1, 3);
                for (size_t i = 0; i < n; ++i)
                {
                    Coordination::ZooKeeperRequestPtr req = engine.rng.chance(0.30)
                        ? buildMultiRequest(sess)
                        : buildSingleWriteRequest(sess);
                    req->xid = sess.next_xid++;
                    if (cluster.submitViaClientStream(stream_node, std::move(req), sid))
                        ++ops_submitted;
                    else
                        break;
                }
                return;
            }
        }

        /// Choose target server for writes: leader normally, or 65% follower when `--auto-forward` is active!
        int write_target = leader;
        if (engine.buggify.auto_forward && engine.rng.chance(0.65))
        {
            std::vector<int> followers;
            for (int oid : cluster.onlineNodes())
                if (oid != leader)
                    followers.push_back(oid);
            if (!followers.empty())
                write_target = engine.rng.pick(followers);
        }

        /// With `multi_request_batches` active, pack 2..5 mixed requests into a single Raft log entry (`write_log_entry_version = 1`)!
        if (engine.buggify.multi_request_batches && engine.rng.chance(0.45))
        {
            const size_t batch_size = engine.rng.rangeSize(2, 5);
            std::vector<DB::KeeperRequestForSession> batch_entries;
            std::vector<std::pair<int64_t, int64_t>> batch_ids;
            batch_entries.reserve(batch_size);
            for (size_t b = 0; b < batch_size; ++b)
            {
                int64_t b_sid = engine.rng.pick(candidates);
                int64_t forced_xid = 0;
                if (b == 0 && engine.buggify.batch_cache_retry && !unknown_retry_xids.empty() && engine.rng.chance(0.40))
                {
                    auto [rsid, rxid] = unknown_retry_xids.back();
                    unknown_retry_xids.pop_back();
                    if (sessions.contains(rsid) && sessions.at(rsid).alive)
                    {
                        b_sid = rsid;
                        forced_xid = rxid;
                        engine.coverage.hit("batch_cache_retry_reused_xid");
                    }
                }
                auto & b_sess = sessions.at(b_sid);
                Coordination::ZooKeeperRequestPtr req = engine.rng.chance(0.30)
                    ? buildMultiRequest(b_sess)
                    : buildSingleWriteRequest(b_sess);
                req->xid = forced_xid != 0 ? forced_xid : b_sess.next_xid++;
                batch_ids.emplace_back(b_sid, req->xid);
                DB::KeeperRequestForSession entry;
                entry.session_id = b_sid;
                entry.time = simTimeMs();
                entry.request = std::move(req);
                batch_entries.push_back(std::move(entry));
            }
            auto res = cluster.submitRequestBatch(write_target, std::move(batch_entries), /*log_entry_version=*/1);
            if (!res || !res->get_accepted())
            {
                ++ops_rejected_no_leader;
                return;
            }
            ops_submitted += batch_ids.size();
            for (const auto & [b_sid, b_xid] : batch_ids)
                cluster.getKeeperOracle().recordClientInvoke(b_sid, b_xid, g_sim_time_ns);
            res->when_ready(
                [this, batch_ids](nuraft::cmd_result<nuraft::ptr<nuraft::buffer>> & r, nuraft::ptr<std::exception> & err)
                {
                    const bool ok = !err && r.get_result_code() == nuraft::cmd_result_code::OK;
                    for (const auto & [b_sid, b_xid] : batch_ids)
                    {
                        cluster.getKeeperOracle().recordClientOutcome(b_sid, b_xid, ok, g_sim_time_ns);
                        if (!ok && engine.buggify.batch_cache_retry && unknown_retry_xids.size() < 32)
                            unknown_retry_xids.emplace_back(b_sid, b_xid);
                    }
                });
            return;
        }

        const int64_t sid = engine.rng.pick(candidates);
        auto & sess = sessions.at(sid);

        /// With 35% probability (or 80% under rollback_torture), submit a pipelined burst of writes!
        const size_t burst = engine.buggify.rollback_torture
            ? engine.rng.rangeSize(4, 6)
            : (engine.rng.chance(0.35) ? engine.rng.rangeSize(2, 3) : 1);
        for (size_t b = 0; b < burst; ++b)
        {
            Coordination::ZooKeeperRequestPtr req;
            if (engine.rng.chance(0.35))
                req = buildMultiRequest(sess);
            else
                req = buildSingleWriteRequest(sess);

            if (engine.buggify.batch_cache_retry && !unknown_retry_xids.empty() && engine.rng.chance(0.25))
            {
                auto [rsid, rxid] = unknown_retry_xids.back();
                unknown_retry_xids.pop_back();
                if (rsid == sid)
                {
                    req->xid = rxid;
                    engine.coverage.hit("batch_cache_retry_reused_xid");
                }
                else
                {
                    req->xid = sess.next_xid++;
                }
            }
            else
            {
                req->xid = sess.next_xid++;
                if (engine.buggify.quorum_reads && engine.rng.chance(0.50))
                    req->xid |= (static_cast<int64_t>(engine.rng.rangeInt(1, 7)) << 33);
            }
            const int64_t xid = req->xid;
            auto res = cluster.submitRequest(write_target, req, sid);
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
                    if (!ok && engine.buggify.batch_cache_retry && unknown_retry_xids.size() < 32)
                        unknown_retry_xids.emplace_back(sid, xid);
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
        if (engine.buggify.rollback_torture)
            return engine.rng.chance(0.50) ? "/sim/h0" : "/sim/h0/c0";
        if (engine.buggify.deep_lsmt_stress)
        {
            const int64_t b = engine.rng.rangeInt(0, 15);
            std::string p = "/sim/k" + std::to_string(b);
            if (engine.rng.chance(0.75))
            {
                p += "/c" + std::to_string(engine.rng.rangeInt(0, 3));
                if (engine.rng.chance(0.65))
                {
                    p += "/d" + std::to_string(engine.rng.rangeInt(0, 2));
                    if (engine.rng.chance(0.50))
                        p += "/e" + std::to_string(engine.rng.rangeInt(0, 1));
                }
            }
            return p;
        }
        const int64_t bucket = engine.rng.rangeInt(0, 7);
        std::string p = "/sim/k" + std::to_string(bucket);
        if (engine.rng.chance(0.35))
        {
            p += "/c" + std::to_string(engine.rng.rangeInt(0, 2));
            if (engine.buggify.large_values && engine.rng.chance(0.35))
                p += "/d" + std::to_string(engine.rng.rangeInt(0, 1));
        }
        return p;
    }

    std::string makePayload(const std::string & prefix)
    {
        std::string s = prefix + std::to_string(engine.rng.nextU32());
        if (engine.buggify.large_values)
        {
            const size_t pad = engine.rng.rangeSize(200, 640);
            s.append(pad, static_cast<char>('a' + (engine.rng.nextU32() % 26)));
        }
        return s;
    }

    Coordination::ZooKeeperRequestPtr buildSingleWriteRequest(ClientSession & /*sess*/, bool for_multi = false)
    {
        static const std::string simuser_digest = DB::KeeperStorage::generateDigest("simuser:simpass");
        auto random_acls = [&]() -> Coordination::ACLs
        {
            if (engine.buggify.acl_torture && engine.rng.chance(0.65))
            {
                Coordination::ACLs out;
                const size_t cnt = engine.rng.rangeSize(1, 3);
                for (size_t i = 0; i < cnt; ++i)
                {
                    const int64_t uid = engine.rng.rangeInt(0, 29);
                    const std::string cred = "u" + std::to_string(uid) + ":p" + std::to_string(uid);
                    const int32_t perms = engine.rng.chance(0.75)
                        ? Coordination::ACL::All
                        : (Coordination::ACL::Read | Coordination::ACL::Write | Coordination::ACL::Create);
                    out.push_back({perms, "digest", DB::KeeperStorage::generateDigest(cred)});
                }
                if (engine.rng.chance(0.45))
                    out.push_back({Coordination::ACL::All, "world", "anyone"});
                return out;
            }
            if (engine.rng.chance(0.20))
                return {{Coordination::ACL::All, "digest", simuser_digest}};
            return {{Coordination::ACL::All, "world", "anyone"}};
        };
        const int64_t kind = engine.rng.rangeInt(0, 16);
        if (kind <= 4)
        {
            /// Create (persistent, ephemeral, sequential, Create2, CreateIfNotExists, CreateContainer, or CreateTTL)
            auto req = std::make_shared<Coordination::ZooKeeperCreateRequest>();
            const bool seq = (kind == 3);
            req->path = seq ? ("/sim/seq-" + std::to_string(engine.rng.rangeInt(0, 1)) + "-") : pickPath();
            req->data = makePayload("v-");
            req->acls = random_acls();
            if (seq)
            {
                req->is_sequential = true;
                req->include_stats = engine.rng.chance(0.30);
            }
            else if (kind == 4)
            {
                const int64_t sub = engine.buggify.mixed_versions ? 0 : engine.rng.rangeInt(0, 2);
                if (sub == 0)
                    req->not_exists = true; // CreateIfNotExists
                else if (sub == 1)
                    req->is_container = true; // CreateContainer
                else
                {
                    req->include_ttl = true; // CreateTTL (short virtual TTL for GC!)
                    req->ttl = engine.rng.rangeInt(1200, 3500);
                }
            }
            else
            {
                req->is_ephemeral = engine.rng.chance(0.35);
                req->include_stats = engine.rng.chance(0.30);
            }
            return req;
        }
        if (kind <= 8)
        {
            /// Set (unconditional or versioned CAS)
            auto req = std::make_shared<Coordination::ZooKeeperSetRequest>();
            req->path = pickPath();
            req->data = makePayload("upd-");
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
        if (kind <= 10)
        {
            /// Remove or TryRemove
            auto req = std::make_shared<Coordination::ZooKeeperRemoveRequest>();
            req->path = pickPath();
            req->version = engine.rng.chance(0.5) ? -1 : static_cast<int32_t>(engine.rng.rangeInt(0, 2));
            req->try_remove = (kind == 10);
            return req;
        }
        if (kind == 11)
        {
            /// RemoveRecursive (with small or generous limit to test both ZNOTEMPTY and deep subtree delete!)
            auto req = std::make_shared<Coordination::ZooKeeperRemoveRecursiveRequest>();
            req->path = "/sim/k" + std::to_string(engine.rng.rangeInt(0, 7));
            req->remove_nodes_limit = static_cast<uint32_t>(engine.rng.rangeInt(1, 8));
            return req;
        }
        if ((kind == 12 || kind == 13) && !for_multi)
        {
            /// SetACL (exercises ACLMap refcounting, aversion CAS, world/digest/auth ACLs, and rollback!)
            auto req = std::make_shared<Coordination::ZooKeeperSetACLRequest>();
            req->path = pickPath();
            req->version = engine.rng.chance(0.55) ? -1 : static_cast<int32_t>(engine.rng.rangeInt(0, 2));
            const int64_t acl_mode = engine.rng.rangeInt(0, 2);
            if (engine.buggify.acl_torture && engine.rng.chance(0.70))
                req->acls = random_acls();
            else if (acl_mode == 0)
                req->acls = {{Coordination::ACL::All, "world", "anyone"}};
            else if (acl_mode == 1)
                req->acls = {{Coordination::ACL::All, "digest", simuser_digest}};
            else
                req->acls = {{Coordination::ACL::All, "auth", ""}};
            return req;
        }
        if (kind == 16 && !for_multi)
        {
            auto req = std::make_shared<Coordination::ZooKeeperSyncRequest>();
            req->path = pickPath();
            engine.coverage.hit("sync_request_submitted");
            return req;
        }
        /// Check, CheckNotExists, or CheckStat
        auto req = std::make_shared<Coordination::ZooKeeperCheckRequest>();
        req->path = pickPath();
        if (kind == 14)
        {
            Coordination::Stat st{};
            st.czxid = -1;
            st.mzxid = -1;
            st.ctime = -1;
            st.mtime = -1;
            st.version = -1;
            st.cversion = -1;
            st.aversion = -1;
            st.ephemeralOwner = -1;
            st.dataLength = -1;
            st.numChildren = -1;
            st.pzxid = -1;
            const auto & tree = cluster.getKeeperOracle().latestTree().nodes;
            auto it = tree.find(req->path);
            if (it != tree.end() && engine.rng.chance(0.7))
            {
                st.version = it->second.version;
                st.aversion = it->second.aversion;
                st.dataLength = static_cast<int32_t>(it->second.data.size());
            }
            else
            {
                st.version = static_cast<int32_t>(engine.rng.rangeInt(0, 2));
                st.numChildren = static_cast<int32_t>(engine.rng.rangeInt(0, 2));
            }
            req->stat_to_check = st;
            req->version = -1;
            return req;
        }
        req->not_exists = (kind >= 15);
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
                multi->requests.push_back(buildSingleWriteRequest(sess, /*for_multi=*/true));
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
        /// Avoid triggering known Bug #133645 (processLocalRequests inserting `{sid, 0}` on a
        /// lagging follower that hasn't committed `SessionID(sid)` yet) unless explicitly testing it.
        const auto & tnode = cluster.getNodes().at(target_node);
        if (!tnode.machine || !cluster.getKeeperOracle().sessionKnownAt(tnode.machine->last_commit_index(), sid))
            return;
        const std::string path = engine.rng.chance(0.3) ? "/sim" : pickPath();

        const int64_t kind = engine.rng.rangeInt(0, 12);
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
        else if (kind == 2)
        {
            auto r = std::make_shared<Coordination::ZooKeeperExistsRequest>();
            r->path = path;
            r->has_watch = true;
            req = r;
        }
        else if (kind == 3)
        {
            auto r = std::make_shared<Coordination::ZooKeeperAddWatchRequest>();
            r->path = path;
            r->mode = engine.rng.chance(0.5)
                ? Coordination::AddWatchRequest::AddWatchMode::PERSISTENT
                : Coordination::AddWatchRequest::AddWatchMode::PERSISTENT_RECURSIVE;
            req = r;
        }
        else if (kind == 4)
        {
            auto r = std::make_shared<Coordination::ZooKeeperListRecursiveRequest>();
            r->path = "/sim";
            r->children_nodes_limit = static_cast<uint32_t>(engine.rng.rangeInt(2, 32));
            req = r;
        }
        else if (kind == 5)
        {
            auto r = std::make_shared<Coordination::ZooKeeperGetACLRequest>();
            r->path = path;
            req = r;
        }
        else if (kind == 6)
        {
            auto r = std::make_shared<Coordination::ZooKeeperListRequest>();
            r->path = "/sim";
            const int64_t lt = engine.rng.rangeInt(0, 2);
            r->list_request_type = (lt == 0) ? Coordination::ListRequestType::ALL
                : (lt == 1 ? Coordination::ListRequestType::PERSISTENT_ONLY : Coordination::ListRequestType::EPHEMERAL_ONLY);
            r->with_stat = engine.rng.chance(0.5);
            r->with_data = engine.rng.chance(0.5);
            r->has_watch = engine.rng.chance(0.5);
            req = r;
        }
        else if (kind == 7)
        {
            auto r = std::make_shared<Coordination::ZooKeeperMultiRequest>();
            r->operation_type = Coordination::ZooKeeperMultiRequest::OperationType::Read;
            auto g1 = std::make_shared<Coordination::ZooKeeperGetRequest>();
            g1->path = path;
            auto e1 = std::make_shared<Coordination::ZooKeeperExistsRequest>();
            e1->path = "/sim";
            auto l1 = std::make_shared<Coordination::ZooKeeperListRequest>();
            l1->path = "/sim";
            r->requests = {g1, e1, l1};
            req = r;
        }
        else if (kind == 8)
        {
            auto r = std::make_shared<Coordination::ZooKeeperCheckWatchRequest>();
            r->path = path;
            r->type = Coordination::CheckWatchRequest::CheckWatchType::ANY;
            req = r;
        }
        else if (kind == 9)
        {
            auto r = std::make_shared<Coordination::ZooKeeperRemoveWatchRequest>();
            r->path = path;
            r->type = Coordination::RemoveWatchRequest::WatchType::ANY;
            req = r;
        }
        else if (kind == 10)
        {
            auto r = std::make_shared<Coordination::ZooKeeperListWithOptionsRequest>();
            r->path = "/sim";
            const int64_t lt = engine.rng.rangeInt(0, 2);
            r->options.filter = (lt == 0) ? Coordination::ListRequestType::ALL
                : (lt == 1 ? Coordination::ListRequestType::PERSISTENT_ONLY : Coordination::ListRequestType::EPHEMERAL_ONLY);
            r->options.with_stat = engine.rng.chance(0.5);
            r->options.with_data = engine.rng.chance(0.5);
            r->options.recursive = engine.rng.chance(0.5);
            r->options.max_results = r->options.recursive ? static_cast<uint32_t>(engine.rng.rangeInt(2, 16)) : 0;
            r->options.shuffle = engine.rng.chance(0.5);
            r->has_watch = !r->options.recursive && engine.rng.chance(0.4);
            engine.coverage.hit("list_with_options_executed");
            req = r;
        }
        else if (kind == 11)
        {
            auto r = std::make_shared<Coordination::ZooKeeperSetWatchesRequest>();
            r->zxid = engine.rng.rangeInt(0, 5);
            r->data_watches = {path};
            r->exist_watches = {pickPath()};
            r->child_watches = {"/sim"};
            engine.coverage.hit("set_watches_executed");
            req = r;
        }
        else
        {
            auto r = std::make_shared<Coordination::ZooKeeperSetWatches2Request>();
            r->zxid = engine.rng.rangeInt(0, 5);
            r->data_watches = {path};
            r->exist_watches = {pickPath()};
            r->child_watches = {"/sim"};
            r->persistent_watches = {path};
            r->persistent_recursive_watches = {"/sim"};
            engine.coverage.hit("set_watches2_executed");
            req = r;
        }
        req->xid = 500000 + static_cast<int64_t>(++reads_executed);
        if (engine.buggify.quorum_reads && engine.rng.chance(0.75))
        {
            const int leader = cluster.findLeader();
            if (leader != 0)
            {
                if (auto res = cluster.submitRequest(leader, req, sid); res && res->get_accepted())
                {
                    ++ops_submitted;
                    engine.coverage.hit("quorum_read_submitted");
                    return;
                }
            }
        }
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
    bool final_barrier_in_flight = false;
    bool lin_reg_created = false;
    bool lin_write_in_flight = false;
    uint64_t lin_seq_counter = 0;
    uint64_t last_acked_lin_val = 0;
    std::map<int64_t, ClientSession> sessions;
    std::vector<std::pair<int64_t, int64_t>> unknown_retry_xids;
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
        if (engine.buggify.enable_backpressure)
        {
            const int leader = cluster.findLeader();
            if (leader != 0)
                cluster.toggleSlowMemberBackpressure(leader, false);
        }
        for (int id : cluster.currentClusterMemberIds())
        {
            if (!cluster.isOnline(id))
                cluster.startNode(id);
        }
        if (!engine.buggify.member_rotation)
        {
            for (int id = 1; id <= cluster.getConfig().num_nodes; ++id)
            {
                if (!cluster.isOnline(id))
                    cluster.startNode(id);
            }
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

        /// Hot reconfig (`request_leadership`, `set_user_ctx`, `update_params`) when enabled!
        if (engine.buggify.priority_torture && engine.rng.chance(0.32))
        {
            cluster.runPriorityTortureAction();
            return;
        }
        if (engine.buggify.hot_reconfig && engine.rng.chance(0.22))
        {
            cluster.runHotReconfigAction();
            return;
        }

        /// Toggle NuRaft slow-member backpressure (`bpon`/`bpoff`) from leader OR follower when enabled!
        if (engine.buggify.enable_backpressure && engine.rng.chance(0.22))
        {
            auto online = cluster.onlineNodes();
            if (!online.empty())
            {
                bp_enabled = !bp_enabled;
                cluster.toggleSlowMemberBackpressure(engine.rng.pick(online), bp_enabled);
                return;
            }
        }

        /// Core-voter membership rotation (`add_srv` voter + `remove_srv` any core voter / old leader)!
        if (engine.buggify.member_rotation && engine.rng.chance(0.26))
        {
            const int leader = cluster.findLeader();
            if (leader != 0)
            {
                cluster.rotateClusterMember(leader);
                return;
            }
        }

        /// Dynamic cluster membership churn (`add_srv` voter/learner + `remove_srv`) when enabled!
        if (engine.buggify.enable_dynamic_membership && engine.rng.chance(0.22))
        {
            const int leader = cluster.findLeader();
            if (leader != 0)
            {
                const int dyn_id = n + 1;
                auto cur_members = cluster.currentClusterMemberIds();
                const bool dyn_in_cfg = std::find(cur_members.begin(), cur_members.end(), dyn_id) != cur_members.end();
                if (!dyn_in_cfg)
                    cluster.addDynamicNode(leader, /*as_learner=*/engine.rng.chance(0.50));
                else
                    cluster.removeDynamicNode(leader);
                return;
            }
        }

        /// Balanced / Chaos / LSMT mixed FDB fault menu:
        if (roll < 26)
            injectNetworkFault(n);
        else if (roll < 50)
        {
            const double pl_prob = engine.buggify.snapshot_on_exit ? 0.35 : 0.75;
            injectNodeCrash(n, quorum, engine.rng.chance(pl_prob) ? KillMode::PowerLoss : KillMode::Graceful);
        }
        else if (roll < 64)
            injectDiskIoFault(n);
        else if (roll < 78)
            injectSnapshotAndWipeFault(n, quorum);
        else if (roll < 86)
            injectSwizzleClog(n);
        else if (roll < 93)
        {
            const int leader = cluster.findLeader();
            if (leader != 0)
                cluster.yieldLeadership(leader);
        }
        else
        {
            const int leader = cluster.findLeader();
            if (leader != 0)
            {
                const int target = static_cast<int>(engine.rng.rangeInt(1, n));
                const int new_prio = static_cast<int>(engine.rng.rangeInt(1, 5));
                int caller = leader;
                if (engine.buggify.auto_forward && engine.rng.chance(0.50))
                {
                    auto online = cluster.onlineNodes();
                    if (!online.empty())
                        caller = engine.rng.pick(online);
                }
                cluster.setNodePriority(caller, target, new_prio);
            }
        }
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
        const int64_t downtime_ms = engine.buggify.snapshot_torture
            ? engine.rng.rangeInt(25, 350)
            : engine.rng.rangeInt(350, 1800);
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

        /// On LSMT-Disk nodes, 50% of the time inject a transient disk fault on `data_disk` to exercise
        /// mid-merge failure, partial-merge resumption (`pickRunsToMerge`), and file-deletion retry!
        if (node.data_disk && engine.rng.chance(0.50))
        {
            static const std::vector<SimCrashDisk::TransientFaultKind> lsmt_faults = {
                SimCrashDisk::TransientFaultKind::Open,
                SimCrashDisk::TransientFaultKind::PartialWrite,
                SimCrashDisk::TransientFaultKind::RemoveError,
            };
            node.data_disk->armTransientFault("", engine.rng.pick(lsmt_faults));
            engine.coverage.hit("lsmt_data_disk_fault_armed");
            (void)n;
            return;
        }

        /// Arm transient snapshot fault (open / partial / sync / marker remove / read / mid-stream read) and trigger a snapshot!
        static const std::vector<std::pair<std::string, SimCrashDisk::TransientFaultKind>> faults = {
            {"snapshot_", SimCrashDisk::TransientFaultKind::Open},
            {"snapshot_", SimCrashDisk::TransientFaultKind::PartialWrite},
            {"snapshot_", SimCrashDisk::TransientFaultKind::SyncError},
            {"tmp_snapshot_", SimCrashDisk::TransientFaultKind::RemoveError},
            {"snapshot_", SimCrashDisk::TransientFaultKind::ReadError},
            {"snapshot_", SimCrashDisk::TransientFaultKind::MidReadError},
        };
        const auto & [prefix, kind] = engine.rng.pick(faults);
        if (engine.buggify.remote_snap_faults && node.latest_snapshot_disk && engine.rng.chance(0.40))
            node.latest_snapshot_disk->armTransientFault(prefix, kind);
        else
            node.snapshot_disk->armTransientFault(prefix, kind);
        if (kind == SimCrashDisk::TransientFaultKind::ReadError || kind == SimCrashDisk::TransientFaultKind::MidReadError)
            engine.coverage.hit("remote_snapshot_read_fault_armed");
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
    bool bp_enabled = false;
};

} // namespace keeper_sim
