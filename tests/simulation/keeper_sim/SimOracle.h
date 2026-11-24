#pragma once

#include <SimClock.h>
#include <SimNetwork.h>

#include <Coordination/KeeperConstants.h>
#include <Coordination/KeeperStateMachine.h>
#include <Coordination/KeeperStorage.h>
#include <Common/ZooKeeper/ZooKeeperCommon.h>

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace keeper_sim
{

/// Continuous inline Raft Consensus Safety Oracle.
/// Checked on every simulation step, RPC response, election, commit, and restart.
class RaftSafetyOracle
{
public:
    struct NodeSnapshot
    {
        int id = 0;
        uint64_t incarnation = 0;
        bool online = false;
        uint64_t term = 0;
        int voted_for = -1;
        bool is_leader = false;
        uint64_t last_log_idx = 0;
        uint64_t durable_idx = 0;
        uint64_t target_commit_idx = 0;
        uint64_t applied_idx = 0;
    };

    struct Stats
    {
        uint64_t state_checks = 0;
        uint64_t elections_observed = 0;
        uint64_t commits_verified = 0;
        uint64_t figure8_violations = 0;
    };

    explicit RaftSafetyOracle(int cluster_size_, bool strict_current_term_commit_, CoverageTracker & coverage_, bool check_leader_completeness_ = true)
        : quorum_size(cluster_size_ / 2 + 1)
        , strict_current_term_commit(strict_current_term_commit_)
        , check_leader_completeness(check_leader_completeness_)
        , coverage(coverage_)
    {
    }

    void onRpcResponse(const SimNetwork::RpcObservation & obs)
    {
        if (obs.msg_type == static_cast<int>(nuraft::msg_type::append_entries_request)
            && obs.accepted
            && obs.resp_term == obs.req_term)
        {
            auto & ack = append_acks[{obs.to, obs.resp_term, obs.from}];
            ack = std::max(ack, obs.end_log_idx);
        }
    }

    void observeNode(
        const NodeSnapshot & snap,
        const std::function<std::optional<std::pair<uint64_t, std::string>>(uint64_t idx)> & read_log_entry)
    {
        ++stats.state_checks;
        if (!snap.online)
            return;

        /// 1. Persisted term monotonicity (even across power-loss restarts).
        auto term_it = max_term_by_node.find(snap.id);
        if (term_it != max_term_by_node.end())
        {
            require(snap.term >= term_it->second,
                "RAFT_SAFETY term-monotonicity violated on node " + std::to_string(snap.id)
                + ": " + std::to_string(term_it->second) + " -> " + std::to_string(snap.term));
        }
        max_term_by_node[snap.id] = snap.term;

        /// 2. One vote per term per node (persisted across crashes).
        if (snap.voted_for != -1 && snap.term > 0)
        {
            const auto key = std::make_pair(snap.id, snap.term);
            auto vit = persisted_votes.find(key);
            if (vit != persisted_votes.end())
            {
                require(vit->second == snap.voted_for,
                    "RAFT_SAFETY one-vote-per-term violated on node " + std::to_string(snap.id)
                    + " term " + std::to_string(snap.term)
                    + ": voted for " + std::to_string(vit->second) + " and " + std::to_string(snap.voted_for));
            }
            else
            {
                persisted_votes[key] = snap.voted_for;
            }
        }

        /// 3. Election safety: at most one leader per term.
        if (snap.is_leader)
        {
            auto lit = leaders_by_term.find(snap.term);
            if (lit != leaders_by_term.end())
            {
                require(lit->second == snap.id,
                    "RAFT_SAFETY election-safety violated in term " + std::to_string(snap.term)
                    + ": leaders " + std::to_string(lit->second) + " and " + std::to_string(snap.id));
            }
            else
            {
                if (!leaders_by_term.empty())
                    coverage.hit("leader_failover");
                leaders_by_term[snap.term] = snap.id;
                ++stats.elections_observed;
                coverage.hit("leader_election");
            }

            /// 3b. Leader Completeness (Raft §5.4.1): every leader elected in a newer term
            /// MUST contain all previously committed log indices (either in its log or covered
            /// by its snapshot). We require `snap.term > high_entry_term` and `snap.term >= highest_leader_term`
            /// so a stale partitioned leader from an older term that hasn't stepped down yet does not
            /// falsely trigger against commits made by a newer leader.
            if (check_leader_completeness && !committed_entries.empty())
            {
                const auto & [high_commit, high_val] = *committed_entries.rbegin();
                const uint64_t high_entry_term = high_val.first;
                const uint64_t highest_leader_term = leaders_by_term.empty() ? 0 : leaders_by_term.rbegin()->first;
                if (snap.term > high_entry_term && snap.term >= highest_leader_term)
                {
                    require(snap.last_log_idx >= high_commit,
                        "RAFT_SAFETY leader-completeness violated: node " + std::to_string(snap.id)
                        + " elected leader in term " + std::to_string(snap.term)
                        + " with last_log_idx=" + std::to_string(snap.last_log_idx)
                        + " < highest_committed=" + std::to_string(high_commit)
                        + " (committed_entry_term=" + std::to_string(high_entry_term) + ")");
                }
            }
        }

        /// 4. Index ordering invariants.
        require(snap.applied_idx <= snap.target_commit_idx || snap.applied_idx <= snap.last_log_idx,
            "RAFT_SAFETY applied > target_commit on node " + std::to_string(snap.id));

        /// 5. Log Matching on recent/tip log entries (same index + term => identical bytes).
        const uint64_t scan_start = snap.last_log_idx > 12 ? snap.last_log_idx - 12 : 1;
        for (uint64_t idx = scan_start; idx <= snap.last_log_idx; ++idx)
        {
            auto entry = read_log_entry(idx);
            if (!entry)
                continue;
            const auto & [entry_term, entry_bytes] = *entry;
            const auto key = std::make_pair(idx, entry_term);
            auto it = log_by_index_term.find(key);
            if (it != log_by_index_term.end())
            {
                require(it->second == entry_bytes,
                    "RAFT_SAFETY log-matching violated at idx=" + std::to_string(idx)
                    + " term=" + std::to_string(entry_term) + " on node " + std::to_string(snap.id));
            }
            else
            {
                log_by_index_term.emplace(key, entry_bytes);
            }
        }

        /// Track durable log entries per node so we can compute exact Raft current-term
        /// quorum certificates (independent of when `target_commit_idx` updates).
        auto & node_dur_terms = durable_terms_by_node[snap.id];
        const uint64_t dur_scan_start = snap.durable_idx > 24 ? snap.durable_idx - 24 : 1;
        for (uint64_t idx = dur_scan_start; idx <= snap.durable_idx; ++idx)
        {
            if (auto entry = read_log_entry(idx))
                node_dur_terms[idx] = entry->first;
        }
        /// Certify any entry whose term equals the current leader's term and is durably
        /// replicated on a strict majority of cluster voters (Raft §5.4.2).
        if (snap.is_leader)
        {
            for (uint64_t idx = dur_scan_start; idx <= snap.durable_idx; ++idx)
            {
                auto tip = read_log_entry(idx);
                if (!tip || tip->first != snap.term)
                    continue;
                int durable_copies = 0;
                for (const auto & [peer_id, peer_terms] : durable_terms_by_node)
                {
                    auto pit = peer_terms.find(idx);
                    if (pit != peer_terms.end() && pit->second == snap.term)
                        ++durable_copies;
                }
                if (durable_copies >= quorum_size)
                {
                    for (uint64_t ci = std::max<uint64_t>(1, certified_high_water + 1); ci <= idx; ++ci)
                        certified_committed.insert(ci);
                    certified_high_water = std::max(certified_high_water, idx);
                }
            }
        }

        /// 6. Strict current-term commitment certificate check (exact Raft Figure 8 safety rule).
        const uint64_t prev_commit = prev_target_commit[snap.id];
        if (snap.target_commit_idx > prev_commit)
        {
            if (snap.is_leader)
            {
                auto tip = read_log_entry(snap.target_commit_idx);
                if (tip)
                {
                    const uint64_t entry_term = tip->first;
                    if (entry_term == snap.term)
                    {
                        for (uint64_t ci = prev_commit + 1; ci <= snap.target_commit_idx; ++ci)
                            certified_committed.insert(ci);
                        certified_high_water = std::max(certified_high_water, snap.target_commit_idx);
                    }
                    else if (!certified_committed.contains(snap.target_commit_idx)
                        && snap.target_commit_idx > certified_high_water)
                    {
                        ++stats.figure8_violations;
                        coverage.hit("raft_figure8_detected");
                        if (strict_current_term_commit)
                        {
                            require(false,
                                "RAFT_SAFETY FIGURE8 current-term-commit violated by leader "
                                + std::to_string(snap.id) + " in term " + std::to_string(snap.term)
                                + ": advanced commit " + std::to_string(prev_commit) + "->"
                                + std::to_string(snap.target_commit_idx)
                                + " with old-term entry term=" + std::to_string(entry_term));
                        }
                    }
                }
            }
            prev_target_commit[snap.id] = snap.target_commit_idx;
        }
    }

    /// Invoked whenever any node commits (applies) a log entry at `log_idx`.
    void verifyCommittedEntry(int node_id, uint64_t log_idx, uint64_t term, const std::string & serialized_bytes)
    {
        ++stats.commits_verified;
        auto it = committed_entries.find(log_idx);
        if (it != committed_entries.end())
        {
            require(it->second.first == term && it->second.second == serialized_bytes,
                "RAFT_SAFETY state-machine-safety violated at log_idx=" + std::to_string(log_idx)
                + " on node " + std::to_string(node_id)
                + " (term " + std::to_string(term) + " vs " + std::to_string(it->second.first) + ")");
        }
        else
        {
            committed_entries.emplace(log_idx, std::make_pair(term, serialized_bytes));
            certified_committed.insert(log_idx);
        }
    }

    /// Verify rollback safety:
    ///   1. A node may never roll back an entry at or below its own applied commit index.
    ///   2. A node may never roll back an entry whose (index, term) was already committed in the cluster.
    ///   (Rolling back an uncommitted conflicting old-term entry so a returning node can accept the
    ///    new majority's committed entry at the same index is valid Raft conflict resolution.)
    void onRollback(int node_id, uint64_t log_idx, uint64_t rolled_back_term, uint64_t node_last_commit_idx)
    {
        require(log_idx > node_last_commit_idx,
            "RAFT_SAFETY node " + std::to_string(node_id) + " rolled back already-applied log_idx="
            + std::to_string(log_idx) + " <= last_commit=" + std::to_string(node_last_commit_idx));
        auto it = committed_entries.find(log_idx);
        if (it != committed_entries.end() && rolled_back_term != 0)
        {
            require(it->second.first != rolled_back_term,
                "RAFT_SAFETY committed (log_idx=" + std::to_string(log_idx)
                + ", term=" + std::to_string(rolled_back_term) + ") rolled back on node " + std::to_string(node_id));
        }
        coverage.hit("log_rollback");
    }

    /// Reset per-node volatile state after a disk wipe. When `state_wiped` is false (e.g.
    /// `WipeAndRejoin` preserving `state_disk` voted_for/term), keep persisted vote/term checks intact.
    void onNodeDiskWiped(int node_id, bool state_wiped = true)
    {
        prev_target_commit.erase(node_id);
        durable_terms_by_node.erase(node_id);
        if (state_wiped)
        {
            max_term_by_node.erase(node_id);
            for (auto it = persisted_votes.begin(); it != persisted_votes.end();)
            {
                if (it->first.first == node_id)
                    it = persisted_votes.erase(it);
                else
                    ++it;
            }
        }
    }

    const Stats & getStats() const { return stats; }
    uint64_t highestCommittedIndex() const { return committed_entries.empty() ? 0 : committed_entries.rbegin()->first; }

private:
    int quorum_size;
    bool strict_current_term_commit;
    bool check_leader_completeness;
    CoverageTracker & coverage;
    std::map<int, uint64_t> max_term_by_node;
    std::map<std::pair<int, uint64_t>, int> persisted_votes;
    std::map<uint64_t, int> leaders_by_term;
    std::map<std::pair<uint64_t, uint64_t>, std::string> log_by_index_term;
    std::map<uint64_t, std::pair<uint64_t, std::string>> committed_entries;
    std::set<uint64_t> certified_committed;
    uint64_t certified_high_water = 0;
    std::map<int, std::map<uint64_t, uint64_t>> durable_terms_by_node;
    std::map<int, uint64_t> prev_target_commit;
    std::map<std::tuple<int, uint64_t, int>, uint64_t> append_acks;
    Stats stats;
};

/// Independent ZooKeeper/Keeper Semantic, Digest, Ephemeral, Watch & Linearizability Oracle.
///
/// Independently models every committed ZooKeeper request in pure C++ (never trusting
/// KeeperStorage's internal state) and verifies:
///   1. Exact response error codes and Multi sub-response error codes on every replica.
///   2. Exact cross-replica `KeeperStateMachine::getNodesDigest()` equality at every log index.
///   3. Deep tree comparison (existence, data, version, ephemeralOwner, sorted children)
///      at any replica's `last_commit_index` — even while lagging behind the leader!
///   4. Atomic all-or-nothing Multi transactions and rollback isolation.
///   5. Ephemeral node cleanup upon session close / virtual-time session expiry.
///   6. Sequential node creation monotonic numbering.
///   7. Client linearizability: every client-acknowledged write is permanently durable
///      and never lost across failovers, partitions, power-loss crashes, or snapshots.
class KeeperSemanticOracle
{
public:
    struct ModelNode
    {
        std::string data;
        int32_t version = 0;
        int32_t aversion = 0;
        int64_t ephemeral_owner = 0;
        int64_t seq_num = 0;
        bool is_ephemeral = false;
        bool is_container = false;
        bool is_ttl = false;
        int64_t ttl_ms = 0;
        int64_t mtime = 0;
        std::vector<Coordination::ACL> acls;
    };

    struct TreeState
    {
        std::map<std::string, ModelNode> nodes;
        std::set<int64_t> active_sessions;
        std::map<int64_t, std::set<std::pair<std::string, std::string>>> session_auths;
        int64_t next_session_id = 1;
        std::set<std::string> known_paths;

        TreeState()
        {
            nodes["/"] = ModelNode{};
            known_paths.insert("/");
        }
    };

    struct ExpectedOutcome
    {
        int error = 0;
        int alt_error = 0; // Optional second valid error code (e.g. RemoveRecursive hash-order ZNOAUTH vs ZNOTEMPTY)
        bool is_multi = false;
        std::vector<int> sub_errors;
        std::vector<int> sub_alt_errors;
        std::string created_path;
        int64_t allocated_session_id = 0;
    };

    enum class ClientOpStatus
    {
        InFlight,
        Acked,
        Unknown,
    };

    struct ClientOpRecord
    {
        int64_t session_id = 0;
        int64_t xid = 0;
        int64_t invoke_ns = 0;
        int64_t complete_ns = 0;
        ClientOpStatus status = ClientOpStatus::InFlight;
        bool committed = false;
    };

    struct Stats
    {
        uint64_t model_commits = 0;
        uint64_t replica_responses_checked = 0;
        uint64_t digests_checked = 0;
        uint64_t deep_tree_checks = 0;
        uint64_t client_ops_acked = 0;
        uint64_t client_ops_unknown = 0;
        uint64_t watches_delivered = 0;
        uint64_t multi_committed = 0;
        uint64_t multi_rolled_back = 0;
        uint64_t sessions_closed = 0;
        uint64_t sequential_created = 0;
    };

    explicit KeeperSemanticOracle(CoverageTracker & coverage_)
        : coverage(coverage_)
    {
        tree_by_index[0] = current_tree;
    }

    void recordClientInvoke(int64_t session_id, int64_t xid, int64_t now_ns)
    {
        if (auto it = client_ops.find({session_id, xid}); it != client_ops.end() && it->second.status == ClientOpStatus::Acked)
            return;
        client_ops[{session_id, xid}] = ClientOpRecord{session_id, xid, now_ns, 0, ClientOpStatus::InFlight, false};
    }

    void recordClientOutcome(int64_t session_id, int64_t xid, bool acked, int64_t now_ns)
    {
        auto it = client_ops.find({session_id, xid});
        if (it == client_ops.end())
            return;
        it->second.complete_ns = now_ns;
        if (acked)
        {
            it->second.status = ClientOpStatus::Acked;
            ++stats.client_ops_acked;
        }
        else
        {
            if (it->second.status == ClientOpStatus::InFlight)
            {
                it->second.status = ClientOpStatus::Unknown;
                ++stats.client_ops_unknown;
            }
        }
    }

    void recordWatchDelivered(int /*node_id*/, int64_t /*session_id*/, const std::string & /*path*/)
    {
        ++stats.watches_delivered;
        coverage.hit("watch_triggered");
    }

    /// Record a config commit index so `tree_by_index` stays continuous across Raft config entries.
    void onConfigCommit(uint64_t log_idx)
    {
        if (log_idx > highest_modeled_idx)
        {
            highest_modeled_idx = log_idx;
            tree_by_index[log_idx] = current_tree;
        }
        else if (!tree_by_index.contains(log_idx))
        {
            auto it = tree_by_index.lower_bound(log_idx);
            if (it != tree_by_index.begin())
                tree_by_index[log_idx] = std::prev(it)->second;
            else
                tree_by_index[log_idx] = TreeState{};
        }
    }

    /// Called immediately after `KeeperStateMachine::commit` applies `request` at `(log_idx, sub_idx)` on `node_id`.
    void onKeeperCommit(
        int node_id,
        uint64_t log_idx,
        uint32_t sub_idx,
        bool is_last_in_batch,
        const DB::KeeperRequestForSession & req,
        const Coordination::ZooKeeperResponsePtr & actual_response,
        uint64_t actual_digest)
    {
        const auto key = std::make_pair(log_idx, sub_idx);
        /// Advance independent reference model on the first replica to reach `(log_idx, sub_idx)`.
        if (!expected_by_key.contains(key))
        {
            require(key > highest_modeled_key,
                "KEEPER_ORACLE out-of-order first commit: log_idx=" + std::to_string(log_idx)
                + " sub=" + std::to_string(sub_idx)
                + " <= highest=(" + std::to_string(highest_modeled_key.first) + "," + std::to_string(highest_modeled_key.second) + ")");
            ExpectedOutcome expected = applyToModel(current_tree, req);
            expected_by_key[key] = expected;
            tree_by_index[log_idx] = current_tree;
            highest_modeled_key = key;
            highest_modeled_idx = std::max(highest_modeled_idx, log_idx);
            ++stats.model_commits;

            /// Mark client op committed for linearizability/durability tracking.
            if (req.request)
            {
                auto cit = client_ops.find({req.session_id, req.request->xid});
                if (cit != client_ops.end())
                    cit->second.committed = true;
            }
        }

        /// Verify exact cross-replica KeeperDigest at `(log_idx, sub_idx)` and at final `log_idx`.
        verifySubDigest(node_id, key, actual_digest);
        if (is_last_in_batch)
            verifyDigest(node_id, log_idx, actual_digest);

        /// Verify actual Keeper response against independent model.
        if (actual_response && req.request)
        {
            const auto op = req.request->getOpNum();
            const bool is_read_op = (op == Coordination::OpNum::Get || op == Coordination::OpNum::Exists
                || op == Coordination::OpNum::GetACL || op == Coordination::OpNum::List
                || op == Coordination::OpNum::SimpleList || op == Coordination::OpNum::FilteredList
                || op == Coordination::OpNum::FilteredListWithStatsAndData
                || op == Coordination::OpNum::ListWithOptions || op == Coordination::OpNum::MultiRead);
            const bool is_watch_or_rec_op = (op == Coordination::OpNum::ListRecursive
                || op == Coordination::OpNum::AddWatch || op == Coordination::OpNum::CheckWatch
                || op == Coordination::OpNum::RemoveWatch || op == Coordination::OpNum::SetWatch
                || op == Coordination::OpNum::SetWatch2);
            if (is_read_op)
            {
                ++stats.replica_responses_checked;
                verifyLocalReadResponse(node_id, log_idx, req.session_id, *req.request, *actual_response);
                coverage.hit("quorum_read_verified");
                return;
            }
            if (is_watch_or_rec_op)
            {
                ++stats.replica_responses_checked;
                coverage.hit("quorum_watch_committed");
                return;
            }

            const auto & expected = expected_by_key.at(key);
            ++stats.replica_responses_checked;
            const int actual_err = static_cast<int>(actual_response->error);
            require(actual_err == expected.error || (expected.alt_error != 0 && actual_err == expected.alt_error),
                "KEEPER_ORACLE response error mismatch on node " + std::to_string(node_id)
                + " log_idx=" + std::to_string(log_idx)
                + " sub=" + std::to_string(sub_idx)
                + " xid=" + std::to_string(req.request->xid)
                + ": actual=" + std::to_string(actual_err)
                + " expected=" + std::to_string(expected.error));

            if (expected.is_multi)
            {
                auto multi_resp = std::dynamic_pointer_cast<Coordination::ZooKeeperMultiResponse>(actual_response);
                require(multi_resp != nullptr, "KEEPER_ORACLE expected MultiResponse");
                require(multi_resp->responses.size() == expected.sub_errors.size(),
                    "KEEPER_ORACLE multi subresponse count mismatch at log_idx=" + std::to_string(log_idx));
                for (size_t i = 0; i < expected.sub_errors.size(); ++i)
                {
                    const int actual_sub = static_cast<int>(multi_resp->responses[i]->error);
                    const int alt_sub = (i < expected.sub_alt_errors.size()) ? expected.sub_alt_errors[i] : 0;
                    require(actual_sub == expected.sub_errors[i] || (alt_sub != 0 && actual_sub == alt_sub),
                        "KEEPER_ORACLE multi subresponse[" + std::to_string(i) + "] mismatch on node "
                        + std::to_string(node_id) + " log_idx=" + std::to_string(log_idx)
                        + ": actual=" + std::to_string(actual_sub)
                        + " expected=" + std::to_string(expected.sub_errors[i]));
                }
            }
            if (!expected.created_path.empty() && expected.error == 0)
            {
                if (auto create_resp = std::dynamic_pointer_cast<Coordination::ZooKeeperCreateResponse>(actual_response))
                {
                    require(create_resp->path_created == expected.created_path,
                        "KEEPER_ORACLE sequential path mismatch on node " + std::to_string(node_id)
                        + ": actual=" + create_resp->path_created + " expected=" + expected.created_path);
                }
            }
            if (expected.allocated_session_id > 0)
            {
                if (auto sess_resp = std::dynamic_pointer_cast<Coordination::ZooKeeperSessionIDResponse>(actual_response))
                {
                    require(sess_resp->session_id == expected.allocated_session_id,
                        "KEEPER_ORACLE allocated session_id mismatch on node " + std::to_string(node_id)
                        + ": actual=" + std::to_string(sess_resp->session_id)
                        + " expected=" + std::to_string(expected.allocated_session_id));
                }
            }
        }
    }

    void verifySubDigest(int node_id, std::pair<uint64_t, uint32_t> key, uint64_t digest_value)
    {
        if (key.first == 0)
            return;
        ++stats.digests_checked;
        auto it = digests_by_key.find(key);
        if (it != digests_by_key.end())
        {
            require(it->second == digest_value,
                "KEEPER_ORACLE digest mismatch across replicas at log_idx=" + std::to_string(key.first)
                + " sub=" + std::to_string(key.second)
                + " on node " + std::to_string(node_id)
                + ": actual=" + std::to_string(digest_value)
                + " expected=" + std::to_string(it->second));
        }
        else
        {
            digests_by_key[key] = digest_value;
        }
    }

    void verifyDigest(int node_id, uint64_t log_idx, uint64_t digest_value)
    {
        if (log_idx == 0)
            return;
        ++stats.digests_checked;
        auto it = digests_by_index.find(log_idx);
        if (it != digests_by_index.end())
        {
            require(it->second == digest_value,
                "KEEPER_ORACLE digest mismatch across replicas at log_idx=" + std::to_string(log_idx)
                + " on node " + std::to_string(node_id)
                + ": actual=" + std::to_string(digest_value)
                + " expected=" + std::to_string(it->second));
        }
        else
        {
            digests_by_index[log_idx] = digest_value;
        }
    }

    /// Deeply inspect a live `KeeperStateMachine` against the independent reference tree
    /// at that replica's exact `last_commit_index()`. Works even when the replica is lagging!
    void verifyNodeTree(int node_id, DB::KeeperStateMachine & machine)
    {
        const uint64_t commit_idx = machine.last_commit_index();
        if (commit_idx == 0)
            return;

        auto it = tree_by_index.find(commit_idx);
        if (it == tree_by_index.end())
        {
            /// If `commit_idx` is a config entry not yet mapped, fall back to nearest <= commit_idx.
            it = tree_by_index.upper_bound(commit_idx);
            if (it == tree_by_index.begin())
                return;
            --it;
        }
        const TreeState & expected_tree = it->second;
        ++stats.deep_tree_checks;

        /// Also check committed digest if we have recorded one for this exact commit index.
        if (digests_by_index.contains(commit_idx))
            verifyDigest(node_id, commit_idx, machine.getStorageUnsafe().getNodesDigest(/*committed=*/true, /*lock_transaction_mutex=*/true).value);

        auto & storage = machine.getStorageUnsafe();
        std::shared_lock lock(storage.storage_mutex);

        /// Verify active sessions consistency: no ghost timeout==0 sessions resurrected after Close!
        {
            const auto actual_sessions = storage.getActiveSessions();
            for (const auto & [sid, timeout_ms] : actual_sessions)
            {
                require(timeout_ms > 0,
                    "KEEPER_ORACLE ghost session resurrected with timeout=0 on node " + std::to_string(node_id)
                    + " sid=" + std::to_string(sid) + " commit_idx=" + std::to_string(commit_idx));
                require(expected_tree.active_sessions.contains(sid),
                    "KEEPER_ORACLE unexpected active session on node " + std::to_string(node_id)
                    + " sid=" + std::to_string(sid) + " timeout=" + std::to_string(timeout_ms)
                    + " commit_idx=" + std::to_string(commit_idx));
            }
            /// Check ephemeral/TTL/container/watch-map internal invariants on every replica!
            {
                size_t expected_ephemerals = 0;
                std::set<std::string> expected_ttls;
                std::set<std::string> expected_containers;
                for (const auto & [p, mn] : expected_tree.nodes)
                {
                    if (mn.is_ephemeral)
                        ++expected_ephemerals;
                    if (mn.is_ttl)
                        expected_ttls.insert(p);
                    if (mn.is_container)
                        expected_containers.insert(p);
                }

                size_t actual_ephemerals_sum = 0;
                for (const auto & [esid, epaths] : storage.committed_ephemerals)
                {
                    require(!epaths.empty(),
                        "KEEPER_ORACLE empty committed_ephemerals bucket on node " + std::to_string(node_id)
                        + " sid=" + std::to_string(esid));
                    require(expected_tree.active_sessions.contains(esid),
                        "KEEPER_ORACLE closed session left in committed_ephemerals on node " + std::to_string(node_id)
                        + " sid=" + std::to_string(esid));
                    actual_ephemerals_sum += epaths.size();
                }
                require(actual_ephemerals_sum == storage.committed_ephemeral_nodes,
                    "KEEPER_ORACLE committed_ephemeral_nodes counter mismatch on node " + std::to_string(node_id)
                    + ": counter=" + std::to_string(storage.committed_ephemeral_nodes)
                    + " actual=" + std::to_string(actual_ephemerals_sum));
                require(actual_ephemerals_sum == expected_ephemerals,
                    "KEEPER_ORACLE ephemeral count mismatch on node " + std::to_string(node_id)
                    + ": actual=" + std::to_string(actual_ephemerals_sum)
                    + " expected=" + std::to_string(expected_ephemerals));

                std::set<std::string> actual_ttls(storage.ttl_paths.begin(), storage.ttl_paths.end());
                require(actual_ttls == expected_ttls,
                    "KEEPER_ORACLE ttl_paths mismatch on node " + std::to_string(node_id)
                    + ": actual=" + std::to_string(actual_ttls.size())
                    + " expected=" + std::to_string(expected_ttls.size()));

                std::set<std::string> actual_containers(storage.container_paths.begin(), storage.container_paths.end());
                require(actual_containers == expected_containers,
                    "KEEPER_ORACLE container_paths mismatch on node " + std::to_string(node_id)
                    + ": actual=" + std::to_string(actual_containers.size())
                    + " expected=" + std::to_string(expected_containers.size()));

                /// Watch-map bijection & total_watches_count integrity check!
                size_t map_watch_sum = 0;
                auto check_watch_map = [&](const auto & wmap, DB::KeeperStorage::WatchType wtype, const char * mname)
                {
                    for (const auto & [wpath, wset] : wmap)
                    {
                        require(!wset.empty(),
                            std::string("KEEPER_ORACLE empty watch bucket in ") + mname
                            + " on node " + std::to_string(node_id) + " path=" + wpath);
                        for (const auto & entry : wset)
                        {
                            int64_t wsid = 0;
                            if constexpr (std::is_same_v<std::decay_t<decltype(entry)>, int64_t>)
                                wsid = entry;
                            else
                                wsid = entry.first;
                            ++map_watch_sum;
                            auto sit = storage.sessions_and_watchers.find(wsid);
                            require(sit != storage.sessions_and_watchers.end()
                                && sit->second.contains(DB::KeeperStorage::WatchInfo{.path = wpath, .type = wtype}),
                                std::string("KEEPER_ORACLE missing reverse entry in sessions_and_watchers for ") + mname
                                + " on node " + std::to_string(node_id) + " sid=" + std::to_string(wsid) + " path=" + wpath);
                        }
                    }
                };
                check_watch_map(storage.watches, DB::KeeperStorage::WatchType::WATCH, "watches");
                check_watch_map(storage.list_watches, DB::KeeperStorage::WatchType::LIST_WATCH, "list_watches");
                check_watch_map(storage.list_with_data_watches, DB::KeeperStorage::WatchType::LIST_WITH_DATA_WATCH, "list_with_data_watches");
                check_watch_map(storage.persistent_watches, DB::KeeperStorage::WatchType::PERSISTENT_WATCH, "persistent_watches");
                check_watch_map(storage.persistent_list_watches, DB::KeeperStorage::WatchType::PERSISTENT_LIST_WATCH, "persistent_list_watches");
                check_watch_map(storage.persistent_recursive_watches, DB::KeeperStorage::WatchType::PERSISTENT_RECURSIVE_WATCH, "persistent_recursive_watches");

                size_t sess_watch_sum = 0;
                for (const auto & [wsid, wset] : storage.sessions_and_watchers)
                {
                    if (!wset.empty())
                        require(expected_tree.active_sessions.contains(wsid),
                            "KEEPER_ORACLE closed session still has active watches on node " + std::to_string(node_id)
                            + " sid=" + std::to_string(wsid));
                    sess_watch_sum += wset.size();
                }
                require(map_watch_sum == sess_watch_sum && map_watch_sum == storage.getTotalWatchesCount(),
                    "KEEPER_ORACLE watch count mismatch on node " + std::to_string(node_id)
                    + ": maps=" + std::to_string(map_watch_sum)
                    + " sessions_and_watchers=" + std::to_string(sess_watch_sum)
                    + " total_watches_count=" + std::to_string(storage.getTotalWatchesCount()));
            }
        }

        /// Check every known user path under `/sim` for exact existence, data, version, ephemeralOwner, and children!
        for (const auto & path : expected_tree.known_paths)
        {
            if (path != "/sim" && !path.starts_with("/sim/"))
                continue;

            DB::KeeperNodeStats actual_stats{};
            std::string actual_data;
            const bool actual_exists = storage.nodes_storage->getCommittedNodeSimple(path, &actual_stats, &actual_data);
            auto model_it = expected_tree.nodes.find(path);
            const bool expected_exists = (model_it != expected_tree.nodes.end());

            require(actual_exists == expected_exists,
                "KEEPER_ORACLE node existence mismatch on node " + std::to_string(node_id)
                + " at commit_idx=" + std::to_string(commit_idx) + " path=" + path
                + " actual=" + std::to_string(actual_exists) + " expected=" + std::to_string(expected_exists));

            if (expected_exists)
            {
                const ModelNode & expected_node = model_it->second;
                require(actual_data == expected_node.data,
                    "KEEPER_ORACLE data mismatch on node " + std::to_string(node_id)
                    + " path=" + path + " commit_idx=" + std::to_string(commit_idx));
                require(actual_stats.version == expected_node.version,
                    "KEEPER_ORACLE version mismatch on node " + std::to_string(node_id)
                    + " path=" + path + " actual=" + std::to_string(actual_stats.version)
                    + " expected=" + std::to_string(expected_node.version));
                require(actual_stats.aversion == expected_node.aversion,
                    "KEEPER_ORACLE aversion mismatch on node " + std::to_string(node_id)
                    + " path=" + path + " actual=" + std::to_string(actual_stats.aversion)
                    + " expected=" + std::to_string(expected_node.aversion));
                require(actual_stats.getEphemeralOwner() == expected_node.ephemeral_owner,
                    "KEEPER_ORACLE ephemeralOwner mismatch on node " + std::to_string(node_id)
                    + " path=" + path + " actual=" + std::to_string(actual_stats.getEphemeralOwner())
                    + " expected=" + std::to_string(expected_node.ephemeral_owner));

                /// Ephemeral-session consistency: an ephemeral node's owner MUST be an active session!
                if (expected_node.is_ephemeral)
                {
                    require(expected_tree.active_sessions.contains(expected_node.ephemeral_owner),
                        "KEEPER_ORACLE orphaned ephemeral on node " + std::to_string(node_id)
                        + " path=" + path + " owner=" + std::to_string(expected_node.ephemeral_owner));
                }

                /// Verify exact sorted child list.
                auto actual_children = storage.nodes_storage->listCommittedChildrenNames(path);
                std::sort(actual_children.begin(), actual_children.end());
                auto expected_children = listChildren(expected_tree, path);
                require(actual_children == expected_children,
                    "KEEPER_ORACLE children mismatch on node " + std::to_string(node_id)
                    + " path=" + path + " commit_idx=" + std::to_string(commit_idx));
            }
        }
    }

    /// Final linearizability & durability invariant: every client-acknowledged write
    /// MUST be present in the final committed Raft log.
    void verifyAllAckedCommitted() const
    {
        for (const auto & [key, rec] : client_ops)
        {
            if (rec.status == ClientOpStatus::Acked)
            {
                require(rec.committed,
                    "KEEPER_ORACLE LINEARIZABILITY VIOLATION: client-acked op session="
                    + std::to_string(rec.session_id) + " xid=" + std::to_string(rec.xid)
                    + " was LOST from committed history!");
            }
        }
    }

    bool sessionKnownAt(uint64_t commit_idx, int64_t sid) const
    {
        if (commit_idx == 0)
            return false;
        auto it = tree_by_index.upper_bound(commit_idx);
        if (it == tree_by_index.begin())
            return false;
        --it;
        return it->second.active_sessions.contains(sid);
    }

    /// Differential local read oracle: verifies `Get`, `Exists`, `List`/`FilteredList`, `ListRecursive`,
    /// `ListWithOptions`, `GetACL`, and `MultiRead` responses on ANY replica (Mem, LSMT-Mem, LSMT-Disk)
    /// against the independent reference model at that node's exact `commit_idx`!
    void verifyLocalReadResponse(
        int node_id,
        uint64_t commit_idx,
        int64_t session_id,
        const Coordination::ZooKeeperRequest & zk_req,
        const Coordination::Response & resp)
    {
        if (commit_idx == 0)
            return;
        auto it = tree_by_index.upper_bound(commit_idx);
        if (it == tree_by_index.begin())
            return;
        --it;
        const TreeState & state = it->second;
        using Coordination::Error;

        auto check_err = [&](int actual, int expected, const char * op_name)
        {
            require(actual == expected,
                std::string("KEEPER_READ_ORACLE ") + op_name + " error mismatch on node " + std::to_string(node_id)
                + " commit_idx=" + std::to_string(commit_idx) + " path=" + zk_req.getPath()
                + " actual=" + std::to_string(actual) + " expected=" + std::to_string(expected));
        };

        switch (zk_req.getOpNum())
        {
            case Coordination::OpNum::Get:
            {
                const auto & req = dynamic_cast<const Coordination::ZooKeeperGetRequest &>(zk_req);
                const auto & r = dynamic_cast<const Coordination::GetResponse &>(resp);
                auto nit = state.nodes.find(req.path);
                if (nit == state.nodes.end())
                {
                    check_err(static_cast<int>(r.error), static_cast<int>(Error::ZNONODE), "Get");
                }
                else if (!checkACL(state, nit->second.acls, Coordination::ACL::Read, session_id))
                {
                    check_err(static_cast<int>(r.error), static_cast<int>(Error::ZNOAUTH), "Get");
                }
                else
                {
                    check_err(static_cast<int>(r.error), static_cast<int>(Error::ZOK), "Get");
                    require(r.data == nit->second.data && r.stat.version == nit->second.version
                        && r.stat.aversion == nit->second.aversion && r.stat.ephemeralOwner == nit->second.ephemeral_owner,
                        "KEEPER_READ_ORACLE Get data/stat mismatch on node " + std::to_string(node_id)
                        + " path=" + req.path + " commit_idx=" + std::to_string(commit_idx));
                }
                coverage.hit("local_read_verified");
                break;
            }
            case Coordination::OpNum::Exists:
            {
                const auto & req = dynamic_cast<const Coordination::ZooKeeperExistsRequest &>(zk_req);
                const auto & r = dynamic_cast<const Coordination::ExistsResponse &>(resp);
                auto nit = state.nodes.find(req.path);
                if (nit == state.nodes.end())
                    check_err(static_cast<int>(r.error), static_cast<int>(Error::ZNONODE), "Exists");
                else
                {
                    check_err(static_cast<int>(r.error), static_cast<int>(Error::ZOK), "Exists");
                    require(r.stat.version == nit->second.version && r.stat.ephemeralOwner == nit->second.ephemeral_owner,
                        "KEEPER_READ_ORACLE Exists stat mismatch on node " + std::to_string(node_id) + " path=" + req.path);
                }
                coverage.hit("local_read_verified");
                break;
            }
            case Coordination::OpNum::GetACL:
            {
                const auto & req = dynamic_cast<const Coordination::ZooKeeperGetACLRequest &>(zk_req);
                const auto & r = dynamic_cast<const Coordination::GetACLResponse &>(resp);
                auto nit = state.nodes.find(req.path);
                if (nit == state.nodes.end())
                    check_err(static_cast<int>(r.error), static_cast<int>(Error::ZNONODE), "GetACL");
                else if (!checkACL(state, nit->second.acls, Coordination::ACL::Admin | Coordination::ACL::Read, session_id))
                    check_err(static_cast<int>(r.error), static_cast<int>(Error::ZNOAUTH), "GetACL");
                else
                {
                    check_err(static_cast<int>(r.error), static_cast<int>(Error::ZOK), "GetACL");
                    require(r.stat.aversion == nit->second.aversion,
                        "KEEPER_READ_ORACLE GetACL aversion mismatch on node " + std::to_string(node_id) + " path=" + req.path);
                }
                coverage.hit("local_read_verified");
                break;
            }
            case Coordination::OpNum::List:
            case Coordination::OpNum::SimpleList:
            case Coordination::OpNum::FilteredList:
            case Coordination::OpNum::FilteredListWithStatsAndData:
            {
                const auto & req = dynamic_cast<const Coordination::ZooKeeperListRequest &>(zk_req);
                const auto & r = dynamic_cast<const Coordination::ListResponse &>(resp);
                auto nit = state.nodes.find(req.path);
                if (nit == state.nodes.end())
                {
                    check_err(static_cast<int>(r.error), static_cast<int>(Error::ZNONODE), "List");
                }
                else if (!checkACL(state, nit->second.acls, Coordination::ACL::Read, session_id))
                {
                    check_err(static_cast<int>(r.error), static_cast<int>(Error::ZNOAUTH), "List");
                }
                else
                {
                    const auto filter = req.list_request_type.value_or(Coordination::ListRequestType::ALL);
                    const bool want_stat = req.with_stat.value_or(false);
                    const bool want_data = req.with_data.value_or(false);
                    const bool inspect_children = (filter != Coordination::ListRequestType::ALL) || want_stat || want_data;
                    auto all_children = listChildren(state, req.path);
                    bool child_noauth = false;
                    std::vector<std::string> expected_names;
                    for (const auto & ch : all_children)
                    {
                        const std::string cpath = (req.path == "/") ? ("/" + ch) : (req.path + "/" + ch);
                        const auto & cnode = state.nodes.at(cpath);
                        if (inspect_children && !checkACL(state, cnode.acls, Coordination::ACL::Read, session_id))
                        {
                            child_noauth = true;
                            break;
                        }
                        if (filter == Coordination::ListRequestType::EPHEMERAL_ONLY && !cnode.is_ephemeral)
                            continue;
                        if (filter == Coordination::ListRequestType::PERSISTENT_ONLY && cnode.is_ephemeral)
                            continue;
                        expected_names.push_back(ch);
                    }
                    if (child_noauth)
                    {
                        check_err(static_cast<int>(r.error), static_cast<int>(Error::ZNOAUTH), "List");
                    }
                    else
                    {
                        check_err(static_cast<int>(r.error), static_cast<int>(Error::ZOK), "List");
                        if (want_stat)
                            require(r.stats.size() == r.names.size(), "KEEPER_READ_ORACLE List stats size mismatch");
                        if (want_data)
                            require(r.data.size() == r.names.size(), "KEEPER_READ_ORACLE List data size mismatch");
                        for (size_t i = 0; i < r.names.size(); ++i)
                        {
                            const std::string cpath = (req.path == "/") ? ("/" + r.names[i]) : (req.path + "/" + r.names[i]);
                            auto cit = state.nodes.find(cpath);
                            require(cit != state.nodes.end(), "KEEPER_READ_ORACLE List returned non-existent child " + cpath);
                            if (want_stat)
                                require(r.stats[i].version == cit->second.version, "KEEPER_READ_ORACLE List child stat mismatch");
                            if (want_data)
                                require(r.data[i] == cit->second.data, "KEEPER_READ_ORACLE List child data mismatch");
                        }
                        auto actual_sorted = r.names;
                        std::sort(actual_sorted.begin(), actual_sorted.end());
                        require(actual_sorted == expected_names,
                            "KEEPER_READ_ORACLE List names mismatch on node " + std::to_string(node_id) + " path=" + req.path);
                    }
                }
                coverage.hit("local_read_verified");
                break;
            }
            case Coordination::OpNum::ListWithOptions:
            {
                const auto & req = dynamic_cast<const Coordination::ZooKeeperListWithOptionsRequest &>(zk_req);
                const auto & r = dynamic_cast<const Coordination::ListWithOptionsResponse &>(resp);
                auto nit = state.nodes.find(req.path);
                if (nit == state.nodes.end())
                {
                    check_err(static_cast<int>(r.error), static_cast<int>(Error::ZNONODE), "ListWithOptions");
                }
                else if (!checkACL(state, nit->second.acls, Coordination::ACL::Read, session_id))
                {
                    check_err(static_cast<int>(r.error), static_cast<int>(Error::ZNOAUTH), "ListWithOptions");
                }
                else if (r.error == Error::ZOK)
                {
                    if (req.options.with_stat)
                        require(r.stats.size() == r.names.size(), "KEEPER_READ_ORACLE ListWithOptions stats size mismatch");
                    if (req.options.with_data)
                        require(r.data.size() == r.names.size(), "KEEPER_READ_ORACLE ListWithOptions data size mismatch");
                    if (req.options.max_results != 0)
                        require(r.names.size() <= req.options.max_results, "KEEPER_READ_ORACLE ListWithOptions exceeded max_results");
                    std::set<std::string> seen;
                    for (size_t i = 0; i < r.names.size(); ++i)
                    {
                        require(seen.insert(r.names[i]).second, "KEEPER_READ_ORACLE ListWithOptions duplicate name " + r.names[i]);
                        const std::string cpath = (req.path == "/") ? ("/" + r.names[i]) : (req.path + "/" + r.names[i]);
                        auto cit = state.nodes.find(cpath);
                        require(cit != state.nodes.end(), "KEEPER_READ_ORACLE ListWithOptions missing child " + cpath);
                        if (req.options.filter == Coordination::ListRequestType::EPHEMERAL_ONLY)
                            require(cit->second.is_ephemeral, "KEEPER_READ_ORACLE ListWithOptions filter EPHEMERAL violated");
                        if (req.options.filter == Coordination::ListRequestType::PERSISTENT_ONLY)
                            require(!cit->second.is_ephemeral, "KEEPER_READ_ORACLE ListWithOptions filter PERSISTENT violated");
                        if (req.options.with_stat)
                            require(r.stats[i].version == cit->second.version, "KEEPER_READ_ORACLE ListWithOptions child stat mismatch");
                        if (req.options.with_data)
                            require(r.data[i] == cit->second.data, "KEEPER_READ_ORACLE ListWithOptions child data mismatch");
                    }
                }
                coverage.hit("local_read_verified");
                break;
            }
            case Coordination::OpNum::MultiRead:
            {
                const auto & req = dynamic_cast<const Coordination::ZooKeeperMultiRequest &>(zk_req);
                const auto & r = dynamic_cast<const Coordination::MultiResponse &>(resp);
                if (r.error == Error::ZOK)
                {
                    require(r.responses.size() == req.requests.size(), "KEEPER_READ_ORACLE MultiRead response count mismatch");
                    for (size_t i = 0; i < req.requests.size(); ++i)
                    {
                        auto sub_zk = std::dynamic_pointer_cast<Coordination::ZooKeeperRequest>(req.requests[i]);
                        if (sub_zk && r.responses[i])
                            verifyLocalReadResponse(node_id, commit_idx, session_id, *sub_zk, *r.responses[i]);
                    }
                }
                break;
            }
            default:
                break;
        }
    }

    const TreeState & latestTree() const { return current_tree; }
    const Stats & getStats() const { return stats; }
    uint64_t highestModeledIndex() const { return highest_modeled_idx; }

private:
    static std::string parentPath(const std::string & path)
    {
        if (path == "/" || path.empty())
            return "";
        const auto pos = path.rfind('/');
        if (pos == 0)
            return "/";
        return path.substr(0, pos);
    }

    static std::vector<std::string> listChildren(const TreeState & state, const std::string & path)
    {
        std::vector<std::string> out;
        const std::string prefix = (path == "/") ? "/" : (path + "/");
        for (const auto & [k, _] : state.nodes)
        {
            if (k == path || !k.starts_with(prefix))
                continue;
            const std::string rest = k.substr(prefix.size());
            if (rest.find('/') == std::string::npos)
                out.push_back(rest);
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    static bool hasChildren(const TreeState & state, const std::string & path)
    {
        const std::string prefix = (path == "/") ? "/" : (path + "/");
        for (const auto & [k, _] : state.nodes)
        {
            if (k != path && k.starts_with(prefix))
                return true;
        }
        return false;
    }

    static bool checkACL(
        const TreeState & state,
        const std::vector<Coordination::ACL> & acls,
        int32_t perm,
        int64_t session_id)
    {
        if (acls.empty())
            return true;
        auto auth_it = state.session_auths.find(session_id);
        for (const auto & acl : acls)
        {
            if ((acl.permissions & perm) == 0)
                continue;
            if (acl.scheme == "world" && acl.id == "anyone")
                return true;
            if (auth_it != state.session_auths.end() && auth_it->second.contains({acl.scheme, acl.id}))
                return true;
        }
        return false;
    }

    static bool fixupACL(
        const TreeState & state,
        const std::vector<Coordination::ACL> & request_acls,
        int64_t session_id,
        std::vector<Coordination::ACL> & out_acls)
    {
        out_acls.clear();
        if (request_acls.empty())
            return true;
        bool valid_found = false;
        for (const auto & req_acl : request_acls)
        {
            if (req_acl.scheme == "auth")
            {
                auto auth_it = state.session_auths.find(session_id);
                if (auth_it != state.session_auths.end())
                {
                    for (const auto & [s, id] : auth_it->second)
                    {
                        valid_found = true;
                        out_acls.push_back(Coordination::ACL{req_acl.permissions, s, id});
                    }
                }
            }
            else if (req_acl.scheme == "world" && req_acl.id == "anyone")
            {
                valid_found = true;
                out_acls.push_back(req_acl);
            }
            else if (req_acl.scheme == "digest")
            {
                if (std::count(req_acl.id.begin(), req_acl.id.end(), ':') != 1)
                    return false;
                valid_found = true;
                out_acls.push_back(req_acl);
            }
        }
        if (std::all_of(out_acls.begin(), out_acls.end(), [](const Coordination::ACL & acl)
            { return acl.scheme == "world" && acl.id == "anyone" && acl.permissions == Coordination::ACL::All; }))
        {
            out_acls.clear();
        }
        return valid_found;
    }

    static int applySingleOp(
        TreeState & state,
        const Coordination::ZooKeeperRequest & zk_req,
        int64_t session_id,
        int64_t req_time,
        std::string & out_created_path,
        int * out_alt_error = nullptr)
    {
        using Coordination::Error;
        switch (zk_req.getOpNum())
        {
            case Coordination::OpNum::Auth:
            {
                const auto & req = dynamic_cast<const Coordination::ZooKeeperAuthRequest &>(zk_req);
                if (req.scheme != "digest" || std::count(req.data.begin(), req.data.end(), ':') != 1)
                    return static_cast<int>(Error::ZAUTHFAILED);
                state.session_auths[session_id].emplace(req.scheme, DB::KeeperStorage::generateDigest(req.data));
                return static_cast<int>(Error::ZOK);
            }
            case Coordination::OpNum::SetACL:
            {
                const auto & req = dynamic_cast<const Coordination::ZooKeeperSetACLRequest &>(zk_req);
                state.known_paths.insert(req.path);
                auto it = state.nodes.find(req.path);
                if (it == state.nodes.end())
                    return static_cast<int>(Error::ZNONODE);
                if (!checkACL(state, it->second.acls, Coordination::ACL::Admin, session_id))
                    return static_cast<int>(Error::ZNOAUTH);
                if (req.version != -1 && req.version != it->second.aversion)
                    return static_cast<int>(Error::ZBADVERSION);
                std::vector<Coordination::ACL> fixed_acls;
                if (!fixupACL(state, req.acls, session_id, fixed_acls))
                    return static_cast<int>(Error::ZINVALIDACL);
                ++it->second.aversion;
                it->second.acls = std::move(fixed_acls);
                return static_cast<int>(Error::ZOK);
            }
            case Coordination::OpNum::Create:
            case Coordination::OpNum::Create2:
            case Coordination::OpNum::CreateIfNotExists:
            case Coordination::OpNum::CreateContainer:
            case Coordination::OpNum::CreateTTL:
            {
                const auto & req = dynamic_cast<const Coordination::ZooKeeperCreateRequest &>(zk_req);
                const std::string parent = parentPath(req.path);
                if (parent.empty() || !state.nodes.contains(parent))
                    return static_cast<int>(Error::ZNONODE);
                auto & parent_node = state.nodes.at(parent);
                if (!checkACL(state, parent_node.acls, Coordination::ACL::Create, session_id))
                    return static_cast<int>(Error::ZNOAUTH);
                if (parent_node.is_ephemeral)
                    return static_cast<int>(Error::ZNOCHILDRENFOREPHEMERALS);
                if (parent_node.is_ttl)
                    return static_cast<int>(Error::ZBADARGUMENTS);

                std::string final_path = req.path;
                if (req.is_sequential)
                {
                    if (req.not_exists)
                        return static_cast<int>(Error::ZBADARGUMENTS);
                    std::ostringstream oss;
                    oss << std::setw(10) << std::setfill('0') << parent_node.seq_num;
                    final_path += oss.str();
                }
                state.known_paths.insert(final_path);
                state.known_paths.insert(parent);

                if (state.nodes.contains(final_path))
                {
                    if (req.not_exists)
                    {
                        out_created_path = final_path;
                        return static_cast<int>(Error::ZOK);
                    }
                    return static_cast<int>(Error::ZNODEEXISTS);
                }

                if (req.include_ttl && (req.is_ephemeral || req.ttl <= 0 || req.ttl > DB::MAX_KEEPER_TTL_MS))
                    return static_cast<int>(Error::ZBADARGUMENTS);

                std::vector<Coordination::ACL> fixed_acls;
                if (!fixupACL(state, req.acls, session_id, fixed_acls))
                    return static_cast<int>(Error::ZINVALIDACL);

                ++parent_node.seq_num;
                ModelNode created;
                created.data = req.data;
                created.version = 0;
                created.aversion = 0;
                created.mtime = req_time;
                created.acls = std::move(fixed_acls);
                if (req.is_ephemeral)
                {
                    created.is_ephemeral = true;
                    created.ephemeral_owner = session_id;
                }
                else if (req.include_ttl)
                {
                    created.is_ttl = true;
                    created.ttl_ms = req.ttl;
                }
                else if (req.is_container)
                {
                    created.is_container = true;
                    created.ephemeral_owner = DB::KeeperNodeStats::CONTAINER_EPHEMERAL_OWNER;
                }
                state.nodes[final_path] = std::move(created);
                out_created_path = final_path;
                return static_cast<int>(Error::ZOK);
            }
            case Coordination::OpNum::Set:
            {
                const auto & req = dynamic_cast<const Coordination::ZooKeeperSetRequest &>(zk_req);
                state.known_paths.insert(req.path);
                auto it = state.nodes.find(req.path);
                if (it == state.nodes.end())
                    return static_cast<int>(Error::ZNONODE);
                if (!checkACL(state, it->second.acls, Coordination::ACL::Write, session_id))
                    return static_cast<int>(Error::ZNOAUTH);
                if (req.version != -1 && req.version != it->second.version)
                    return static_cast<int>(Error::ZBADVERSION);
                it->second.data = req.data;
                ++it->second.version;
                it->second.mtime = req_time;
                return static_cast<int>(Error::ZOK);
            }
            case Coordination::OpNum::Remove:
            case Coordination::OpNum::TryRemove:
            {
                const auto & req = dynamic_cast<const Coordination::ZooKeeperRemoveRequest &>(zk_req);
                state.known_paths.insert(req.path);
                const bool is_ttl_gc = req.try_remove && session_id == DB::keeper_internal_ttl_garbage_collector_session_id;
                const bool is_container_gc = req.try_remove && session_id == DB::keeper_internal_container_garbage_collector_session_id;
                const bool is_internal_gc = is_ttl_gc || is_container_gc;
                const std::string parent = parentPath(req.path);
                if (parent.empty() || !state.nodes.contains(parent))
                    return req.try_remove ? static_cast<int>(Error::ZOK) : static_cast<int>(Error::ZNONODE);
                if (!is_internal_gc && !checkACL(state, state.nodes.at(parent).acls, Coordination::ACL::Delete, session_id))
                    return static_cast<int>(Error::ZNOAUTH);
                auto it = state.nodes.find(req.path);
                if (it == state.nodes.end())
                    return req.try_remove ? static_cast<int>(Error::ZOK) : static_cast<int>(Error::ZNONODE);
                if (req.version != -1 && req.version != it->second.version)
                    return req.try_remove ? static_cast<int>(Error::ZOK) : static_cast<int>(Error::ZBADVERSION);
                if (is_ttl_gc && (!it->second.is_ttl || req_time < it->second.mtime + it->second.ttl_ms))
                    return static_cast<int>(Error::ZOK);
                if (is_container_gc && (!it->second.is_container || hasChildren(state, req.path)))
                    return static_cast<int>(Error::ZOK);
                if (hasChildren(state, req.path))
                    return req.try_remove ? static_cast<int>(Error::ZOK) : static_cast<int>(Error::ZNOTEMPTY);
                state.nodes.erase(it);
                return static_cast<int>(Error::ZOK);
            }
            case Coordination::OpNum::RemoveRecursive:
            {
                const auto & req = dynamic_cast<const Coordination::ZooKeeperRemoveRecursiveRequest &>(zk_req);
                if (req.path == "/")
                    return static_cast<int>(Error::ZBADARGUMENTS);
                const std::string parent = parentPath(req.path);
                if (parent.empty() || !state.nodes.contains(parent))
                    return static_cast<int>(Error::ZOK);
                if (!checkACL(state, state.nodes.at(parent).acls, Coordination::ACL::Delete, session_id))
                    return static_cast<int>(Error::ZNOAUTH);
                if (!state.nodes.contains(req.path))
                    return static_cast<int>(Error::ZOK);
                /// Keeper's `visitUncommittedRecursive` iterates children in unordered hash-set order,
                /// so if BOTH `subtree_size > limit` (ZNOTEMPTY) and some child lacks Delete ACL (ZNOAUTH),
                /// different replicas can return either error (known Bug #7). Record alt_error so we keep hunting!
                std::vector<std::string> to_remove;
                const std::string prefix = req.path + "/";
                bool has_noauth = false;
                for (const auto & [k, node] : state.nodes)
                {
                    if (k == req.path || k.starts_with(prefix))
                    {
                        to_remove.push_back(k);
                        if (!checkACL(state, node.acls, Coordination::ACL::Delete, session_id))
                            has_noauth = true;
                    }
                }
                const bool exceeds_limit = to_remove.size() > req.remove_nodes_limit;
                if (has_noauth && exceeds_limit)
                {
                    if (out_alt_error)
                        *out_alt_error = static_cast<int>(Error::ZNOTEMPTY);
                    return static_cast<int>(Error::ZNOAUTH);
                }
                if (has_noauth)
                    return static_cast<int>(Error::ZNOAUTH);
                if (exceeds_limit)
                    return static_cast<int>(Error::ZNOTEMPTY);
                for (const auto & k : to_remove)
                {
                    state.known_paths.insert(k);
                    state.nodes.erase(k);
                }
                return static_cast<int>(Error::ZOK);
            }
            case Coordination::OpNum::Check:
            case Coordination::OpNum::CheckNotExists:
            case Coordination::OpNum::CheckStat:
            {
                const auto & req = dynamic_cast<const Coordination::ZooKeeperCheckRequest &>(zk_req);
                state.known_paths.insert(req.path);
                auto it = state.nodes.find(req.path);
                if (it != state.nodes.end() && !checkACL(state, it->second.acls, Coordination::ACL::Read, session_id))
                    return static_cast<int>(Error::ZNOAUTH);
                if (req.not_exists)
                {
                    if (it != state.nodes.end() && (req.version == -1 || req.version == it->second.version))
                        return static_cast<int>(Error::ZNODEEXISTS);
                    return static_cast<int>(Error::ZOK);
                }
                if (it == state.nodes.end())
                    return static_cast<int>(Error::ZNONODE);
                if (req.version != -1 && req.version != it->second.version)
                    return static_cast<int>(Error::ZBADVERSION);
                if (zk_req.getOpNum() == Coordination::OpNum::CheckStat && req.stat_to_check)
                {
                    const auto & v = *req.stat_to_check;
                    if (v.version != -1 && v.version != it->second.version)
                        return static_cast<int>(Error::ZBADVERSION);
                    if (v.aversion != -1 && v.aversion != it->second.aversion)
                        return static_cast<int>(Error::ZBADVERSION);
                    if (v.ephemeralOwner != -1 && v.ephemeralOwner != it->second.ephemeral_owner)
                        return static_cast<int>(Error::ZBADVERSION);
                    if (v.dataLength != -1 && v.dataLength != static_cast<int32_t>(it->second.data.size()))
                        return static_cast<int>(Error::ZBADVERSION);
                    if (v.numChildren != -1 && v.numChildren != static_cast<int32_t>(listChildren(state, req.path).size()))
                        return static_cast<int>(Error::ZBADVERSION);
                }
                return static_cast<int>(Error::ZOK);
            }
            default:
                return static_cast<int>(Error::ZOK);
        }
    }

    ExpectedOutcome applyToModel(TreeState & state, const DB::KeeperRequestForSession & req_for_session)
    {
        ExpectedOutcome out;
        if (!req_for_session.request)
            return out;
        const auto & zk_req = *req_for_session.request;
        switch (zk_req.getOpNum())
        {
            case Coordination::OpNum::SessionID:
            {
                out.allocated_session_id = state.next_session_id++;
                state.active_sessions.insert(out.allocated_session_id);
                out.error = 0;
                return out;
            }
            case Coordination::OpNum::Close:
            {
                state.active_sessions.erase(req_for_session.session_id);
                state.session_auths.erase(req_for_session.session_id);
                size_t removed_ephemerals = 0;
                for (auto it = state.nodes.begin(); it != state.nodes.end();)
                {
                    if (it->second.ephemeral_owner == req_for_session.session_id)
                    {
                        it = state.nodes.erase(it);
                        ++removed_ephemerals;
                    }
                    else
                    {
                        ++it;
                    }
                }
                ++stats.sessions_closed;
                if (removed_ephemerals > 0)
                    coverage.hit("session_expired_ephemerals_cleaned");
                out.error = 0;
                return out;
            }
            case Coordination::OpNum::Heartbeat:
            case Coordination::OpNum::Sync:
            {
                out.error = 0;
                return out;
            }
            case Coordination::OpNum::Create:
            case Coordination::OpNum::Create2:
            case Coordination::OpNum::CreateIfNotExists:
            case Coordination::OpNum::CreateContainer:
            case Coordination::OpNum::CreateTTL:
            {
                const auto & creq = dynamic_cast<const Coordination::ZooKeeperCreateRequest &>(zk_req);
                out.error = applySingleOp(state, zk_req, req_for_session.session_id, req_for_session.time, out.created_path);
                if (creq.is_sequential && out.error == 0)
                {
                    ++stats.sequential_created;
                    coverage.hit("sequential_node_created");
                }
                if (creq.is_container && out.error == 0)
                    coverage.hit("container_node_created");
                if (creq.include_ttl && out.error == 0)
                    coverage.hit("ttl_node_created");
                return out;
            }
            case Coordination::OpNum::Set:
            case Coordination::OpNum::Remove:
            case Coordination::OpNum::TryRemove:
            case Coordination::OpNum::RemoveRecursive:
            case Coordination::OpNum::Check:
            case Coordination::OpNum::CheckNotExists:
            case Coordination::OpNum::CheckStat:
            case Coordination::OpNum::Auth:
            case Coordination::OpNum::SetACL:
            {
                out.error = applySingleOp(state, zk_req, req_for_session.session_id, req_for_session.time, out.created_path, &out.alt_error);
                if (zk_req.getOpNum() == Coordination::OpNum::RemoveRecursive)
                    coverage.hit("remove_recursive_executed");
                else if (zk_req.getOpNum() == Coordination::OpNum::TryRemove)
                    coverage.hit("try_remove_executed");
                else if (zk_req.getOpNum() == Coordination::OpNum::CheckStat)
                    coverage.hit("check_stat_executed");
                else if (zk_req.getOpNum() == Coordination::OpNum::Auth && out.error == 0)
                    coverage.hit("auth_executed");
                else if (zk_req.getOpNum() == Coordination::OpNum::SetACL)
                    coverage.hit("set_acl_executed");
                return out;
            }
            case Coordination::OpNum::Multi:
            {
                const auto & mreq = dynamic_cast<const Coordination::ZooKeeperMultiRequest &>(zk_req);
                out.is_multi = true;
                out.error = 0; // Raw KeeperStateMachine outer response carries ZOK; subresponses carry errors.
                TreeState copy = state;
                bool failed = false;
                for (const auto & sub : mreq.requests)
                {
                    if (failed)
                    {
                        out.sub_errors.push_back(static_cast<int>(Coordination::Error::ZRUNTIMEINCONSISTENCY));
                        out.sub_alt_errors.push_back(0);
                        continue;
                    }
                    std::string sub_created;
                    int sub_alt = 0;
                    const int err = applySingleOp(copy, *sub, req_for_session.session_id, req_for_session.time, sub_created, &sub_alt);
                    out.sub_errors.push_back(err);
                    out.sub_alt_errors.push_back(sub_alt);
                    if (err != 0)
                        failed = true;
                }
                if (!failed)
                {
                    state = std::move(copy);
                    ++stats.multi_committed;
                    coverage.hit("multi_committed");
                }
                else
                {
                    ++stats.multi_rolled_back;
                    coverage.hit("multi_rolled_back");
                }
                return out;
            }
            default:
                return out;
        }
    }

    CoverageTracker & coverage;
    TreeState current_tree;
    uint64_t highest_modeled_idx = 0;
    std::pair<uint64_t, uint32_t> highest_modeled_key{0, 0};
    std::map<uint64_t, TreeState> tree_by_index;
    std::map<std::pair<uint64_t, uint32_t>, ExpectedOutcome> expected_by_key;
    std::map<std::pair<uint64_t, uint32_t>, uint64_t> digests_by_key;
    std::map<uint64_t, uint64_t> digests_by_index;
    std::map<std::pair<int64_t, int64_t>, ClientOpRecord> client_ops;
    Stats stats;
};

} // namespace keeper_sim
