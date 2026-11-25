#pragma once

/// Live single-node ClickHouse Keeper server/dispatcher/TCP/HTTP/4LW integration tests.
/// Executed in an isolated child process (`--live-server-tests`, automatically invoked by `--ci`)
/// with virtual POSIX time disabled so real `KeeperDispatcher`, `KeeperRequestDispatcher`,
/// `KeeperRequestDispatcherOld`, `KeeperServer`, `KeeperStateManager`, `SessionExpiryQueue`,
/// `KeeperReadThreadPool`, `FourLetterCommandFactory` (all non-jemalloc 4LW commands + `rcfg`),
/// `KeeperOverDispatcher`, `KeeperTCPHandler` (plain + LZ4 compression + XID64 + 4LW + limits),
/// and `KeeperHTTP*` (`/ready`, `/dashboard/content`, `/api/v1/commands`, `/api/v1/storage`)
/// are exercised end-to-end on ephemeral loopback ports in ~0.5-1.5s!

#include <SimClock.h>

#include <Common/Macros.h>
#include <Common/MultiVersion.h>
#include <Common/ZooKeeper/KeeperOverDispatcher.h>
#include <Common/ZooKeeper/ZooKeeper.h>
#include <Common/ZooKeeper/ZooKeeperArgs.h>
#include <Common/ZooKeeper/ZooKeeperCommon.h>
#include <Common/ZooKeeper/ZooKeeperConstants.h>
#include <Common/ZooKeeper/ZooKeeperIO.h>
#include <Coordination/CoordinationSettings.h>
#include <Coordination/FourLetterCommand.h>
#include <Coordination/KeeperAsynchronousMetrics.h>
#include <Coordination/KeeperDispatcher.h>
#include <Core/ServerUUID.h>
#include <IO/WriteBufferFromString.h>
#include <Server/CloudPlacementInfo.h>
#include <Server/HTTP/HTTPContext.h>
#include <Server/HTTP/HTTPServer.h>
#include <Server/HTTPHandlerFactory.h>
#include <Server/KeeperDashboardRequestHandler.h>
#include <Server/KeeperHTTPHandlerFactory.h>
#include <Server/KeeperHTTPStorageHandler.h>
#include <Server/KeeperJemallocHandler.h>
#include <Server/KeeperTCPHandlerFactory.h>
#include <Server/TCPServer.h>

#if USE_SSL
#    include <Poco/Net/Context.h>
#    include <Poco/Net/SSLManager.h>
#    include <Poco/Net/SecureServerSocket.h>
#    include <openssl/evp.h>
#    include <openssl/pem.h>
#    include <openssl/x509.h>
#endif

#include <Poco/Event.h>
#include <Poco/JSON/Array.h>
#include <Poco/JSON/Object.h>
#include <Poco/JSON/Parser.h>
#include <Poco/Net/HTTPClientSession.h>
#include <Poco/Net/HTTPRequest.h>
#include <Poco/Net/HTTPResponse.h>
#include <Poco/Net/HTTPServerParams.h>
#include <Poco/Net/ServerSocket.h>
#include <Poco/Net/SocketAddress.h>
#include <Poco/Net/StreamSocket.h>
#include <Poco/Net/TCPServerParams.h>
#include <Poco/StreamCopier.h>
#include <Poco/ThreadPool.h>
#include <Poco/Util/XMLConfiguration.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <future>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

namespace keeper_sim
{

namespace live_server_detail
{

inline uint16_t pickFreeLoopbackPort()
{
    Poco::Net::ServerSocket probe(Poco::Net::SocketAddress("127.0.0.1", 0));
    const uint16_t port = probe.address().port();
    probe.close();
    return port;
}

inline Poco::AutoPtr<Poco::Util::XMLConfiguration> makeLiveKeeperConfig(
    const std::filesystem::path & base_dir,
    uint16_t tcp_port,
    uint16_t raft_port,
    bool use_new_dispatcher)
{
    std::filesystem::create_directories(base_dir / "logs");
    std::filesystem::create_directories(base_dir / "snapshots");

    std::ostringstream xml;
    xml << "<clickhouse><keeper_server>\n"
        << "  <tcp_port>" << tcp_port << "</tcp_port>\n"
        << "  <server_id>1</server_id>\n"
        << "  <enable_ipv6>false</enable_ipv6>\n"
        << "  <log_storage_path>" << (base_dir / "logs").string() << "</log_storage_path>\n"
        << "  <snapshot_storage_path>" << (base_dir / "snapshots").string() << "</snapshot_storage_path>\n"
        << "  <enable_reconfiguration>true</enable_reconfiguration>\n"
        << "  <four_letter_word_white_list>*</four_letter_word_white_list>\n"
        << "  <superdigest>" << DB::KeeperStorage::generateDigest("super:simpass") << "</superdigest>\n"
        << "  <coordination_settings>\n"
        << "    <operation_timeout_ms>5000</operation_timeout_ms>\n"
        << "    <startup_timeout>5000</startup_timeout>\n"
        << "    <session_timeout_ms>10000</session_timeout_ms>\n"
        << "    <min_session_timeout_ms>1000</min_session_timeout_ms>\n"
        << "    <raft_logs_level>error</raft_logs_level>\n"
        << "    <heart_beat_interval_ms>100</heart_beat_interval_ms>\n"
        << "    <election_timeout_lower_bound_ms>200</election_timeout_lower_bound_ms>\n"
        << "    <election_timeout_upper_bound_ms>400</election_timeout_upper_bound_ms>\n"
        << "    <snapshot_distance>256</snapshot_distance>\n"
        << "    <dead_session_check_period_ms>100</dead_session_check_period_ms>\n"
        << "    <ttl_gc_period_ms>100</ttl_gc_period_ms>\n"
        << "    <container_gc_period_ms>100</container_gc_period_ms>\n"
        << "    <parallel_read_threads>2</parallel_read_threads>\n"
        << "    <parallel_read_chunk_size>2</parallel_read_chunk_size>\n"
        << "    <parallel_read_min_batch>2</parallel_read_min_batch>\n"
        << "    <optimize_read_order>true</optimize_read_order>\n"
        << "    <use_new_dispatcher>" << (use_new_dispatcher ? "true" : "false") << "</use_new_dispatcher>\n"
        << "    <max_request_size>65536</max_request_size>\n"
        << "    <use_xid_64>true</use_xid_64>\n"
        << "  </coordination_settings>\n"
        << "  <raft_configuration>\n"
        << "    <server>\n"
        << "      <id>1</id>\n"
        << "      <hostname>127.0.0.1</hostname>\n"
        << "      <port>" << raft_port << "</port>\n"
        << "    </server>\n"
        << "  </raft_configuration>\n"
        << "</keeper_server></clickhouse>\n";

    std::istringstream iss(xml.str());
    return new Poco::Util::XMLConfiguration(iss);
}

inline void waitUntilActiveLeader(DB::KeeperDispatcher & dispatcher, int timeout_ms = 5000)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (dispatcher.checkInit() && dispatcher.isServerActive() && dispatcher.isLeader())
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    require(false, "Live KeeperDispatcher did not become active leader within timeout");
}

inline std::string sendRawTCPFourLetterWord(uint16_t port, const std::string & cmd)
{
    Poco::Net::StreamSocket sock(Poco::Net::SocketAddress("127.0.0.1", port));
    sock.setReceiveTimeout(Poco::Timespan(5, 0));
    sock.setSendTimeout(Poco::Timespan(5, 0));
    const int sent = sock.sendBytes(cmd.data(), static_cast<int>(cmd.size()));
    require(sent == static_cast<int>(cmd.size()), "TCP 4LW short send for " + cmd);
    std::string out;
    char buf[4096];
    for (;;)
    {
        const int n = sock.receiveBytes(buf, sizeof(buf));
        if (n <= 0)
            break;
        out.append(buf, static_cast<size_t>(n));
    }
    sock.close();
    return out;
}

struct LiveHTTPResult
{
    int status = 0;
    std::string body;
};

inline LiveHTTPResult sendLoopbackHTTP(
    uint16_t port,
    const std::string & method,
    const std::string & uri,
    const std::string & body = {})
{
    Poco::Net::HTTPClientSession session("127.0.0.1", port);
    session.setTimeout(Poco::Timespan(5, 0));
    Poco::Net::HTTPRequest req(method, uri, Poco::Net::HTTPMessage::HTTP_1_1);
    if (!body.empty())
    {
        req.setContentLength(body.size());
        req.setContentType("text/plain");
    }
    std::ostream & os = session.sendRequest(req);
    if (!body.empty())
        os.write(body.data(), static_cast<std::streamsize>(body.size()));

    Poco::Net::HTTPResponse resp;
    std::istream & is = session.receiveResponse(resp);
    std::ostringstream oss;
    Poco::StreamCopier::copyStream(is, oss);
    return LiveHTTPResult{static_cast<int>(resp.getStatus()), oss.str()};
}

struct SimHTTPContext final : public DB::IHTTPContext
{
    [[nodiscard]] uint64_t getMaxHstsAge() const override { return 0; }
    [[nodiscard]] uint64_t getMaxUriSize() const override { return 65536; }
    [[nodiscard]] uint64_t getMaxFields() const override { return 1000; }
    [[nodiscard]] uint64_t getMaxFieldNameSize() const override { return 4096; }
    [[nodiscard]] uint64_t getMaxFieldValueSize() const override { return 16384; }
    [[nodiscard]] uint64_t getMaxRequestHeaderSize() const override { return 65536; }
    [[nodiscard]] Poco::Timespan getHeadersReadTimeout() const override { return Poco::Timespan(5, 0); }
    [[nodiscard]] Poco::Timespan getReceiveTimeout() const override { return Poco::Timespan(5, 0); }
    [[nodiscard]] Poco::Timespan getSendTimeout() const override { return Poco::Timespan(5, 0); }
};

/// 1 + 2 + 3: Live `KeeperDispatcher` (`use_new_dispatcher = true`, `optimize_read_order = true`,
/// `parallel_read_threads = 2`), ALL non-jemalloc 4LW commands + `rcfg`, `OpNum::Reconfig`,
/// `KeeperOverDispatcher`, real `KeeperTCPHandler` (raw TCP 4LW, plain + LZ4 + XID64 clients,
/// `max_request_size` enforcement), AND real HTTP handlers (`/ready`, `/dashboard/content`,
/// `/api/v1/commands`, `/api/v1/storage`)!
inline void testLiveDispatcherTcpHttpAnd4LW(const std::filesystem::path & root_dir)
{
    const auto base_dir = root_dir / "live_new_dispatcher";
    const uint16_t raft_port = pickFreeLoopbackPort();

    /// Bind ephemeral loopback sockets BEFORE initializing KeeperDispatcher so ports are collision-free.
    Poco::Net::ServerSocket tcp_socket(Poco::Net::SocketAddress("127.0.0.1", 0));
    const uint16_t tcp_port = tcp_socket.address().port();
    Poco::Net::ServerSocket http_socket(Poco::Net::SocketAddress("127.0.0.1", 0));
    const uint16_t http_port = http_socket.address().port();

    auto config = makeLiveKeeperConfig(base_dir, tcp_port, raft_port, /*use_new_dispatcher=*/true);
    DB::PlacementInfo::PlacementInfo::instance().initialize(*config);
    auto macros = std::make_shared<const DB::Macros>();

    auto dispatcher = std::make_shared<DB::KeeperDispatcher>();
    dispatcher->initialize(*config, /*standalone_keeper=*/true, /*start_async=*/false, macros);
    waitUntilActiveLeader(*dispatcher);

    /// ---- Part A: Direct KeeperDispatcher + KeeperRequestDispatcher (`optimize_read_order`, batching) ----
    {
        const int64_t sid1 = dispatcher->getSessionID(10000);
        const int64_t sid2 = dispatcher->getSessionID(10000);
        require(sid1 > 0 && sid2 > 0 && sid1 != sid2, "Live KeeperDispatcher getSessionID failed");

        std::mutex resp_mu;
        std::condition_variable resp_cv;
        std::unordered_map<int64_t, Coordination::ZooKeeperResponsePtr> s1_resps;
        std::unordered_map<int64_t, Coordination::ZooKeeperResponsePtr> s2_resps;
        std::atomic<int> watch_hits{0};

        dispatcher->registerSession(sid1, [&](const Coordination::ZooKeeperResponsePtr & resp, Coordination::ZooKeeperRequestPtr) -> bool
        {
            if (resp->xid == Coordination::WATCH_XID)
            {
                watch_hits.fetch_add(1);
                resp_cv.notify_all();
                return true;
            }
            {
                std::lock_guard lk(resp_mu);
                s1_resps[resp->xid] = resp;
            }
            resp_cv.notify_all();
            return true;
        });
        dispatcher->registerSession(sid2, [&](const Coordination::ZooKeeperResponsePtr & resp, Coordination::ZooKeeperRequestPtr) -> bool
        {
            if (resp->xid == Coordination::WATCH_XID)
                return true;
            {
                std::lock_guard lk(resp_mu);
                s2_resps[resp->xid] = resp;
            }
            resp_cv.notify_all();
            return true;
        });

        auto wait_resp = [&](std::unordered_map<int64_t, Coordination::ZooKeeperResponsePtr> & map, int64_t xid)
        {
            std::unique_lock lk(resp_mu);
            const bool ok = resp_cv.wait_for(lk, std::chrono::seconds(5), [&]() { return map.contains(xid); });
            require(ok, "Timed out waiting for live dispatcher response xid=" + std::to_string(xid));
            return map.at(xid);
        };

        /// Create base node + pipelined interleaved reads & writes to exercise `optimize_read_order` + `KeeperReadThreadPool`.
        {
            auto c = std::make_shared<Coordination::ZooKeeperCreateRequest>();
            c->xid = 1;
            c->path = "/disp";
            c->data = "v0";
            c->acls = {{Coordination::ACL::All, "world", "anyone"}};
            require(dispatcher->putRequest(c, sid1, /*use_xid_64=*/false), "putRequest create /disp failed");
            auto r = wait_resp(s1_resps, 1);
            require(r->error == Coordination::Error::ZOK, "create /disp failed");
        }
        /// Pipeline 6 concurrent reads across sid1 & sid2 (including a watch on /disp), followed by a write!
        for (int i = 0; i < 6; ++i)
        {
            auto g = std::make_shared<Coordination::ZooKeeperGetRequest>();
            g->xid = 10 + i;
            g->path = "/disp";
            g->has_watch = (i == 0);
            require(dispatcher->putRequest(g, (i % 2 == 0) ? sid1 : sid2, false), "putRequest pipelined get failed");
        }
        {
            auto s = std::make_shared<Coordination::ZooKeeperSetRequest>();
            s->xid = 20;
            s->path = "/disp";
            s->data = "v1";
            s->version = 0;
            require(dispatcher->putRequest(s, sid2, false), "putRequest set /disp failed");
        }
        for (int i = 0; i < 6; ++i)
        {
            auto r = wait_resp((i % 2 == 0) ? s1_resps : s2_resps, 10 + i);
            require(r->error == Coordination::Error::ZOK, "pipelined get failed");
        }
        {
            auto r = wait_resp(s2_resps, 20);
            require(r->error == Coordination::Error::ZOK, "set /disp failed");
        }
        {
            std::unique_lock lk(resp_mu);
            const bool got_watch = resp_cv.wait_for(lk, std::chrono::seconds(5), [&]() { return watch_hits.load() >= 1; });
            require(got_watch, "Live dispatcher did not fire watch on /disp");
        }

        /// Test `OpNum::Reconfig` through real `KeeperStateMachine::processReconfiguration`:
        /// 1. Non -1 version -> ZBADVERSION
        {
            auto rc = std::make_shared<Coordination::ZooKeeperReconfigRequest>();
            rc->xid = 30;
            rc->joining = "server.1=127.0.0.1:" + std::to_string(raft_port) + ";participant;2";
            rc->version = 5;
            require(dispatcher->putRequest(rc, sid1, false), "putRequest reconfig bad version failed");
            auto r = wait_resp(s1_resps, 30);
            require(r->error == Coordination::Error::ZBADVERSION, "Expected ZBADVERSION on reconfig version=5");
        }
        /// 2. Valid priority update on server 1 via OpNum::Reconfig -> ZOK + serialized config!
        {
            auto rc = std::make_shared<Coordination::ZooKeeperReconfigRequest>();
            rc->xid = 31;
            rc->joining = "server.1=127.0.0.1:" + std::to_string(raft_port) + ";participant;2";
            rc->version = -1;
            require(dispatcher->putRequest(rc, sid1, false), "putRequest reconfig priority update failed");
            auto r = wait_resp(s1_resps, 31);
            require(r->error == Coordination::Error::ZOK, "Expected ZOK on reconfig priority update");
            auto * rc_resp = dynamic_cast<Coordination::ZooKeeperReconfigResponse *>(r.get());
            require(rc_resp && rc_resp->value.find("server.1=") != std::string::npos, "Missing server.1 in reconfig response");
        }

        /// Test snapshot creation + introspection APIs + dynamic config reload.
        const uint64_t snap_idx = dispatcher->createSnapshot();
        require(snap_idx > 0, "Live dispatcher createSnapshot returned 0");
        const auto log_info = dispatcher->getKeeperLogInfo();
        require(log_info.last_committed_log_idx >= snap_idx, "getKeeperLogInfo last_committed_log_idx < snap_idx");
        const auto members = dispatcher->getClusterMembersInfo();
        require(members.size() == 1 && members[0].server_id == 1, "getClusterMembersInfo mismatch");
        const auto changelogs = dispatcher->getChangelogsStatus();
        require(!changelogs.empty(), "getChangelogsStatus empty");
        dispatcher->updateConfiguration(*config, macros);

        /// Exercise `KeeperAsynchronousMetrics` collection (`updateKeeperInformation`).
        {
            DB::AsynchronousMetricValues async_metrics;
            DB::updateKeeperInformation(*dispatcher, async_metrics);
            require(async_metrics.contains("KeeperZnodeCount")
                && async_metrics.at("KeeperZnodeCount").value >= 1.0
                && async_metrics.contains("KeeperLastCommittedLogIdx")
                && async_metrics.contains("KeeperIsStandalone"),
                "updateKeeperInformation missing core Keeper async metrics");
        }

        /// Exercise `max_memory_usage_soft_limit` (`ZOUTOFMEMORY` on memory-increasing write, reads still succeed!).
        {
            std::istringstream soft_xml("<clickhouse><keeper_server><max_memory_usage_soft_limit>1</max_memory_usage_soft_limit></keeper_server></clickhouse>");
            Poco::AutoPtr<Poco::Util::XMLConfiguration> soft_cfg(new Poco::Util::XMLConfiguration(soft_xml));
            dispatcher->getKeeperContext()->updateKeeperMemorySoftLimit(*soft_cfg);
            require(dispatcher->isExceedingMemorySoftLimit(), "Expected isExceedingMemorySoftLimit() == true when limit=1");

            auto c_oom = std::make_shared<Coordination::ZooKeeperCreateRequest>();
            c_oom->xid = 40;
            c_oom->path = "/oom_rejected";
            c_oom->data = "nope";
            c_oom->acls = {{Coordination::ACL::All, "world", "anyone"}};
            require(dispatcher->putRequest(c_oom, sid1, false), "putRequest oom create failed");
            auto r_oom = wait_resp(s1_resps, 40);
            require(r_oom->error == Coordination::Error::ZOUTOFMEMORY,
                "Expected ZOUTOFMEMORY under soft limit, got " + std::to_string(static_cast<int>(r_oom->error)));

            auto g_ok = std::make_shared<Coordination::ZooKeeperGetRequest>();
            g_ok->xid = 41;
            g_ok->path = "/disp";
            require(dispatcher->putRequest(g_ok, sid1, false), "putRequest read under soft limit failed");
            require(wait_resp(s1_resps, 41)->error == Coordination::Error::ZOK, "Reads should still succeed under memory soft limit");

            /// Restore unlimited soft limit (0).
            std::istringstream unlim_xml("<clickhouse><keeper_server><max_memory_usage_soft_limit>0</max_memory_usage_soft_limit></keeper_server></clickhouse>");
            Poco::AutoPtr<Poco::Util::XMLConfiguration> unlim_cfg(new Poco::Util::XMLConfiguration(unlim_xml));
            dispatcher->getKeeperContext()->updateKeeperMemorySoftLimit(*unlim_cfg);
            require(!dispatcher->isExceedingMemorySoftLimit(), "Expected isExceedingMemorySoftLimit() == false after reset");
        }

        dispatcher->finishSession(sid1);
        dispatcher->finishSession(sid2);
    }
    std::cout << "  [PASS] live: dispatcher_new_reconfig_and_introspection\n";

    /// ---- Part B: ALL non-jemalloc 4LW commands via real FourLetterCommandFactory + `rcfg` ----
    {
        DB::FourLetterCommandFactory::registerCommands(*dispatcher);
        auto & factory = DB::FourLetterCommandFactory::instance();
        factory.initializeAllowList(*dispatcher);

        const std::vector<std::string> cmds = {
            "ruok", "mntr", "srvr", "stat", "conf", "cons",
            "wchs", "wchc", "wchp", "dump", "envi", "dirs",
            "isro", "apiv", "csnp", "lgif", "rclc", "clrs",
            "ftfl", "bpon", "bpof", "pfev", "lgrq", "lgrq",
            "srst", "crst", "rqld", "ydld",
        };
        for (const auto & name : cmds)
        {
            const int32_t code = DB::IFourLetterCommand::toCode(name);
            require(factory.isKnown(code) && factory.isEnabled(code), "4LW command not known/enabled: " + name);
            require(dispatcher->tryBeginFourLetterCommand(), "tryBeginFourLetterCommand failed for " + name);
            const std::string out = factory.get(code)->run();
            dispatcher->finishFourLetterCommand();
            if (name == "ruok")
                require(out == "imok", "ruok expected imok, got: " + out);
            else if (name == "isro")
                require(out == "rw", "isro expected rw, got: " + out);
            else if (name == "mntr")
                require(out.find("zk_server_state\tstandalone") != std::string::npos
                    || out.find("zk_server_state\tleader") != std::string::npos, "mntr missing server state: " + out);
            else if (name == "srvr")
                require(out.find("Mode: standalone") != std::string::npos
                    || out.find("Mode: leader") != std::string::npos, "srvr missing Mode: " + out);
            else if (name == "lgif")
                require(out.find("last_committed_log_idx") != std::string::npos, "lgif missing last_committed_log_idx: " + out);
            else if (name == "ftfl")
                require(out.find("filtered_list") != std::string::npos, "ftfl missing filtered_list");
        }
        /// Test `rcfg` 4LW command with valid preconditions & empty actions, plus invalid precondition rejection.
        {
            const int32_t rcfg_code = DB::IFourLetterCommand::toCode("rcfg");
            require(factory.isKnown(rcfg_code) && factory.supportArguments(rcfg_code), "rcfg not known or missing arg support");
            const std::string ok_json = R"({"preconditions":{"members":[1],"leaders":[1]},"actions":[]})";
            const std::string res = factory.get(rcfg_code)->runWithArgument(ok_json);
            require(!res.empty(), "rcfg valid preconditions returned empty output");

            const std::string bad_json = R"({"preconditions":{"members":[1,2]},"actions":[]})";
            bool bad_rejected = false;
            try
            {
                const std::string bad_res = factory.get(rcfg_code)->runWithArgument(bad_json);
                bad_rejected = (bad_res.find("error") != std::string::npos || bad_res.find("Precondition failed") != std::string::npos);
            }
            catch (const std::exception &)
            {
                bad_rejected = true;
            }
            require(bad_rejected, "rcfg mismatched precondition should fail");
        }
        /// Test `KeeperOverDispatcher` direct in-process `zkutil::ZooKeeper` client!
        {
            auto zk_over_disp = zkutil::ZooKeeper::createFromImpl([dispatcher]()
            {
                return std::make_unique<Coordination::KeeperOverDispatcher>(dispatcher, Poco::Timespan(10, 0));
            });
            zk_over_disp->create("/over_disp", "inproc-val", zkutil::CreateMode::Persistent);
            require(zk_over_disp->get("/over_disp") == "inproc-val", "KeeperOverDispatcher get mismatch");
            zk_over_disp->set("/over_disp", "inproc-v2");
            require(zk_over_disp->get("/over_disp") == "inproc-v2", "KeeperOverDispatcher set mismatch");
            zk_over_disp->remove("/over_disp");
            require(!zk_over_disp->exists("/over_disp"), "KeeperOverDispatcher remove failed");
        }
    }
    std::cout << "  [PASS] live: all_27_four_letter_words_rcfg_and_over_dispatcher\n";

    /// ---- Part C: Real `KeeperTCPHandler` over loopback TCP (4LW, plain, LZ4+XID64, max_request_size) ----
    Poco::ThreadPool server_pool(2, 16);
    {
        auto tcp_factory = Poco::SharedPtr<DB::TCPServerConnectionFactory>(new DB::KeeperTCPHandlerFactory(
            [config]() -> const Poco::Util::AbstractConfiguration & { return *config; },
            dispatcher,
            /*receive_timeout_seconds=*/5,
            /*send_timeout_seconds=*/5,
            /*secure=*/false));
        Poco::Net::TCPServerParams::Ptr tcp_params = new Poco::Net::TCPServerParams();
        tcp_params->setMaxThreads(8);
        DB::TCPServer tcp_server(tcp_factory, server_pool, tcp_socket, tcp_params);
        tcp_server.start();

        /// 1. Raw TCP 4LW commands over loopback socket.
        require(sendRawTCPFourLetterWord(tcp_port, "ruok") == "imok", "TCP ruok failed");
        require(sendRawTCPFourLetterWord(tcp_port, "isro") == "rw", "TCP isro failed");
        {
            const auto mntr = sendRawTCPFourLetterWord(tcp_port, "mntr");
            require(mntr.find("zk_server_state\tstandalone") != std::string::npos
                || mntr.find("zk_server_state\tleader") != std::string::npos, "TCP mntr failed");
        }
        require(sendRawTCPFourLetterWord(tcp_port, "dirs").find("snapshot_dir_size") != std::string::npos, "TCP dirs failed");

        /// 2. Real `zkutil::ZooKeeper` TCP client — PLAIN protocol (no compression, 32-bit XID).
        {
            zkutil::ZooKeeperArgs args("127.0.0.1:" + std::to_string(tcp_port));
            args.connection_timeout_ms = 5000;
            args.session_timeout_ms = 10000;
            args.operation_timeout_ms = 5000;
            args.use_compression = false;
            args.use_xid_64 = false;
            auto zk = zkutil::ZooKeeper::create(args, nullptr, nullptr);

            zk->create("/tcp_plain", "hello-plain", zkutil::CreateMode::Persistent);
            auto watch_ev = std::make_shared<Poco::Event>();
            require(zk->get("/tcp_plain", nullptr, watch_ev) == "hello-plain", "TCP plain get mismatch");
            zk->set("/tcp_plain", "hello-plain-2");
            require(watch_ev->tryWait(3000), "TCP plain watch did not fire");

            /// Ephemeral node + Multi + MultiRead over real TCP.
            zk->create("/tcp_eph", "e1", zkutil::CreateMode::Ephemeral);
            Coordination::Requests mops;
            mops.push_back(zkutil::makeCreateRequest("/tcp_m1", "m1", zkutil::CreateMode::Persistent));
            mops.push_back(zkutil::makeSetRequest("/tcp_plain", "hello-plain-3", -1));
            auto mres = zk->multi(mops);
            require(mres.size() == 2 && mres[0]->error == Coordination::Error::ZOK, "TCP plain multi failed");
            auto gets = zk->tryGet({"/tcp_plain", "/tcp_m1"});
            require(gets.size() == 2 && gets[0].data == "hello-plain-3" && gets[1].data == "m1", "TCP plain multiRead failed");
        }

        /// 3. Real `zkutil::ZooKeeper` TCP client — LZ4 COMPRESSION + 64-BIT XIDs!
        {
            zkutil::ZooKeeperArgs args("127.0.0.1:" + std::to_string(tcp_port));
            args.connection_timeout_ms = 5000;
            args.session_timeout_ms = 10000;
            args.operation_timeout_ms = 5000;
            args.use_compression = true;
            args.use_xid_64 = true;
            auto zk = zkutil::ZooKeeper::create(args, nullptr, nullptr);

            const std::string big_val(2048, 'Z');
            zk->create("/tcp_lz4", big_val, zkutil::CreateMode::Persistent);
            require(zk->get("/tcp_lz4") == big_val, "TCP LZ4+XID64 get mismatch");
            zk->set("/tcp_lz4", "lz4-updated");
            require(zk->get("/tcp_lz4") == "lz4-updated", "TCP LZ4+XID64 set mismatch");
            auto children = zk->getChildren("/");
            require(!children.empty(), "TCP LZ4+XID64 list failed");
            zk->remove("/tcp_lz4");
        }

        /// 4. Oversized request (> max_request_size = 65536) is cleanly rejected without crashing KeeperTCPHandler.
        {
            Poco::Net::StreamSocket sock(Poco::Net::SocketAddress("127.0.0.1", tcp_port));
            sock.setReceiveTimeout(Poco::Timespan(2, 0));
            sock.setSendTimeout(Poco::Timespan(2, 0));
            /// Send ZooKeeper handshake first (connect request length=44).
            DB::WriteBufferFromOwnString hs;
            Coordination::write(static_cast<int32_t>(44), hs);
            Coordination::write(static_cast<int32_t>(0), hs);        // protocolVersion
            Coordination::write(static_cast<int64_t>(0), hs);        // lastZxidSeen
            Coordination::write(static_cast<int32_t>(5000), hs);     // timeOut
            Coordination::write(static_cast<int64_t>(0), hs);        // sessionId
            Coordination::write(std::string(16, '\0'), hs);          // passwd
            const std::string hs_str = hs.str();
            sock.sendBytes(hs_str.data(), static_cast<int>(hs_str.size()));
            char hbuf[256];
            const int hn = sock.receiveBytes(hbuf, sizeof(hbuf));
            require(hn > 0, "TCP handshake response missing");

            /// Now send a frame declaring length = 100000 (> max_request_size=65536).
            DB::WriteBufferFromOwnString big;
            Coordination::write(static_cast<int32_t>(100000), big);
            const std::string big_str = big.str();
            sock.sendBytes(big_str.data(), static_cast<int>(big_str.size()));
            /// Server should close the connection cleanly (0 bytes or error), and remain healthy for subsequent 4LW!
            try
            {
                char dummy[64];
                (void)sock.receiveBytes(dummy, sizeof(dummy));
            }
            catch (...) {}
            sock.close();
            require(sendRawTCPFourLetterWord(tcp_port, "ruok") == "imok", "Server unhealthy after oversized request");
        }

        tcp_server.stop();
    }
    std::cout << "  [PASS] live: tcp_handler_plain_lz4_xid64_4lw_and_limits\n";

    /// ---- Part D: Real HTTP handlers (`/ready`, `/dashboard/content`, `/api/v1/commands`, `/api/v1/storage`) ----
    {
        auto keeper_http_client = std::make_shared<DB::KeeperHTTPClient>([dispatcher]() -> std::shared_ptr<zkutil::ZooKeeper>
        {
            return zkutil::ZooKeeper::createFromImpl([dispatcher]()
            {
                return std::make_unique<Coordination::KeeperOverDispatcher>(dispatcher, Poco::Timespan(10, 0));
            });
        });

        auto main_factory = std::make_shared<DB::KeeperHTTPRequestHandlerFactory>("SimKeeperHTTP");
        {
            auto h = std::make_shared<DB::HandlingRuleHTTPHandlerFactory<DB::KeeperHTTPReadinessHandler>>(
                [dispatcher]() { return std::make_unique<DB::KeeperHTTPReadinessHandler>(dispatcher); });
            h->attachStrictPath("/ready");
            h->allowGetAndHeadRequest();
            main_factory->addHandler(h);
        }
        {
            auto h = std::make_shared<DB::HandlingRuleHTTPHandlerFactory<DB::KeeperDashboardWebUIRequestHandler>>(
                []() { return std::make_unique<DB::KeeperDashboardWebUIRequestHandler>(); });
            h->attachStrictPath("/dashboard");
            h->allowGetAndHeadRequest();
            main_factory->addPathToHints("/dashboard");
            main_factory->addHandler(h);
        }
        {
            auto h = std::make_shared<DB::HandlingRuleHTTPHandlerFactory<DB::KeeperDashboardContentRequestHandler>>(
                [dispatcher]() { return std::make_unique<DB::KeeperDashboardContentRequestHandler>(dispatcher); });
            h->attachStrictPath("/dashboard/content");
            h->allowGetAndHeadRequest();
            main_factory->addHandler(h);
        }
        {
            auto h = std::make_shared<DB::HandlingRuleHTTPHandlerFactory<DB::KeeperJemallocWebUIHandler>>(
                []() { return std::make_unique<DB::KeeperJemallocWebUIHandler>(); });
            h->attachStrictPath("/jemalloc");
            h->allowGetAndHeadRequest();
            main_factory->addPathToHints("/jemalloc");
            main_factory->addHandler(h);
        }
        {
            auto h = std::make_shared<DB::HandlingRuleHTTPHandlerFactory<DB::KeeperJemallocRedirectHandler>>(
                []() { return std::make_unique<DB::KeeperJemallocRedirectHandler>(); });
            h->attachStrictPath("/jemalloc/");
            h->allowGetAndHeadRequest();
            main_factory->addHandler(h);
        }
        {
#if USE_JEMALLOC
            auto h = std::make_shared<DB::HandlingRuleHTTPHandlerFactory<DB::KeeperJemallocStatusHandler>>(
                []() { return std::make_unique<DB::KeeperJemallocStatusHandler>(); });
#else
            auto h = std::make_shared<DB::HandlingRuleHTTPHandlerFactory<DB::KeeperJemallocNotAvailableHandler>>(
                []() { return std::make_unique<DB::KeeperJemallocNotAvailableHandler>(); });
#endif
            h->attachStrictPath("/jemalloc/status");
            h->allowGetAndHeadRequest();
            main_factory->addHandler(h);
        }
        {
            auto h = std::make_shared<DB::HandlingRuleHTTPHandlerFactory<DB::KeeperHTTPCommandsHandler>>(
                [dispatcher, keeper_http_client]() { return std::make_unique<DB::KeeperHTTPCommandsHandler>(dispatcher, keeper_http_client); });
            h->attachNonStrictPath("/api/v1/commands");
            h->allowRESTMethods();
            main_factory->addHandler(h);
        }
        {
            auto h = std::make_shared<DB::HandlingRuleHTTPHandlerFactory<DB::KeeperHTTPStorageHandler>>(
                [keeper_http_client, ctx = dispatcher->getKeeperContext()]()
                { return std::make_unique<DB::KeeperHTTPStorageHandler>(keeper_http_client, ctx); });
            h->attachNonStrictPath("/api/v1/storage");
            h->allowRESTMethods();
            main_factory->addHandler(h);
        }

        auto http_ctx = std::make_shared<SimHTTPContext>();
        Poco::Net::HTTPServerParams::Ptr http_params = new Poco::Net::HTTPServerParams();
        http_params->setMaxThreads(4);
        DB::HTTPServer http_server(http_ctx, main_factory, server_pool, http_socket, http_params);
        http_server.start();

        /// 1. Readiness endpoint `/ready`
        {
            auto r = sendLoopbackHTTP(http_port, "GET", "/ready");
            require(r.status == 200 && r.body.find("\"status\":\"ok\"") != std::string::npos
                && (r.body.find("\"role\":\"standalone\"") != std::string::npos || r.body.find("\"role\":\"leader\"") != std::string::npos),
                "HTTP /ready failed: status=" + std::to_string(r.status) + " body=" + r.body);
        }
        /// 2. Dashboard HTML (`/dashboard`), Dashboard JSON (`/dashboard/content`), Jemalloc UI/Status, and 404 hints
        {
            auto r_ui = sendLoopbackHTTP(http_port, "GET", "/dashboard");
            require(r_ui.status == 200 && !r_ui.body.empty(), "HTTP /dashboard failed");
            auto r = sendLoopbackHTTP(http_port, "GET", "/dashboard/content");
            require(r.status == 200 && !r.body.empty(), "HTTP /dashboard/content failed");
            auto r_jm = sendLoopbackHTTP(http_port, "GET", "/jemalloc");
            require(r_jm.status == 200 && !r_jm.body.empty(), "HTTP /jemalloc failed");
            auto r_jm_redir = sendLoopbackHTTP(http_port, "GET", "/jemalloc/");
            require(r_jm_redir.status == 301 || r_jm_redir.status == 302, "HTTP /jemalloc/ redirect failed");
            auto r_jm_st = sendLoopbackHTTP(http_port, "GET", "/jemalloc/status");
            require(r_jm_st.status == 200 || r_jm_st.status == 501, "HTTP /jemalloc/status unexpected status");
            auto r_404 = sendLoopbackHTTP(http_port, "GET", "/dashboar");
            require(r_404.status == 404 && r_404.body.find("/dashboard") != std::string::npos,
                "HTTP KeeperNotFoundHandler 404 hints failed: status=" + std::to_string(r_404.status));
        }
        /// 3. Commands endpoint `/api/v1/commands` (both 4LW `ruok` and KeeperClientCLI `ls /`)
        {
            auto r1 = sendLoopbackHTTP(http_port, "GET", "/api/v1/commands?command=ruok");
            require(r1.status == 200 && r1.body.find("imok") != std::string::npos,
                "HTTP /api/v1/commands ruok failed: " + r1.body);
            auto r2 = sendLoopbackHTTP(http_port, "GET", "/api/v1/commands?command=ls%20/");
            require(r2.status == 200 && r2.body.find("keeper") != std::string::npos,
                "HTTP /api/v1/commands ls failed: " + r2.body);
        }
        /// 4. Storage REST CRUD `/api/v1/storage` (POST -> GET -> PUT -> GET?children=true -> DELETE)
        {
            auto r_post = sendLoopbackHTTP(http_port, "POST", "/api/v1/storage/http_key", "http-v1");
            require(r_post.status == 201, "HTTP POST /api/v1/storage/http_key expected 201, got " + std::to_string(r_post.status));

            auto r_get = sendLoopbackHTTP(http_port, "GET", "/api/v1/storage/http_key");
            require(r_get.status == 200 && r_get.body.find("http-v1") != std::string::npos,
                "HTTP GET /api/v1/storage/http_key failed: " + r_get.body);

            auto r_put = sendLoopbackHTTP(http_port, "PUT", "/api/v1/storage/http_key?version=0", "http-v2");
            require(r_put.status == 200, "HTTP PUT /api/v1/storage/http_key expected 200, got " + std::to_string(r_put.status));

            auto r_list = sendLoopbackHTTP(http_port, "GET", "/api/v1/storage?children=true");
            require(r_list.status == 200 && r_list.body.find("http_key") != std::string::npos,
                "HTTP GET /api/v1/storage?children=true failed: " + r_list.body);

            auto r_del = sendLoopbackHTTP(http_port, "DELETE", "/api/v1/storage/http_key?version=1");
            require(r_del.status == 204, "HTTP DELETE /api/v1/storage/http_key expected 204, got " + std::to_string(r_del.status));

            auto r_gone = sendLoopbackHTTP(http_port, "GET", "/api/v1/storage/http_key");
            require(r_gone.status == 404, "HTTP GET deleted key expected 404, got " + std::to_string(r_gone.status));
        }

        http_server.stopAll(/*abort_current=*/true);
    }
    std::cout << "  [PASS] live: http_handlers_readiness_dashboard_commands_and_storage\n";

    dispatcher->signalShutdown();
    dispatcher->beginTCPConnectionDrain();
    dispatcher->shutdownBeforeConnectionsFinish();
    dispatcher->shutdownAfterConnectionsFinish(/*closed_all_connections=*/true);
}

/// 4. Live `KeeperDispatcher` with legacy `KeeperRequestDispatcherOld` (`use_new_dispatcher = false`).
inline void testLiveDispatcherOldLifecycle(const std::filesystem::path & root_dir)
{
    const auto base_dir = root_dir / "live_old_dispatcher";
    const uint16_t tcp_port = pickFreeLoopbackPort();
    const uint16_t raft_port = pickFreeLoopbackPort();
    auto config = makeLiveKeeperConfig(base_dir, tcp_port, raft_port, /*use_new_dispatcher=*/false);
    auto macros = std::make_shared<const DB::Macros>();

    auto dispatcher = std::make_shared<DB::KeeperDispatcher>();
    dispatcher->initialize(*config, /*standalone_keeper=*/true, /*start_async=*/false, macros);
    waitUntilActiveLeader(*dispatcher);

    const int64_t sid = dispatcher->getSessionID(10000);
    require(sid > 0, "Old dispatcher getSessionID failed");

    std::mutex mu;
    std::condition_variable cv;
    std::unordered_map<int64_t, Coordination::ZooKeeperResponsePtr> resps;
    dispatcher->registerSession(sid, [&](const Coordination::ZooKeeperResponsePtr & resp, Coordination::ZooKeeperRequestPtr) -> bool
    {
        if (resp->xid == Coordination::WATCH_XID)
            return true;
        {
            std::lock_guard lk(mu);
            resps[resp->xid] = resp;
        }
        cv.notify_all();
        return true;
    });
    auto wait_xid = [&](int64_t xid)
    {
        std::unique_lock lk(mu);
        require(cv.wait_for(lk, std::chrono::seconds(5), [&]() { return resps.contains(xid); }),
            "Old dispatcher timeout on xid=" + std::to_string(xid));
        return resps.at(xid);
    };

    {
        auto c = std::make_shared<Coordination::ZooKeeperCreateRequest>();
        c->xid = 1;
        c->path = "/old_disp";
        c->data = "legacy-ok";
        c->acls = {{Coordination::ACL::All, "world", "anyone"}};
        require(dispatcher->putRequest(c, sid, false), "Old dispatcher create failed");
        require(wait_xid(1)->error == Coordination::Error::ZOK, "Old dispatcher create ZOK expected");
    }
    {
        auto g = std::make_shared<Coordination::ZooKeeperGetRequest>();
        g->xid = 2;
        g->path = "/old_disp";
        require(dispatcher->putRequest(g, sid, false), "Old dispatcher get failed");
        auto r = wait_xid(2);
        auto * gr = dynamic_cast<Coordination::ZooKeeperGetResponse *>(r.get());
        require(gr && gr->error == Coordination::Error::ZOK && gr->data == "legacy-ok", "Old dispatcher get mismatch");
    }

    dispatcher->finishSession(sid);
    dispatcher->signalShutdown();
    dispatcher->beginTCPConnectionDrain();
    dispatcher->shutdownBeforeConnectionsFinish();
    dispatcher->shutdownAfterConnectionsFinish(/*closed_all_connections=*/true);
    std::cout << "  [PASS] live: dispatcher_old_lifecycle\n";
}

#if USE_SSL
inline void generateSelfSignedCertPem(const std::filesystem::path & cert_path, const std::filesystem::path & key_path)
{
    EVP_PKEY * pkey = EVP_RSA_gen(2048);
    require(pkey != nullptr, "EVP_RSA_gen failed");
    X509 * x509 = X509_new();
    require(x509 != nullptr, "X509_new failed");
    ASN1_INTEGER_set(X509_get_serialNumber(x509), 1);
    X509_gmtime_adj(X509_get_notBefore(x509), 0);
    X509_gmtime_adj(X509_get_notAfter(x509), 86400);
    X509_set_pubkey(x509, pkey);
    X509_NAME * name = X509_get_subject_name(x509);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char *>("127.0.0.1"), -1, -1, 0);
    X509_set_issuer_name(x509, name);
    require(X509_sign(x509, pkey, EVP_sha256()) != 0, "X509_sign failed");

    FILE * fk = fopen(key_path.c_str(), "wb");
    require(fk != nullptr && PEM_write_PrivateKey(fk, pkey, nullptr, nullptr, 0, nullptr, nullptr) == 1, "Write key.pem failed");
    fclose(fk);

    FILE * fc = fopen(cert_path.c_str(), "wb");
    require(fc != nullptr && PEM_write_X509(fc, x509) == 1, "Write cert.pem failed");
    fclose(fc);

    X509_free(x509);
    EVP_PKEY_free(pkey);
}
#endif

/// 5. Live TCP `client_password` authentication (reject wrong password, accept right password)
/// + real TLS/SSL `SecureServerSocket` (`tcp_port_secure` + `secure://127.0.0.1:<port>` client)!
inline void testLiveTCPPasswordAuthAndTLS(const std::filesystem::path & root_dir)
{
    const auto base_dir = root_dir / "live_auth_tls";
    std::filesystem::create_directories(base_dir / "logs");
    std::filesystem::create_directories(base_dir / "snapshots");
    const uint16_t raft_port = pickFreeLoopbackPort();
    Poco::Net::ServerSocket plain_socket(Poco::Net::SocketAddress("127.0.0.1", 0));
    const uint16_t plain_port = plain_socket.address().port();

    std::ostringstream xml;
    xml << "<clickhouse><keeper_server>\n"
        << "  <tcp_port>" << plain_port << "</tcp_port>\n"
        << "  <server_id>1</server_id>\n"
        << "  <enable_ipv6>false</enable_ipv6>\n"
        << "  <client_password>simsecret</client_password>\n"
        << "  <log_storage_path>" << (base_dir / "logs").string() << "</log_storage_path>\n"
        << "  <snapshot_storage_path>" << (base_dir / "snapshots").string() << "</snapshot_storage_path>\n"
        << "  <coordination_settings>\n"
        << "    <operation_timeout_ms>5000</operation_timeout_ms>\n"
        << "    <startup_timeout>5000</startup_timeout>\n"
        << "    <session_timeout_ms>10000</session_timeout_ms>\n"
        << "    <raft_logs_level>error</raft_logs_level>\n"
        << "    <heart_beat_interval_ms>100</heart_beat_interval_ms>\n"
        << "    <election_timeout_lower_bound_ms>200</election_timeout_lower_bound_ms>\n"
        << "    <election_timeout_upper_bound_ms>400</election_timeout_upper_bound_ms>\n"
        << "  </coordination_settings>\n"
        << "  <raft_configuration>\n"
        << "    <server><id>1</id><hostname>127.0.0.1</hostname><port>" << raft_port << "</port></server>\n"
        << "  </raft_configuration>\n"
        << "</keeper_server></clickhouse>\n";
    std::istringstream iss(xml.str());
    Poco::AutoPtr<Poco::Util::XMLConfiguration> config(new Poco::Util::XMLConfiguration(iss));
    auto macros = std::make_shared<const DB::Macros>();

    auto dispatcher = std::make_shared<DB::KeeperDispatcher>();
    dispatcher->initialize(*config, /*standalone_keeper=*/true, /*start_async=*/false, macros);
    waitUntilActiveLeader(*dispatcher);

    Poco::ThreadPool pool(2, 8);
    {
        auto factory = Poco::SharedPtr<DB::TCPServerConnectionFactory>(new DB::KeeperTCPHandlerFactory(
            [config]() -> const Poco::Util::AbstractConfiguration & { return *config; },
            dispatcher, 5, 5, /*secure=*/false));
        Poco::Net::TCPServerParams::Ptr params = new Poco::Net::TCPServerParams();
        params->setMaxThreads(4);
        DB::TCPServer tcp_server(factory, pool, plain_socket, params);
        tcp_server.start();

        /// Wrong password MUST be rejected.
        {
            zkutil::ZooKeeperArgs bad_args("127.0.0.1:" + std::to_string(plain_port));
            bad_args.password = DB::SecretString("wrongpass");
            bad_args.connection_timeout_ms = 2000;
            bad_args.session_timeout_ms = 5000;
            bad_args.operation_timeout_ms = 2000;
            bad_args.num_connection_retries = 0;
            bool rejected = false;
            try
            {
                auto zk = zkutil::ZooKeeper::create(bad_args, nullptr, nullptr);
                (void)zk->exists("/");
            }
            catch (const std::exception &)
            {
                rejected = true;
            }
            require(rejected, "KeeperTCPHandler accepted wrong client_password!");
        }

        /// Correct password MUST succeed.
        {
            zkutil::ZooKeeperArgs ok_args("127.0.0.1:" + std::to_string(plain_port));
            ok_args.password = DB::SecretString("simsecret");
            ok_args.connection_timeout_ms = 5000;
            ok_args.session_timeout_ms = 10000;
            ok_args.operation_timeout_ms = 5000;
            auto zk = zkutil::ZooKeeper::create(ok_args, nullptr, nullptr);
            zk->create("/pw_ok", "auth-pass", zkutil::CreateMode::Persistent);
            require(zk->get("/pw_ok") == "auth-pass", "Password-authenticated TCP get mismatch");
            zk->remove("/pw_ok");
        }
        tcp_server.stop();
    }

#if USE_SSL
    {
        const auto cert_path = base_dir / "cert.pem";
        const auto key_path = base_dir / "key.pem";
        generateSelfSignedCertPem(cert_path, key_path);

        Poco::Net::Context::Ptr server_ctx = new Poco::Net::Context(
            Poco::Net::Context::SERVER_USE,
            key_path.string(),
            cert_path.string(),
            "",
            Poco::Net::Context::VERIFY_NONE);
        Poco::Net::Context::Ptr client_ctx = new Poco::Net::Context(
            Poco::Net::Context::CLIENT_USE,
            "",
            "",
            "",
            Poco::Net::Context::VERIFY_NONE);
        Poco::Net::SSLManager::instance().initializeServer(nullptr, nullptr, server_ctx);
        Poco::Net::SSLManager::instance().initializeClient(nullptr, nullptr, client_ctx);

        Poco::Net::SecureServerSocket tls_socket(Poco::Net::SocketAddress("127.0.0.1", 0), 64, server_ctx);
        const uint16_t tls_port = tls_socket.address().port();

        auto tls_factory = Poco::SharedPtr<DB::TCPServerConnectionFactory>(new DB::KeeperTCPHandlerFactory(
            [config]() -> const Poco::Util::AbstractConfiguration & { return *config; },
            dispatcher, 5, 5, /*secure=*/true));
        Poco::Net::TCPServerParams::Ptr tls_params = new Poco::Net::TCPServerParams();
        tls_params->setMaxThreads(4);
        DB::TCPServer tls_server(tls_factory, pool, tls_socket, tls_params);
        tls_server.start();

        zkutil::ZooKeeperArgs tls_args("secure://127.0.0.1:" + std::to_string(tls_port));
        tls_args.password = DB::SecretString("simsecret");
        tls_args.connection_timeout_ms = 5000;
        tls_args.session_timeout_ms = 10000;
        tls_args.operation_timeout_ms = 5000;
        auto zk_tls = zkutil::ZooKeeper::create(tls_args, nullptr, nullptr);
        zk_tls->create("/tls_ok", "encrypted-tcp", zkutil::CreateMode::Persistent);
        require(zk_tls->get("/tls_ok") == "encrypted-tcp", "TLS secure:// get mismatch");
        zk_tls->remove("/tls_ok");

        tls_server.stop();
    }
#endif

    dispatcher->signalShutdown();
    dispatcher->beginTCPConnectionDrain();
    dispatcher->shutdownBeforeConnectionsFinish();
    dispatcher->shutdownAfterConnectionsFinish(/*closed_all_connections=*/true);
    std::cout << "  [PASS] live: tcp_password_auth_and_tls_secure_socket\n";
}

/// 6. Live 3-node loopback `KeeperDispatcher` cluster:
/// - Quorum election + peer health (`getClusterMembersInfo`, `getKeeper4LWInfo`, `updateKeeperInformation`)
/// - Real follower->leader `client_req_stream` forwarding + follower read-after-write consistency
/// - Real `rcfg` leadership transfer (`transfer_leadership`) to a specific follower
/// - Real leader shutdown -> automatic Raft failover on 2/3 majority
/// - Real `forceRecovery()` (`KeeperServer::enterRecoveryMode`) on 1 surviving node preserving committed data!
inline void testLive3NodeClusterForwardingFailoverAndForceRecovery(const std::filesystem::path & root_dir)
{
    const auto base_dir = root_dir / "live_3node";
    const std::array<uint16_t, 3> raft_ports = {
        pickFreeLoopbackPort(),
        pickFreeLoopbackPort(),
        pickFreeLoopbackPort(),
    };
    const std::array<uint16_t, 3> tcp_ports = {
        pickFreeLoopbackPort(),
        pickFreeLoopbackPort(),
        pickFreeLoopbackPort(),
    };

    auto make_node_cfg = [&](int id, bool single_node_local_only) -> Poco::AutoPtr<Poco::Util::XMLConfiguration>
    {
        const auto ndir = base_dir / ("node_" + std::to_string(id));
        std::filesystem::create_directories(ndir / "logs");
        std::filesystem::create_directories(ndir / "snapshots");

        std::ostringstream xml;
        xml << "<clickhouse><keeper_server>\n"
            << "  <tcp_port>" << tcp_ports[id - 1] << "</tcp_port>\n"
            << "  <server_id>" << id << "</server_id>\n"
            << "  <enable_ipv6>false</enable_ipv6>\n"
            << "  <enable_reconfiguration>true</enable_reconfiguration>\n"
            << "  <log_storage_path>" << (ndir / "logs").string() << "</log_storage_path>\n"
            << "  <snapshot_storage_path>" << (ndir / "snapshots").string() << "</snapshot_storage_path>\n"
            << "  <coordination_settings>\n"
            << "    <operation_timeout_ms>5000</operation_timeout_ms>\n"
            << "    <startup_timeout>5000</startup_timeout>\n"
            << "    <session_timeout_ms>10000</session_timeout_ms>\n"
            << "    <raft_logs_level>error</raft_logs_level>\n"
            << "    <heart_beat_interval_ms>50</heart_beat_interval_ms>\n"
            << "    <election_timeout_lower_bound_ms>150</election_timeout_lower_bound_ms>\n"
            << "    <election_timeout_upper_bound_ms>300</election_timeout_upper_bound_ms>\n"
            << "    <use_new_dispatcher>true</use_new_dispatcher>\n"
            << "    <optimize_read_order>true</optimize_read_order>\n"
            << "  </coordination_settings>\n"
            << "  <raft_configuration>\n";
        if (single_node_local_only)
        {
            xml << "    <server><id>" << id << "</id><hostname>127.0.0.1</hostname><port>" << raft_ports[id - 1] << "</port></server>\n";
        }
        else
        {
            for (int s = 1; s <= 3; ++s)
                xml << "    <server><id>" << s << "</id><hostname>127.0.0.1</hostname><port>" << raft_ports[s - 1] << "</port></server>\n";
        }
        xml << "  </raft_configuration>\n</keeper_server></clickhouse>\n";
        std::istringstream iss(xml.str());
        return new Poco::Util::XMLConfiguration(iss);
    };

    auto macros = std::make_shared<const DB::Macros>();
    std::array<std::shared_ptr<DB::KeeperDispatcher>, 3> nodes;
    for (int i = 0; i < 3; ++i)
    {
        auto cfg = make_node_cfg(i + 1, /*single_node_local_only=*/false);
        nodes[i] = std::make_shared<DB::KeeperDispatcher>();
        nodes[i]->initialize(*cfg, /*standalone_keeper=*/true, /*start_async=*/true, macros);
    }

    auto find_leader_idx = [&]() -> int
    {
        for (int i = 0; i < 3; ++i)
            if (nodes[i] && nodes[i]->isServerActive() && nodes[i]->isLeader())
                return i;
        return -1;
    };

    auto wait_until = [](int timeout_ms, auto && pred, const std::string & msg)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (pred())
                return;
            std::this_thread::sleep_for(std::chrono::milliseconds(15));
        }
        require(false, msg);
    };

    /// Wait for 1 leader + 2 active followers.
    int leader_idx = -1;
    wait_until(8000, [&]()
    {
        leader_idx = find_leader_idx();
        if (leader_idx < 0)
            return false;
        int active_followers = 0;
        for (int i = 0; i < 3; ++i)
            if (i != leader_idx && nodes[i]->isServerActive() && nodes[i]->isFollower())
                ++active_followers;
        return active_followers == 2;
    }, "3-node live cluster did not reach 1 leader + 2 followers");

    const int follower_idx = (leader_idx + 1) % 3;

    /// 1. Follower -> Leader forwarding via real `KeeperRequestDispatcher` (`client_req_stream`) + local follower read!
    {
        auto zk_follower = zkutil::ZooKeeper::createFromImpl([d = nodes[follower_idx]]()
        {
            return std::make_unique<Coordination::KeeperOverDispatcher>(d, Poco::Timespan(10, 0));
        });
        zk_follower->create("/forwarded_from_follower", "fwd-v1", zkutil::CreateMode::Persistent);
        require(zk_follower->get("/forwarded_from_follower") == "fwd-v1", "Follower read-after-write mismatch");

        /// Verify leader sees the write and reports 2 synced followers in 4LW & cluster members info.
        auto zk_leader = zkutil::ZooKeeper::createFromImpl([d = nodes[leader_idx]]()
        {
            return std::make_unique<Coordination::KeeperOverDispatcher>(d, Poco::Timespan(10, 0));
        });
        require(zk_leader->get("/forwarded_from_follower") == "fwd-v1", "Leader did not observe follower-forwarded write");

        wait_until(5000, [&]()
        {
            const auto info = nodes[leader_idx]->getKeeper4LWInfo();
            return info.follower_count == 2 && info.synced_follower_count == 2;
        }, "Leader did not report 2 synced followers");

        const auto members = nodes[leader_idx]->getClusterMembersInfo();
        require(members.size() == 3, "Expected 3 cluster members on leader");
        size_t alive_peers = 0;
        for (const auto & m : members)
            if (!m.is_self && m.is_alive.value_or(false))
                ++alive_peers;
        require(alive_peers == 2, "Expected 2 alive peers in getClusterMembersInfo");

        DB::AsynchronousMetricValues mv;
        DB::updateKeeperInformation(*nodes[leader_idx], mv);
        require(mv.contains("KeeperFollowers") && mv.at("KeeperFollowers").value == 2.0
            && mv.contains("KeeperSyncedFollowers") && mv.at("KeeperSyncedFollowers").value == 2.0
            && mv.contains("KeeperIsLeader") && mv.at("KeeperIsLeader").value == 1.0,
            "Leader async metrics missing KeeperFollowers/KeeperSyncedFollowers/KeeperIsLeader");
    }

    /// 2. Live `rcfg` `set_priority` + `transfer_leadership` to `follower_idx` (`server_id = follower_idx + 1`).
    {
        const int old_leader_id = leader_idx + 1;
        const int target_leader_id = follower_idx + 1;
        std::ostringstream js;
        js << R"({"preconditions":{"members":[1,2,3],"leaders":[)" << old_leader_id
           << R"(]},"actions":[{"set_priority":[{"id":)" << target_leader_id << R"(,"priority":2}]},{"transfer_leadership":[)"
           << target_leader_id << R"(]}]})";
        Poco::JSON::Parser parser;
        auto obj = parser.parse(js.str()).extract<Poco::JSON::Object::Ptr>();
        auto res = nodes[leader_idx]->reconfigureClusterFromReconfigureCommand(obj);
        std::ostringstream res_ss;
        Poco::JSON::Stringifier::stringify(res, res_ss);
        require(!res.isNull() && res_ss.str().find("error") == std::string::npos,
            "rcfg transfer_leadership failed: " + res_ss.str());

        wait_until(8000, [&]() { return find_leader_idx() == follower_idx; },
            "Leadership did not transfer to target follower " + std::to_string(target_leader_id));
        leader_idx = follower_idx;
    }

    /// 3. Leader crash/shutdown -> automatic failover to surviving 2/3 majority -> write `/after_failover`.
    {
        auto stop_node = [&](int idx)
        {
            if (!nodes[idx])
                return;
            nodes[idx]->signalShutdown();
            nodes[idx]->beginTCPConnectionDrain();
            nodes[idx]->shutdownBeforeConnectionsFinish();
            nodes[idx]->shutdownAfterConnectionsFinish(/*closed_all_connections=*/true);
            nodes[idx].reset();
        };

        const int killed_leader = leader_idx;
        stop_node(killed_leader);

        int new_leader = -1;
        wait_until(8000, [&]()
        {
            new_leader = find_leader_idx();
            return new_leader >= 0 && new_leader != killed_leader;
        }, "Remaining 2/3 nodes failed to elect a new leader after failover");

        auto zk_surv = zkutil::ZooKeeper::createFromImpl([d = nodes[new_leader]]()
        {
            return std::make_unique<Coordination::KeeperOverDispatcher>(d, Poco::Timespan(10, 0));
        });
        zk_surv->create("/after_failover", "survived-2of3", zkutil::CreateMode::Persistent);
        require(zk_surv->get("/after_failover") == "survived-2of3", "Write on surviving 2/3 majority failed");

        /// Ensure node 1 (index 0) receives `/after_failover` before shutting down all nodes for forceRecovery test.
        if (!nodes[0])
        {
            /// Node 1 was the killed leader; bring it back briefly to sync `/after_failover`.
            auto cfg1 = make_node_cfg(1, /*single_node_local_only=*/false);
            nodes[0] = std::make_shared<DB::KeeperDispatcher>();
            nodes[0]->initialize(*cfg1, /*standalone_keeper=*/true, /*start_async=*/true, macros);
            wait_until(8000, [&]() { return nodes[0]->isServerActive(); }, "Node 1 failed to rejoin before recovery test");
        }
        wait_until(5000, [&]()
        {
            const uint64_t l_idx = nodes[new_leader]->getKeeperLogInfo().last_committed_log_idx;
            return nodes[0]->getKeeperLogInfo().last_committed_log_idx >= l_idx;
        }, "Node 1 did not catch up to new_leader before forceRecovery");

        /// Shut down ALL 3 nodes cleanly.
        for (int i = 0; i < 3; ++i)
            stop_node(i);
    }

    /// 4. Force Recovery (`forceRecovery()` / `KeeperServer::enterRecoveryMode`) on ONLY node 1!
    {
        auto cfg1_single = make_node_cfg(1, /*single_node_local_only=*/true);
        auto node1 = std::make_shared<DB::KeeperDispatcher>();
        node1->initialize(*cfg1_single, /*standalone_keeper=*/true, /*start_async=*/true, macros);

        /// Persisted 3-node config prevents 1/3 node from electing a leader until `forceRecovery()`.
        std::this_thread::sleep_for(std::chrono::milliseconds(350));
        require(!node1->isLeader(), "Isolated 1/3 node should not be leader before forceRecovery");

        node1->forceRecovery();
        waitUntilActiveLeader(*node1, 8000);

        auto zk_rec = zkutil::ZooKeeper::createFromImpl([node1]()
        {
            return std::make_unique<Coordination::KeeperOverDispatcher>(node1, Poco::Timespan(10, 0));
        });
        require(zk_rec->get("/forwarded_from_follower") == "fwd-v1", "forceRecovery lost /forwarded_from_follower");
        require(zk_rec->get("/after_failover") == "survived-2of3", "forceRecovery lost /after_failover");
        zk_rec->create("/after_force_recovery", "recovered-ok", zkutil::CreateMode::Persistent);
        require(zk_rec->get("/after_force_recovery") == "recovered-ok", "Write after forceRecovery failed");

        node1->signalShutdown();
        node1->beginTCPConnectionDrain();
        node1->shutdownBeforeConnectionsFinish();
        node1->shutdownAfterConnectionsFinish(/*closed_all_connections=*/true);
    }
    std::cout << "  [PASS] live: 3node_forwarding_rcfg_failover_and_force_recovery\n";
}

} // namespace live_server_detail

inline void runKeeperLiveServerTests(const std::filesystem::path & state_dir)
{
    require(!state_dir.empty(), "--state-dir is required for --live-server-tests");
    if (std::filesystem::exists(state_dir))
        std::filesystem::remove_all(state_dir);
    std::filesystem::create_directories(state_dir);

    /// Live server tests exercise real multi-threaded KeeperDispatcher, NuRaft ASIO, TCP, and HTTP,
    /// so virtual POSIX time must stay disabled here.
    g_virtual_time_enabled = false;
    DB::ServerUUID::setRandomForUnitTests();
    std::cout << std::unitbuf;

    live_server_detail::testLiveDispatcherTcpHttpAnd4LW(state_dir);
    live_server_detail::testLiveDispatcherOldLifecycle(state_dir);
    live_server_detail::testLiveTCPPasswordAuthAndTLS(state_dir);
    live_server_detail::testLive3NodeClusterForwardingFailoverAndForceRecovery(state_dir);

    std::error_code ec;
    std::filesystem::remove_all(state_dir, ec);
    std::cout << "PASS keeper-sim-live-server-tests\n";
}

} // namespace keeper_sim
