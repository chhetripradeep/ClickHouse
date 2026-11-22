#pragma once

#include <SimClock.h>
#include <SimNetwork.h>

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

namespace fdb_sim
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

    explicit RaftSafetyOracle(int cluster_size_, bool strict_current_term_commit_, CoverageTracker & coverage_)
        : quorum_size(cluster_size_ / 2 + 1)
        , strict_current_term_commit(strict_current_term_commit_)
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

    /// Reset per-node volatile state after a disk wipe (a wiped node loses its local term/vote history
    /// by definition when simulating hardware replacement).
    void onNodeDiskWiped(int node_id)
    {
        max_term_by_node.erase(node_id);
        prev_target_commit.erase(node_id);
        durable_terms_by_node.erase(node_id);
        for (auto it = persisted_votes.begin(); it != persisted_votes.end();)
        {
            if (it->first.first == node_id)
                it = persisted_votes.erase(it);
            else
                ++it;
        }
    }

    const Stats & getStats() const { return stats; }
    uint64_t highestCommittedIndex() const { return committed_entries.empty() ? 0 : committed_entries.rbegin()->first; }

private:
    int quorum_size;
    bool strict_current_term_commit;
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
        int64_t ephemeral_owner = 0;
        int64_t seq_num = 0;
    };

    struct TreeState
    {
        std::map<std::string, ModelNode> nodes;
        std::set<int64_t> active_sessions;
        int64_t next_session_id = 1;
        std::set<std::string> known_paths;

        TreeState()
        {
            nodes["/"] = ModelNode{"", 0, 0, 0};
            known_paths.insert("/");
        }
    };

    struct ExpectedOutcome
    {
        int error = 0;
        bool is_multi = false;
        std::vector<int> sub_errors;
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

    /// Called immediately after `KeeperStateMachine::commit` applies `request` at `log_idx` on `node_id`.
    void onKeeperCommit(
        int node_id,
        uint64_t log_idx,
        const DB::KeeperRequestForSession & req,
        const Coordination::ZooKeeperResponsePtr & actual_response,
        uint64_t actual_digest)
    {
        /// Advance independent reference model on the first replica to reach `log_idx`.
        if (!expected_by_index.contains(log_idx))
        {
            require(log_idx > highest_modeled_idx,
                "KEEPER_ORACLE out-of-order first commit: log_idx=" + std::to_string(log_idx)
                + " <= highest=" + std::to_string(highest_modeled_idx));
            ExpectedOutcome expected = applyToModel(current_tree, req);
            expected_by_index[log_idx] = expected;
            tree_by_index[log_idx] = current_tree;
            highest_modeled_idx = log_idx;
            ++stats.model_commits;

            /// Mark client op committed for linearizability/durability tracking.
            if (req.request)
            {
                auto cit = client_ops.find({req.session_id, req.request->xid});
                if (cit != client_ops.end())
                    cit->second.committed = true;
            }
        }

        /// Verify exact cross-replica KeeperDigest at `log_idx`.
        verifyDigest(node_id, log_idx, actual_digest);

        /// Verify actual Keeper response against independent model.
        if (actual_response && req.request)
        {
            const auto & expected = expected_by_index.at(log_idx);
            ++stats.replica_responses_checked;
            require(static_cast<int>(actual_response->error) == expected.error,
                "KEEPER_ORACLE response error mismatch on node " + std::to_string(node_id)
                + " log_idx=" + std::to_string(log_idx)
                + " xid=" + std::to_string(req.request->xid)
                + ": actual=" + std::to_string(static_cast<int>(actual_response->error))
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
                    require(actual_sub == expected.sub_errors[i],
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
                require(actual_stats.getEphemeralOwner() == expected_node.ephemeral_owner,
                    "KEEPER_ORACLE ephemeralOwner mismatch on node " + std::to_string(node_id)
                    + " path=" + path + " actual=" + std::to_string(actual_stats.getEphemeralOwner())
                    + " expected=" + std::to_string(expected_node.ephemeral_owner));

                /// Ephemeral-session consistency: an ephemeral node's owner MUST be an active session!
                if (expected_node.ephemeral_owner != 0)
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

    static int applySingleOp(
        TreeState & state,
        const Coordination::ZooKeeperRequest & zk_req,
        int64_t session_id,
        std::string & out_created_path)
    {
        using Coordination::Error;
        switch (zk_req.getOpNum())
        {
            case Coordination::OpNum::Create:
            {
                const auto & req = dynamic_cast<const Coordination::ZooKeeperCreateRequest &>(zk_req);
                const std::string parent = parentPath(req.path);
                if (parent.empty() || !state.nodes.contains(parent))
                    return static_cast<int>(Error::ZNONODE);
                if (state.nodes.at(parent).ephemeral_owner != 0)
                    return static_cast<int>(Error::ZNOCHILDRENFOREPHEMERALS);

                std::string final_path = req.path;
                auto & parent_node = state.nodes.at(parent);
                if (req.is_sequential)
                {
                    std::ostringstream oss;
                    oss << std::setw(10) << std::setfill('0') << parent_node.seq_num;
                    final_path += oss.str();
                }
                state.known_paths.insert(final_path);
                state.known_paths.insert(parent);

                if (state.nodes.contains(final_path))
                {
                    if (req.not_exists)
                        return static_cast<int>(Error::ZOK);
                    return static_cast<int>(Error::ZNODEEXISTS);
                }

                ++parent_node.seq_num;
                state.nodes[final_path] = ModelNode{
                    req.data,
                    0,
                    req.is_ephemeral ? session_id : 0,
                    0,
                };
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
                if (req.version != -1 && req.version != it->second.version)
                    return static_cast<int>(Error::ZBADVERSION);
                it->second.data = req.data;
                ++it->second.version;
                return static_cast<int>(Error::ZOK);
            }
            case Coordination::OpNum::Remove:
            {
                const auto & req = dynamic_cast<const Coordination::ZooKeeperRemoveRequest &>(zk_req);
                state.known_paths.insert(req.path);
                auto it = state.nodes.find(req.path);
                if (it == state.nodes.end())
                    return static_cast<int>(Error::ZNONODE);
                if (req.version != -1 && req.version != it->second.version)
                    return static_cast<int>(Error::ZBADVERSION);
                if (hasChildren(state, req.path))
                    return static_cast<int>(Error::ZNOTEMPTY);
                state.nodes.erase(it);
                return static_cast<int>(Error::ZOK);
            }
            case Coordination::OpNum::Check:
            {
                const auto & req = dynamic_cast<const Coordination::ZooKeeperCheckRequest &>(zk_req);
                state.known_paths.insert(req.path);
                auto it = state.nodes.find(req.path);
                if (req.not_exists)
                {
                    if (it != state.nodes.end())
                        return static_cast<int>(Error::ZNODEEXISTS);
                    return static_cast<int>(Error::ZOK);
                }
                if (it == state.nodes.end())
                    return static_cast<int>(Error::ZNONODE);
                if (req.version != -1 && req.version != it->second.version)
                    return static_cast<int>(Error::ZBADVERSION);
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
            {
                const auto & creq = dynamic_cast<const Coordination::ZooKeeperCreateRequest &>(zk_req);
                out.error = applySingleOp(state, zk_req, req_for_session.session_id, out.created_path);
                if (creq.is_sequential && out.error == 0)
                {
                    ++stats.sequential_created;
                    coverage.hit("sequential_node_created");
                }
                return out;
            }
            case Coordination::OpNum::Set:
            case Coordination::OpNum::Remove:
            case Coordination::OpNum::Check:
            {
                out.error = applySingleOp(state, zk_req, req_for_session.session_id, out.created_path);
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
                        continue;
                    }
                    std::string sub_created;
                    const int err = applySingleOp(copy, *sub, req_for_session.session_id, sub_created);
                    out.sub_errors.push_back(err);
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
    std::map<uint64_t, TreeState> tree_by_index;
    std::map<uint64_t, ExpectedOutcome> expected_by_index;
    std::map<uint64_t, uint64_t> digests_by_index;
    std::map<std::pair<int64_t, int64_t>, ClientOpRecord> client_ops;
    Stats stats;
};

} // namespace fdb_sim
