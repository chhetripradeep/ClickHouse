#pragma once

#include <SimEngine.h>

#include <libnuraft/nuraft.hxx>
#include <libnuraft/raft_server_handler.hxx>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace keeper_sim
{

/// Thin subclass of NuRaft's `raft_server` exposing its protected background execution hooks
/// so the discrete-event engine drives append and commit work single-threaded.
class SimRaftServer : public nuraft::raft_server
{
public:
    explicit SimRaftServer(nuraft::context * ctx)
        : nuraft::raft_server(ctx, nuraft::raft_server::init_options(false, false, true))
    {
    }

    void appendStep()
    {
        append_entries_in_bg_exec();
    }

    void commitStep()
    {
        commit_in_bg_exec();
    }
};

/// Real NuRaft timer scheduler wired directly onto SimEngine's virtual-time priority queue.
/// Election timers, heartbeat timers, and client request timers fire automatically as
/// virtual time advances — no hardcoded manual timer firing needed.
class SimTimers : public nuraft::delayed_task_scheduler
{
public:
    SimTimers(int node_id_, uint64_t incarnation_, SimEngine & engine_)
        : node_id(node_id_)
        , incarnation(incarnation_)
        , engine(engine_)
    {
    }

    void schedule(nuraft::ptr<nuraft::delayed_task> & task, int32_t milliseconds) override
    {
        require(milliseconds >= 0, "SimTimers: negative delay");
        cancel_impl(task);
        task->reset();

        auto task_copy = task;
        const int type = task->get_type();
        const EventId eid = engine.scheduleAfterMs(
            milliseconds,
            "timer-" + std::to_string(type),
            [this, task_copy, type]()
            {
                active.erase(task_copy.get());
                engine.trace("TIMER_FIRE n=" + std::to_string(node_id) + " type=" + std::to_string(type));
                task_copy->execute();
            },
            node_id,
            incarnation);
        active[task.get()] = eid;
    }

    void cancelAll()
    {
        for (const auto & [ptr, eid] : active)
            engine.cancel(eid);
        active.clear();
    }

    bool empty() const { return active.empty(); }

private:
    void cancel_impl(nuraft::ptr<nuraft::delayed_task> & task) override
    {
        auto it = active.find(task.get());
        if (it != active.end())
        {
            engine.cancel(it->second);
            active.erase(it);
        }
    }

    int node_id;
    uint64_t incarnation;
    SimEngine & engine;
    std::map<nuraft::delayed_task *, EventId> active;
};

/// Directed network link parameters (src -> dst).
struct LinkConfig
{
    bool cut = false;                 // Hard partition cut on this direction
    double drop_prob = 0.0;           // Packet drop probability
    double response_drop_prob = 0.0;  // Drop reply AFTER destination processes request
    int64_t base_latency_ms = 2;      // Normal one-way latency
    int64_t jitter_ms = 3;            // Random jitter (enables natural reordering)
    int64_t clog_extra_ms = 0;        // SwizzleClogger injected delay
};

/// FoundationDB Sim2-inspired simulated network, RPC transport, and NuRaft `global_mgr`.
/// Supports:
///   - Symmetric and asymmetric network partitions
///   - SwizzleClogger link delays, jitter, reordering, packet drops, and reply-only drops
///   - Deferred NuRaft async durability responses with clean cancellation on crash
///   - Full multi-chunk Raft snapshot installation RPCs (`install_snapshot_request`)
class SimNetwork : public nuraft::global_mgr, public nuraft::raft_server_handler
{
public:
    using nuraft::raft_server_handler::process_req;

    struct PendingAsyncResponse
    {
        int src = 0;
        int dst = 0;
        uint64_t src_inc = 0;
        uint64_t dst_inc = 0;
        std::function<void(bool notify_src)> cancel;
    };

    struct RpcObservation
    {
        int msg_type = 0;
        int from = 0;
        int to = 0;
        uint64_t req_term = 0;
        uint64_t resp_term = 0;
        bool accepted = false;
        uint64_t prev_log_idx = 0;
        uint64_t end_log_idx = 0;
        uint64_t next_idx = 0;
    };

    struct Stats
    {
        uint64_t requests_sent = 0;
        uint64_t requests_dropped_partition = 0;
        uint64_t requests_dropped_loss = 0;
        uint64_t responses_dropped_loss = 0;
        uint64_t requests_clogged = 0;
        uint64_t snapshot_rpcs_sent = 0;
        uint64_t async_responses_deferred = 0;
        uint64_t async_responses_cancelled = 0;
    };

    explicit SimNetwork(SimEngine & engine_)
        : engine(engine_)
    {
    }

    /// Optional callback invoked whenever an RPC response is delivered to the caller (for Raft safety oracle).
    std::function<void(const RpcObservation &)> on_rpc_response;

    void registerServer(int node_id, nuraft::ptr<SimRaftServer> server, uint64_t incarnation)
    {
        servers[node_id] = std::move(server);
        incarnations[node_id] = incarnation;
        engine.setNodeIncarnation(node_id, incarnation);
    }

    void unregisterServer(int node_id)
    {
        servers.erase(node_id);
        /// Bump incarnation so any queued events for the dead incarnation are skipped.
        const uint64_t next_inc = ++incarnations[node_id];
        engine.setNodeIncarnation(node_id, next_inc);
        cancelPendingForNode(node_id);
    }

    LinkConfig & link(int src, int dst)
    {
        return links[{src, dst}];
    }

    void healAll()
    {
        for (auto & [edge, cfg] : links)
        {
            cfg.cut = false;
            cfg.drop_prob = 0.0;
            cfg.response_drop_prob = 0.0;
            cfg.clog_extra_ms = 0;
            cfg.base_latency_ms = 2;
            cfg.jitter_ms = engine.buggify.isActive("network_reordering") ? 3 : 1;
        }
        engine.trace("NET_HEAL_ALL");
    }

    void partitionSymmetric(const std::set<int> & side_a, const std::set<int> & side_b)
    {
        for (int a : side_a)
        {
            for (int b : side_b)
            {
                link(a, b).cut = true;
                link(b, a).cut = true;
            }
        }
        engine.coverage.hit("network_partition");
        engine.trace("NET_PARTITION_SYMMETRIC");
    }

    void partitionAsymmetric(int src, int dst)
    {
        link(src, dst).cut = true;
        engine.coverage.hit("asymmetric_partition");
        engine.trace("NET_PARTITION_ASYMMETRIC " + std::to_string(src) + "->" + std::to_string(dst));
    }

    void clogLink(int src, int dst, int64_t extra_ms, double drop_prob, double resp_drop_prob)
    {
        auto & cfg = link(src, dst);
        cfg.clog_extra_ms = extra_ms;
        cfg.drop_prob = drop_prob;
        cfg.response_drop_prob = resp_drop_prob;
        engine.coverage.hit("clogged_link");
    }

    /// NuRaft `global_mgr` overrides
    void init_raft_server(nuraft::raft_server *) override {}

    void close_raft_server(nuraft::raft_server * server) override
    {
        if (!server)
            return;
        cancelPendingForNode(server->get_id());
    }

    void request_append(nuraft::ptr<nuraft::raft_server> server) override
    {
        auto node = std::static_pointer_cast<SimRaftServer>(server);
        const int id = node->get_id();
        const uint64_t inc = incarnations[id];
        const int64_t delay_us = engine.buggify.fire("slow_commits", engine.rng, 0.20)
            ? engine.rng.rangeInt(500, 4000)
            : engine.rng.rangeInt(50, 300);
        engine.scheduleAfterUs(
            delay_us,
            "raft-append",
            [node]() { node->appendStep(); },
            id,
            inc);
    }

    void request_commit(nuraft::ptr<nuraft::raft_server> server) override
    {
        auto node = std::static_pointer_cast<SimRaftServer>(server);
        const int id = node->get_id();
        const uint64_t inc = incarnations[id];
        const int64_t delay_us = engine.buggify.fire("slow_commits", engine.rng, 0.30)
            ? engine.rng.rangeInt(1000, 15000)
            : engine.rng.rangeInt(50, 400);
        engine.scheduleAfterUs(
            delay_us,
            "raft-commit",
            [node]() { node->commitStep(); },
            id,
            inc);
    }

    void sendRpc(nuraft::ptr<nuraft::req_msg> request, nuraft::rpc_handler callback, int fallback_dst = 0)
    {
        ++stats.requests_sent;
        const int src = request->get_src();
        const int dst = request->get_dst() != 0 ? request->get_dst() : fallback_dst;
        const uint64_t src_inc = (src != 0) ? incarnations[src] : 0;
        const uint64_t dst_inc = incarnations[dst];
        const auto mtype = request->get_type();
        const uint64_t req_term = request->get_term();
        const uint64_t prev_idx = request->get_last_log_idx();
        const uint64_t end_idx = prev_idx + request->log_entries().size();

        if (mtype == nuraft::msg_type::install_snapshot_request)
        {
            ++stats.snapshot_rpcs_sent;
            engine.coverage.hit("snapshot_network_transfer");
        }

        const auto cfg = link(src, dst);
        if (cfg.cut)
        {
            ++stats.requests_dropped_partition;
            engine.coverage.hit("dropped_message");
            scheduleRpcError(src, src_inc, dst, request, callback, "simulated network partition", 1);
            return;
        }
        if (cfg.drop_prob > 0.0 && engine.rng.chance(cfg.drop_prob))
        {
            ++stats.requests_dropped_loss;
            engine.coverage.hit("dropped_message");
            scheduleRpcError(src, src_inc, dst, request, callback, "simulated packet drop",
                std::max<int64_t>(2, cfg.base_latency_ms));
            return;
        }

        int64_t delay_ms = cfg.base_latency_ms + engine.rng.rangeInt(0, std::max<int64_t>(0, cfg.jitter_ms));
        if (cfg.clog_extra_ms > 0)
        {
            delay_ms += engine.rng.rangeInt(cfg.clog_extra_ms / 2, cfg.clog_extra_ms);
            ++stats.requests_clogged;
            engine.coverage.hit("clogged_message");
        }
        if (engine.buggify.fire("network_reordering", engine.rng, 0.15))
        {
            delay_ms += engine.rng.rangeInt(2, 25);
            engine.coverage.hit("reordered_message");
        }

        engine.scheduleAfterMs(
            delay_ms,
            "rpc-req-" + std::to_string(static_cast<int>(mtype)),
            [this, request, callback, src, dst, src_inc, dst_inc, mtype, req_term, prev_idx, end_idx]() mutable
            {
                auto dst_it = servers.find(dst);
                if (dst_it == servers.end() || incarnations[dst] != dst_inc)
                {
                    scheduleRpcError(src, src_inc, dst, request, callback, "simulated node offline", 1);
                    return;
                }

                /// Re-check link cut at delivery time.
                if (link(src, dst).cut)
                {
                    ++stats.requests_dropped_partition;
                    scheduleRpcError(src, src_inc, dst, request, callback, "simulated link cut in flight", 1);
                    return;
                }

                auto response = process_req(dst_it->second.get(), *request);
                if (!response)
                {
                    /// NuRaft legitimately returns nullptr when rejecting a late RPC (e.g. full-consensus arrival limit).
                    scheduleRpcError(src, src_inc, dst, request, callback, "simulated null response from peer", 2);
                    return;
                }
                require(!response->has_cb(), "SimNetwork: unexpected blocking RPC callback");

                auto deliver_reply = [this, callback, request, response, src, dst, src_inc, mtype, req_term, prev_idx, end_idx]() mutable
                {
                    const auto rev_cfg = link(dst, src);
                    if (rev_cfg.cut)
                    {
                        ++stats.requests_dropped_partition;
                        scheduleRpcError(src, src_inc, dst, request, callback, "simulated reverse partition on reply", 1);
                        return;
                    }
                    const double eff_resp_drop = (mtype == nuraft::msg_type::install_snapshot_request && engine.buggify.snapshot_torture)
                        ? std::max(rev_cfg.response_drop_prob, 0.30)
                        : rev_cfg.response_drop_prob;
                    if (eff_resp_drop > 0.0 && engine.rng.chance(eff_resp_drop))
                    {
                        ++stats.responses_dropped_loss;
                        engine.coverage.hit("dropped_response_after_exec");
                        if (mtype == nuraft::msg_type::install_snapshot_request)
                            engine.coverage.hit("dropped_snapshot_response_after_exec");
                        scheduleRpcError(src, src_inc, dst, request, callback, "simulated response drop after execution",
                            std::max<int64_t>(2, rev_cfg.base_latency_ms));
                        return;
                    }
                    int64_t rev_delay = rev_cfg.base_latency_ms + engine.rng.rangeInt(0, std::max<int64_t>(0, rev_cfg.jitter_ms));
                    if (rev_cfg.clog_extra_ms > 0)
                        rev_delay += engine.rng.rangeInt(rev_cfg.clog_extra_ms / 2, rev_cfg.clog_extra_ms);

                    engine.scheduleAfterMs(
                        rev_delay,
                        "rpc-resp-" + std::to_string(static_cast<int>(response->get_type())),
                        [this, callback, response, src, dst, mtype, req_term, prev_idx, end_idx]() mutable
                        {
                            if (on_rpc_response)
                            {
                                on_rpc_response(RpcObservation{
                                    static_cast<int>(mtype),
                                    dst,
                                    src,
                                    req_term,
                                    response->get_term(),
                                    response->get_accepted(),
                                    prev_idx,
                                    end_idx,
                                    response->get_next_idx(),
                                });
                            }
                            nuraft::ptr<nuraft::rpc_exception> no_err;
                            callback(response, no_err);
                        },
                        src,
                        src_inc);
                };

                if (response->has_async_cb())
                {
                    ++stats.async_responses_deferred;
                    auto deferred = response->call_async_cb();
                    /// Break promise/response reference cycle before registering completion callback.
                    response->set_async_cb({});
                    const uint64_t rid = ++next_response_id;
                    auto cancelled = std::make_shared<bool>(false);
                    pending_responses.emplace(
                        rid,
                        PendingAsyncResponse{
                            src,
                            dst,
                            src_inc,
                            dst_inc,
                            [this, cancelled, request, callback, src, src_inc, dst](bool notify_src) mutable
                            {
                                *cancelled = true;
                                if (notify_src)
                                    scheduleRpcError(src, src_inc, dst, request, callback, "simulated peer crashed before durable reply", 1);
                            },
                        });

                    deferred->when_ready(
                        [this, rid, cancelled, response, deliver_reply](
                            nuraft::cmd_result<nuraft::ptr<nuraft::buffer>> & result,
                            nuraft::ptr<std::exception> & error) mutable
                        {
                            if (*cancelled)
                            {
                                ++stats.async_responses_cancelled;
                                result.reset();
                                return;
                            }
                            pending_responses.erase(rid);
                            if (error)
                            {
                                response->unaccept();
                                response->set_result_code(nuraft::cmd_result_code::FAILED);
                            }
                            else
                            {
                                response->set_ctx(result.get());
                                const auto code = result.get_result_code();
                                if (code != nuraft::cmd_result_code::OK)
                                {
                                    response->unaccept();
                                    response->set_result_code(code);
                                }
                            }
                            deliver_reply();
                            result.reset();
                        });
                }
                else
                {
                    deliver_reply();
                }
            },
            dst,
            dst_inc);
    }

    size_t pendingResponseCount() const { return pending_responses.size(); }
    const Stats & getStats() const { return stats; }

private:
    void scheduleRpcError(
        int src,
        uint64_t src_inc,
        int dst,
        nuraft::ptr<nuraft::req_msg> request,
        nuraft::rpc_handler callback,
        const std::string & reason,
        int64_t delay_ms)
    {
        engine.scheduleAfterMs(
            delay_ms,
            "rpc-err",
            [request, callback, reason]() mutable
            {
                nuraft::ptr<nuraft::resp_msg> no_resp;
                auto err = nuraft::cs_new<nuraft::rpc_exception>(reason, request);
                callback(no_resp, err);
            },
            src,
            src_inc);
        (void)dst;
    }

    void cancelPendingForNode(int node_id)
    {
        for (auto it = pending_responses.begin(); it != pending_responses.end();)
        {
            if (it->second.src == node_id || it->second.dst == node_id)
            {
                it->second.cancel(it->second.src != node_id);
                it = pending_responses.erase(it);
            }
            else
            {
                ++it;
            }
        }
    }

    SimEngine & engine;
    std::map<int, nuraft::ptr<SimRaftServer>> servers;
    std::map<int, uint64_t> incarnations;
    std::map<std::pair<int, int>, LinkConfig> links;
    std::map<uint64_t, PendingAsyncResponse> pending_responses;
    uint64_t next_response_id = 0;
    Stats stats;
};

class SimRpcClient : public nuraft::rpc_client
{
public:
    SimRpcClient(SimNetwork & net_, uint64_t id_, int target_id_ = 0, bool pipelining_ = false)
        : net(net_)
        , id(id_)
        , target_id(target_id_)
        , pipelining(pipelining_)
    {
    }

    void send(nuraft::ptr<nuraft::req_msg> & req, nuraft::rpc_handler & cb, uint64_t) override
    {
        net.sendRpc(req, cb, target_id);
    }

    uint64_t get_id() const override { return id; }
    bool is_abandoned() const override { return false; }
    bool supports_pipelining() const override { return pipelining; }

private:
    SimNetwork & net;
    uint64_t id;
    int target_id;
    bool pipelining;
};

class SimRpcFactory : public nuraft::rpc_client_factory
{
public:
    explicit SimRpcFactory(SimNetwork & net_, bool pipelining_ = false)
        : net(net_)
        , pipelining(pipelining_)
    {
    }

    nuraft::ptr<nuraft::rpc_client> create_client(const std::string & endpoint) override
    {
        int target_id = 0;
        try
        {
            if (!endpoint.empty())
                target_id = std::stoi(endpoint);
        }
        catch (...)
        {
        }
        return nuraft::cs_new<SimRpcClient>(net, ++next_id, target_id, pipelining);
    }

private:
    SimNetwork & net;
    bool pipelining;
    uint64_t next_id = 0;
};

} // namespace keeper_sim
