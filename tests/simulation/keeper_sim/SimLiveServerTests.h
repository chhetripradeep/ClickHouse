#pragma once

/// Live single-node, 2-node, and 3-node/4-node ClickHouse Keeper integration test suites.
/// Executed in parallel isolated child processes (`--live-server-tests --live-suite <name>`,
/// automatically invoked by `--ci`) with virtual POSIX time disabled and disjoint loopback port blocks
/// so real `KeeperDispatcher`, `KeeperRequestDispatcher`, `KeeperRequestDispatcherOld`, `KeeperServer`,
/// `KeeperStateManager`, `SessionExpiryQueue`, `KeeperReadThreadPool`, `FourLetterCommandFactory`
/// (all 28+ 4LW commands + `rcfg`), `KeeperOverDispatcher`, `KeeperTCPHandler` (plain, LZ4, XID64,
/// OpenTelemetry tracing, TLS 1.3, password auth, `chroot`, session timeout clamping, `SessionExpired`,
/// cross-client watches, `read_during_close`), `KeeperHTTP*`, client helpers (`ZooKeeperLock`,
/// `ZooKeeperNodeCache`, `ZooKeeperCachingGetter`, `ZooKeeperRetriesControl`, `path_acls`, AZ preference),
/// multi-node LSMT-Disk, chunked `InstallSnapshot`, `forceRecovery`, and TLS Raft peers are exercised
/// end-to-end in ~10s wall-clock.

#include <SimClock.h>

#include <Common/Macros.h>
#include <Common/MultiVersion.h>
#include <Common/ZooKeeper/KeeperOverDispatcher.h>
#include <Common/ZooKeeper/ZooKeeper.h>
#include <Common/ZooKeeper/ZooKeeperArgs.h>
#include <Common/ZooKeeper/ZooKeeperCachingGetter.h>
#include <Common/ZooKeeper/ZooKeeperCommon.h>
#include <Common/ZooKeeper/ZooKeeperConstants.h>
#include <Common/ZooKeeper/ZooKeeperIO.h>
#include <Common/ZooKeeper/ZooKeeperLock.h>
#include <Common/ZooKeeper/ZooKeeperNodeCache.h>
#include <Common/ZooKeeper/ZooKeeperPathUtils.h>
#include <Common/ZooKeeper/ZooKeeperRetries.h>
#include <IO/ReadBufferFromString.h>
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
#    include <Server/CertificateReloader.h>
#    include <base/scope_guard.h>
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
#include <Poco/Util/Application.h>
#include <Poco/Util/MapConfiguration.h>
#include <Poco/Util/XMLConfiguration.h>

#if USE_AWS_S3
#    include <Coordination/KeeperSnapshotManagerS3.h>
#    include <Disks/DiskLocal.h>
#    include <IO/WriteBufferFromFile.h>
#    include <Server/HTTP/HTTPRequestHandler.h>
#    include <Server/HTTP/HTTPServerRequest.h>
#    include <Server/HTTP/HTTPServerResponse.h>
#endif

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

inline std::atomic<uint16_t> g_live_port_cursor{0};

inline uint16_t pickFreeLoopbackPort()
{
    uint16_t cursor = g_live_port_cursor.load(std::memory_order_relaxed);
    if (cursor != 0)
    {
        for (int tries = 0; tries < 256; ++tries)
        {
            const uint16_t cand = g_live_port_cursor.fetch_add(1, std::memory_order_relaxed);
            if (cand < 1024)
                continue;
            try
            {
                Poco::Net::ServerSocket probe;
                probe.bind(Poco::Net::SocketAddress("127.0.0.1", cand), /*reuseAddress=*/false);
                probe.close();
                return cand;
            }
            catch (...)
            {
            }
        }
    }
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

inline void shutdownDispatcher(std::shared_ptr<DB::KeeperDispatcher> & d)
{
    if (!d)
        return;
    d->signalShutdown();
    d->beginTCPConnectionDrain();
    d->shutdownBeforeConnectionsFinish();
    d->shutdownAfterConnectionsFinish(/*closed_all_connections=*/true);
    d.reset();
}

template <typename Pred>
inline void waitUntil(int timeout_ms, Pred && pred, const std::string & msg)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred())
            return;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    throw std::runtime_error(msg);
}

inline std::shared_ptr<zkutil::ZooKeeper> makeOverDispatcherClient(
    const std::shared_ptr<DB::KeeperDispatcher> & d, int timeout_s = 10)
{
    return zkutil::ZooKeeper::createFromImpl([d, timeout_s]()
    {
        return std::make_unique<Coordination::KeeperOverDispatcher>(d, Poco::Timespan(timeout_s, 0));
    });
}

inline void runRcfgCommand(DB::KeeperDispatcher & leader, const std::string & json_cmd, const std::string & label)
{
    Poco::JSON::Parser parser;
    auto obj = parser.parse(json_cmd).extract<Poco::JSON::Object::Ptr>();
    auto res = leader.reconfigureClusterFromReconfigureCommand(obj);
    std::ostringstream res_ss;
    Poco::JSON::Stringifier::stringify(res, res_ss);
    require(!res.isNull() && res_ss.str().find("error") == std::string::npos, label + " failed: " + res_ss.str());
}

inline void waitUntilActiveLeader(DB::KeeperDispatcher & dispatcher, int timeout_ms = 5000)
{
    waitUntil(
        timeout_ms,
        [&]() { return dispatcher.checkInit() && dispatcher.isServerActive() && dispatcher.isLeader(); },
        "Live KeeperDispatcher did not become active leader within timeout");
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

/// Raw-wire HTTP/1.1 `HEAD` probe (`Connection: close`): `Poco::Net::HTTPClientSession`
/// forces a zero-length stream for `HEAD` and silently ignores illegal response bodies on the wire,
/// so read the actual bytes after `\r\n\r\n` directly from the TCP socket.
inline LiveHTTPResult sendRawLoopbackHTTPHead(uint16_t port, const std::string & uri)
{
    Poco::Net::StreamSocket sock(Poco::Net::SocketAddress("127.0.0.1", port));
    sock.setReceiveTimeout(Poco::Timespan(5, 0));
    sock.setSendTimeout(Poco::Timespan(5, 0));
    const std::string req = "HEAD " + uri + " HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port)
        + "\r\nConnection: close\r\n\r\n";
    const int sent = sock.sendBytes(req.data(), static_cast<int>(req.size()));
    require(sent == static_cast<int>(req.size()), "Raw HTTP HEAD short send for " + uri);

    std::string raw;
    char buf[4096];
    for (;;)
    {
        const int n = sock.receiveBytes(buf, sizeof(buf));
        if (n <= 0)
            break;
        raw.append(buf, static_cast<size_t>(n));
    }
    sock.close();

    const size_t sep = raw.find("\r\n\r\n");
    require(sep != std::string::npos, "Raw HTTP HEAD missing header terminator for " + uri);
    const size_t sp1 = raw.find(' ');
    require(sp1 != std::string::npos && sp1 + 4 <= sep, "Raw HTTP HEAD malformed status line for " + uri);
    const int status = std::stoi(raw.substr(sp1 + 1, 3));
    return LiveHTTPResult{status, raw.substr(sep + 4)};
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

    /// ---- Part B: 4LW allow-list enforcement (`ruok,isro` only) -> then ALL 28+ 4LW commands + `rcfg` ----
    {
        /// First verify restricted allow-list enforcement before widening to `*`.
        dispatcher->getKeeperConfiguration()->four_letter_word_allow_list = "ruok,isro";
        DB::FourLetterCommandFactory::registerCommands(*dispatcher);
        auto & factory = DB::FourLetterCommandFactory::instance();
        require(factory.isEnabled(DB::IFourLetterCommand::toCode("ruok")), "ruok should be enabled in restricted allow-list");
        require(factory.isEnabled(DB::IFourLetterCommand::toCode("isro")), "isro should be enabled in restricted allow-list");
        require(!factory.isEnabled(DB::IFourLetterCommand::toCode("csnp")), "csnp should be disabled in restricted allow-list");

        /// Now widen allow-list to `*` to exercise every registered 4LW command.
        dispatcher->getKeeperConfiguration()->four_letter_word_allow_list = "*";
        factory.initializeAllowList(*dispatcher);

        std::vector<std::string> cmds = {
            "ruok", "mntr", "srvr", "stat", "conf", "cons",
            "wchs", "wchc", "wchp", "dump", "envi", "dirs",
            "isro", "apiv", "csnp", "lgif", "rclc", "clrs",
            "ftfl", "bpon", "bpof", "pfev", "lgrq", "lgrq",
            "srst", "crst", "rqld", "ydld",
        };
#if USE_JEMALLOC
        cmds.push_back("jmst");
#endif
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
            auto zk_over_disp = makeOverDispatcherClient(dispatcher);
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

        /// 3. Real `zkutil::ZooKeeper` TCP client — LZ4 COMPRESSION + 64-BIT XIDs + OPENTELEMETRY TRACING!
        {
            zkutil::ZooKeeperArgs args("127.0.0.1:" + std::to_string(tcp_port));
            args.connection_timeout_ms = 5000;
            args.session_timeout_ms = 10000;
            args.operation_timeout_ms = 5000;
            args.use_compression = true;
            args.use_xid_64 = true;
            args.pass_opentelemetry_tracing_context = true;
            auto zk = zkutil::ZooKeeper::create(args, nullptr, nullptr);

            const std::string big_val(2048, 'Z');
            zk->create("/tcp_lz4", big_val, zkutil::CreateMode::Persistent);
            require(zk->get("/tcp_lz4") == big_val, "TCP LZ4+XID64+Tracing get mismatch");
            zk->set("/tcp_lz4", "lz4-updated");
            require(zk->get("/tcp_lz4") == "lz4-updated", "TCP LZ4+XID64+Tracing set mismatch");
            auto children = zk->getChildren("/");
            require(!children.empty(), "TCP LZ4+XID64+Tracing list failed");
            zk->remove("/tcp_lz4");
        }

        /// 3b. Client with `last_zxid_seen` ahead of server MUST be refused (`SESSION_REFUSED`)!
        {
            zkutil::ZooKeeperArgs future_args("127.0.0.1:" + std::to_string(tcp_port));
            future_args.connection_timeout_ms = 2000;
            future_args.session_timeout_ms = 5000;
            future_args.operation_timeout_ms = 2000;
            future_args.num_connection_retries = 0;
            future_args.last_zxid_seen = 999999999LL;
            bool refused = false;
            try
            {
                auto zk = zkutil::ZooKeeper::create(future_args, nullptr, nullptr);
                (void)zk->exists("/");
            }
            catch (const std::exception &)
            {
                refused = true;
            }
            require(refused, "KeeperTCPHandler accepted client with future last_zxid_seen!");
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
            return makeOverDispatcherClient(dispatcher);
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

        std::vector<std::string> head_violations;
        auto check_head_empty = [&](const std::string & uri, const std::string & tag)
        {
            auto r_head = sendRawLoopbackHTTPHead(http_port, uri);
            if (r_head.status != 200 || !r_head.body.empty())
            {
                head_violations.push_back(
                    "HTTP HEAD " + uri + " (" + tag + ") status=" + std::to_string(r_head.status)
                    + " wire_body_bytes=" + std::to_string(r_head.body.size()));
            }
        };

        /// 1. Readiness endpoint `/ready` (GET + RFC 9110 HEAD empty-body check, #135025)
        {
            auto r = sendLoopbackHTTP(http_port, "GET", "/ready");
            require(r.status == 200 && r.body.find("\"status\":\"ok\"") != std::string::npos
                && (r.body.find("\"role\":\"standalone\"") != std::string::npos || r.body.find("\"role\":\"leader\"") != std::string::npos),
                "HTTP /ready failed: status=" + std::to_string(r.status) + " body=" + r.body);
            check_head_empty("/ready", "#135025");
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
        /// 3. Commands endpoint `/api/v1/commands` (both 4LW `ruok`, KeeperClientCLI `ls /`, and HEAD empty-body check)
        {
            auto r1 = sendLoopbackHTTP(http_port, "GET", "/api/v1/commands?command=ruok");
            require(r1.status == 200 && r1.body.find("imok") != std::string::npos,
                "HTTP /api/v1/commands ruok failed: " + r1.body);
            check_head_empty("/api/v1/commands?command=ruok", "#135025");
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

            check_head_empty("/api/v1/storage/http_key", "Bug #63");

            auto r_put = sendLoopbackHTTP(http_port, "PUT", "/api/v1/storage/http_key?version=0", "http-v2");
            require(r_put.status == 200, "HTTP PUT /api/v1/storage/http_key expected 200, got " + std::to_string(r_put.status));

            auto r_list = sendLoopbackHTTP(http_port, "GET", "/api/v1/storage?children=true");
            require(r_list.status == 200 && r_list.body.find("http_key") != std::string::npos,
                "HTTP GET /api/v1/storage?children=true failed: " + r_list.body);

            auto r_del = sendLoopbackHTTP(http_port, "DELETE", "/api/v1/storage/http_key?version=1");
            require(r_del.status == 204, "HTTP DELETE /api/v1/storage/http_key expected 204, got " + std::to_string(r_del.status));

            auto r_gone = sendLoopbackHTTP(http_port, "GET", "/api/v1/storage/http_key");
            require(r_gone.status == 404, "HTTP GET deleted key expected 404, got " + std::to_string(r_gone.status));

            if (!head_violations.empty())
            {
                std::string msg = "RFC 9110 HTTP HEAD sent non-empty wire body:";
                for (const auto & v : head_violations)
                    msg += " [" + v + "]";
                throw std::runtime_error(msg);
            }
        }

        http_server.stopAll(/*abort_current=*/true);
    }
    std::cout << "  [PASS] live: http_handlers_readiness_dashboard_commands_and_storage\n";

    shutdownDispatcher(dispatcher);
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
    shutdownDispatcher(dispatcher);
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

    shutdownDispatcher(dispatcher);
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

    /// Wait for 1 leader + 2 active followers.
    int leader_idx = -1;
    waitUntil(8000, [&]()
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
        auto zk_follower = makeOverDispatcherClient(nodes[follower_idx]);
        zk_follower->create("/forwarded_from_follower", "fwd-v1", zkutil::CreateMode::Persistent);
        require(zk_follower->get("/forwarded_from_follower") == "fwd-v1", "Follower read-after-write mismatch");

        /// Verify leader sees the write and reports 2 synced followers in 4LW & cluster members info.
        auto zk_leader = makeOverDispatcherClient(nodes[leader_idx]);
        require(zk_leader->get("/forwarded_from_follower") == "fwd-v1", "Leader did not observe follower-forwarded write");

        waitUntil(5000, [&]()
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

    /// 2. Live `rcfg` dynamic 4th-node join (`add_members`) -> catch-up -> removal (`remove_members`)!
    {
        const uint16_t raft_port4 = pickFreeLoopbackPort();
        const uint16_t tcp_port4 = pickFreeLoopbackPort();
        const auto ndir4 = base_dir / "node_4";
        std::filesystem::create_directories(ndir4 / "logs");
        std::filesystem::create_directories(ndir4 / "snapshots");

        std::ostringstream xml4;
        xml4 << "<clickhouse><keeper_server>\n"
             << "  <tcp_port>" << tcp_port4 << "</tcp_port>\n"
             << "  <server_id>4</server_id>\n"
             << "  <enable_ipv6>false</enable_ipv6>\n"
             << "  <enable_reconfiguration>true</enable_reconfiguration>\n"
             << "  <log_storage_path>" << (ndir4 / "logs").string() << "</log_storage_path>\n"
             << "  <snapshot_storage_path>" << (ndir4 / "snapshots").string() << "</snapshot_storage_path>\n"
             << "  <coordination_settings>\n"
             << "    <operation_timeout_ms>5000</operation_timeout_ms>\n"
             << "    <startup_timeout>5000</startup_timeout>\n"
             << "    <session_timeout_ms>10000</session_timeout_ms>\n"
             << "    <raft_logs_level>error</raft_logs_level>\n"
             << "    <heart_beat_interval_ms>50</heart_beat_interval_ms>\n"
             << "    <election_timeout_lower_bound_ms>150</election_timeout_lower_bound_ms>\n"
             << "    <election_timeout_upper_bound_ms>300</election_timeout_upper_bound_ms>\n"
             << "    <use_new_dispatcher>true</use_new_dispatcher>\n"
             << "  </coordination_settings>\n"
             << "  <raft_configuration>\n"
             << "    <server><id>1</id><hostname>127.0.0.1</hostname><port>" << raft_ports[0] << "</port></server>\n"
             << "    <server><id>2</id><hostname>127.0.0.1</hostname><port>" << raft_ports[1] << "</port></server>\n"
             << "    <server><id>3</id><hostname>127.0.0.1</hostname><port>" << raft_ports[2] << "</port></server>\n"
             << "    <server><id>4</id><hostname>127.0.0.1</hostname><port>" << raft_port4 << "</port><start_as_follower>true</start_as_follower></server>\n"
             << "  </raft_configuration>\n</keeper_server></clickhouse>\n";
        std::istringstream iss4(xml4.str());
        Poco::AutoPtr<Poco::Util::XMLConfiguration> cfg4(new Poco::Util::XMLConfiguration(iss4));
        auto node4 = std::make_shared<DB::KeeperDispatcher>();
        SCOPE_EXIT({
            shutdownDispatcher(node4);
        });
        node4->initialize(*cfg4, /*standalone_keeper=*/true, /*start_async=*/true, macros);

        /// Add Node 4 to the live 3-node cluster via `rcfg` `add_members`!
        {
            std::ostringstream js;
            js << R"({"preconditions":{"members":[1,2,3],"leaders":[)" << (leader_idx + 1)
               << R"(]},"actions":[{"add_members":[{"id":4,"endpoint":"127.0.0.1:)" << raft_port4
               << R"(","learner":false,"priority":1}]}]})";
            runRcfgCommand(*nodes[leader_idx], js.str(), "rcfg add_members(4)");
        }
        waitUntil(8000, [&]() { return node4->isServerActive() && node4->isFollower(); },
            "Dynamically added Node 4 did not become active follower");
        {
            auto zk4 = makeOverDispatcherClient(node4);
            require(zk4->get("/forwarded_from_follower") == "fwd-v1", "Dynamically added Node 4 missing replicated key");
        }
        require(nodes[leader_idx]->getClusterMembersInfo().size() == 4, "Expected 4 members after add_members(4)");

        /// Remove Node 4 back out via `rcfg` `remove_members`!
        {
            std::ostringstream js;
            js << R"({"preconditions":{"members":[1,2,3,4],"leaders":[)" << (leader_idx + 1)
               << R"(]},"actions":[{"remove_members":[4]}]})";
            runRcfgCommand(*nodes[leader_idx], js.str(), "rcfg remove_members(4)");
        }
        require(nodes[leader_idx]->getClusterMembersInfo().size() == 3, "Expected 3 members after remove_members(4)");
    }

    /// 2b. Live `rcfg` `set_priority` + `transfer_leadership` to `follower_idx` (`server_id = follower_idx + 1`).
    {
        const int old_leader_id = leader_idx + 1;
        const int target_leader_id = follower_idx + 1;
        std::ostringstream js;
        js << R"({"preconditions":{"members":[1,2,3],"leaders":[)" << old_leader_id
           << R"(]},"actions":[{"set_priority":[{"id":)" << target_leader_id << R"(,"priority":2}]},{"transfer_leadership":[)"
           << target_leader_id << R"(]}]})";
        runRcfgCommand(*nodes[leader_idx], js.str(), "rcfg transfer_leadership");

        waitUntil(8000, [&]() { return find_leader_idx() == follower_idx; },
            "Leadership did not transfer to target follower " + std::to_string(target_leader_id));
        leader_idx = follower_idx;
    }

    /// 3. Leader crash/shutdown -> automatic failover to surviving 2/3 majority -> write `/after_failover`.
    {
        auto stop_node = [&](int idx)
        {
            if (!nodes[idx])
                return;
            shutdownDispatcher(nodes[idx]);
        };

        const int killed_leader = leader_idx;
        stop_node(killed_leader);

        int new_leader = -1;
        waitUntil(8000, [&]()
        {
            new_leader = find_leader_idx();
            return new_leader >= 0 && new_leader != killed_leader;
        }, "Remaining 2/3 nodes failed to elect a new leader after failover");

        auto zk_surv = makeOverDispatcherClient(nodes[new_leader]);
        zk_surv->create("/after_failover", "survived-2of3", zkutil::CreateMode::Persistent);
        require(zk_surv->get("/after_failover") == "survived-2of3", "Write on surviving 2/3 majority failed");

        /// Ensure node 1 (index 0) receives `/after_failover` before shutting down all nodes for forceRecovery test.
        if (!nodes[0])
        {
            /// Node 1 was the killed leader; bring it back briefly to sync `/after_failover`.
            auto cfg1 = make_node_cfg(1, /*single_node_local_only=*/false);
            nodes[0] = std::make_shared<DB::KeeperDispatcher>();
            nodes[0]->initialize(*cfg1, /*standalone_keeper=*/true, /*start_async=*/true, macros);
            waitUntil(8000, [&]() { return nodes[0]->isServerActive(); }, "Node 1 failed to rejoin before recovery test");
        }
        waitUntil(5000, [&]()
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

        auto zk_rec = makeOverDispatcherClient(node1);
        require(zk_rec->get("/forwarded_from_follower") == "fwd-v1", "forceRecovery lost /forwarded_from_follower");
        require(zk_rec->get("/after_failover") == "survived-2of3", "forceRecovery lost /after_failover");
        zk_rec->create("/after_force_recovery", "recovered-ok", zkutil::CreateMode::Persistent);
        require(zk_rec->get("/after_force_recovery") == "recovered-ok", "Write after forceRecovery failed");

        shutdownDispatcher(node1);
    }
    std::cout << "  [PASS] live: 3node_forwarding_rcfg_failover_and_force_recovery\n";
}

#if USE_SSL
/// 7. Real 3-node TLS-encrypted Peer-to-Peer Raft Cluster (`raft_configuration.secure = true`)!
/// Sets up a scoped `Poco::Util::Application` providing `openSSL.server.*` and `openSSL.client.*`
/// configs (as read by `KeeperServer::setSSLParams`), starts 3 loopback TLS Raft nodes,
/// verifies quorum election + follower->leader replication over encrypted Raft RPCs!
inline void testLive3NodeTLSRaftCluster(const std::filesystem::path & root_dir)
{
    const auto base_dir = root_dir / "live_3node_tls_raft";
    std::filesystem::create_directories(base_dir);
    const auto cert_path = base_dir / "raft_cert.pem";
    const auto key_path = base_dir / "raft_key.pem";
    generateSelfSignedCertPem(cert_path, key_path);

    /// Scoped `Poco::Util::Application` so `Poco::Util::Application::instance().config()` serves
    /// the ephemeral TLS cert/key to `KeeperServer::setSSLParams`.
    struct ScopedSSLApp final : public Poco::Util::Application
    {
        ScopedSSLApp(const std::string & cert, const std::string & key)
        {
            Poco::AutoPtr<Poco::Util::MapConfiguration> ssl_cfg(new Poco::Util::MapConfiguration());
            ssl_cfg->setString("openSSL.server.certificateFile", cert);
            ssl_cfg->setString("openSSL.server.privateKeyFile", key);
            ssl_cfg->setString("openSSL.server.verificationMode", "none");
            ssl_cfg->setBool("openSSL.server.loadDefaultCAFile", false);
            ssl_cfg->setString("openSSL.client.certificateFile", cert);
            ssl_cfg->setString("openSSL.client.privateKeyFile", key);
            ssl_cfg->setString("openSSL.client.verificationMode", "none");
            ssl_cfg->setBool("openSSL.client.loadDefaultCAFile", false);
            config().add(ssl_cfg, -100);
        }
    };
    ScopedSSLApp ssl_app(cert_path.string(), key_path.string());
    DB::CertificateReloader::instance().tryLoad(ssl_app.config());
    DB::CertificateReloader::instance().tryLoadClient(ssl_app.config());

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

    auto macros = std::make_shared<const DB::Macros>();
    std::array<std::shared_ptr<DB::KeeperDispatcher>, 3> nodes;
    SCOPE_EXIT({
        for (auto & n : nodes)
        {
            shutdownDispatcher(n);
        }
    });
    for (int i = 0; i < 3; ++i)
    {
        const int id = i + 1;
        const auto ndir = base_dir / ("node_" + std::to_string(id));
        std::filesystem::create_directories(ndir / "logs");
        std::filesystem::create_directories(ndir / "snapshots");

        std::ostringstream xml;
        xml << "<clickhouse><keeper_server>\n"
            << "  <tcp_port>" << tcp_ports[i] << "</tcp_port>\n"
            << "  <server_id>" << id << "</server_id>\n"
            << "  <enable_ipv6>false</enable_ipv6>\n"
            << "  <log_storage_path>" << (ndir / "logs").string() << "</log_storage_path>\n"
            << "  <snapshot_storage_path>" << (ndir / "snapshots").string() << "</snapshot_storage_path>\n"
            << "  <coordination_settings>\n"
            << "    <operation_timeout_ms>5000</operation_timeout_ms>\n"
            << "    <startup_timeout>5000</startup_timeout>\n"
            << "    <session_timeout_ms>10000</session_timeout_ms>\n"
            << "    <raft_logs_level>error</raft_logs_level>\n"
            << "    <heart_beat_interval_ms>60</heart_beat_interval_ms>\n"
            << "    <election_timeout_lower_bound_ms>180</election_timeout_lower_bound_ms>\n"
            << "    <election_timeout_upper_bound_ms>360</election_timeout_upper_bound_ms>\n"
            << "    <use_new_dispatcher>true</use_new_dispatcher>\n"
            << "  </coordination_settings>\n"
            << "  <raft_configuration>\n"
            << "    <secure>true</secure>\n";
        for (int s = 1; s <= 3; ++s)
            xml << "    <server><id>" << s << "</id><hostname>127.0.0.1</hostname><port>" << raft_ports[s - 1] << "</port></server>\n";
        xml << "  </raft_configuration>\n</keeper_server></clickhouse>\n";

        std::istringstream iss(xml.str());
        Poco::AutoPtr<Poco::Util::XMLConfiguration> cfg(new Poco::Util::XMLConfiguration(iss));
        nodes[i] = std::make_shared<DB::KeeperDispatcher>();
        nodes[i]->initialize(*cfg, /*standalone_keeper=*/true, /*start_async=*/true, macros);
    }

    int leader_idx = -1;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (std::chrono::steady_clock::now() < deadline)
    {
        leader_idx = -1;
        int followers = 0;
        for (int i = 0; i < 3; ++i)
        {
            if (nodes[i]->isServerActive() && nodes[i]->isLeader())
                leader_idx = i;
            else if (nodes[i]->isServerActive() && nodes[i]->isFollower())
                ++followers;
        }
        if (leader_idx >= 0 && followers == 2)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }
    require(leader_idx >= 0, "3-node TLS Raft cluster did not elect leader with 2 followers");

    const int follower_idx = (leader_idx + 1) % 3;
    auto zk_f = makeOverDispatcherClient(nodes[follower_idx]);
    zk_f->create("/tls_raft_key", "encrypted-raft-ok", zkutil::CreateMode::Persistent);
    require(zk_f->get("/tls_raft_key") == "encrypted-raft-ok", "TLS Raft follower read mismatch");

    auto zk_l = makeOverDispatcherClient(nodes[leader_idx]);
    require(zk_l->get("/tls_raft_key") == "encrypted-raft-ok", "TLS Raft leader read mismatch");

    /// Hot-reload TLS certificates on disk (`CertificateReloader::tryLoad` + `tryLoadClient`) and verify
    /// continued TLS Raft replication (`test_keeper_raft_cert_reload`)!
    {
        generateSelfSignedCertPem(cert_path, key_path);
        DB::CertificateReloader::instance().tryLoad(ssl_app.config());
        DB::CertificateReloader::instance().tryLoadClient(ssl_app.config());
        auto zk_after_reload = makeOverDispatcherClient(nodes[follower_idx]);
        zk_after_reload->create("/tls_raft_after_cert_reload", "reloaded-cert-ok", zkutil::CreateMode::Persistent);
        require(zk_after_reload->get("/tls_raft_after_cert_reload") == "reloaded-cert-ok",
            "TLS Raft cluster write after certificate reload failed");
    }

    for (auto & n : nodes)
    {
        shutdownDispatcher(n);
    }

    std::cout << "  [PASS] live: 3node_tls_encrypted_raft_peers\n";
}
#endif

#if USE_AWS_S3
/// 8. `KeeperSnapshotManagerS3` end-to-end upload + `.LOCK` coordination against an in-process loopback mock S3 HTTP server!
inline void testLiveS3SnapshotUploader(const std::filesystem::path & root_dir)
{
    struct MockS3Store
    {
        std::mutex mu;
        std::unordered_map<std::string, std::string> objects;
    };
    auto store = std::make_shared<MockS3Store>();

    struct MockS3Handler final : public DB::HTTPRequestHandler
    {
        std::shared_ptr<MockS3Store> store;
        explicit MockS3Handler(std::shared_ptr<MockS3Store> s) : store(std::move(s)) {}
        void handleRequest(DB::HTTPServerRequest & req, DB::HTTPServerResponse & resp, const ProfileEvents::Event &) override
        {
            const std::string key = req.getURI();
            const std::string & method = req.getMethod();
            if (method == Poco::Net::HTTPRequest::HTTP_PUT)
            {
                std::ostringstream body;
                auto & in = req.getStream();
                char buf[4096];
                while (!in.eof())
                {
                    const size_t n = in.read(buf, sizeof(buf));
                    if (n == 0)
                        break;
                    body.write(buf, static_cast<std::streamsize>(n));
                }
                {
                    std::lock_guard lk(store->mu);
                    store->objects[key] = body.str();
                }
                resp.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
                resp.set("ETag", "\"sim-etag\"");
                resp.send();
            }
            else if (method == Poco::Net::HTTPRequest::HTTP_HEAD)
            {
                std::lock_guard lk(store->mu);
                if (auto it = store->objects.find(key); it != store->objects.end())
                {
                    resp.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
                    resp.setContentLength(it->second.size());
                }
                else
                {
                    resp.setStatus(Poco::Net::HTTPResponse::HTTP_NOT_FOUND);
                }
                resp.send();
            }
            else if (method == Poco::Net::HTTPRequest::HTTP_GET)
            {
                std::lock_guard lk(store->mu);
                if (auto it = store->objects.find(key); it != store->objects.end())
                {
                    resp.setStatus(Poco::Net::HTTPResponse::HTTP_OK);
                    resp.setContentLength(it->second.size());
                    *resp.send() << it->second;
                }
                else
                {
                    resp.setStatus(Poco::Net::HTTPResponse::HTTP_NOT_FOUND);
                    resp.send();
                }
            }
            else if (method == Poco::Net::HTTPRequest::HTTP_DELETE)
            {
                {
                    std::lock_guard lk(store->mu);
                    store->objects.erase(key);
                }
                resp.setStatus(Poco::Net::HTTPResponse::HTTP_NO_CONTENT);
                resp.send();
            }
            else
            {
                resp.setStatus(Poco::Net::HTTPResponse::HTTP_METHOD_NOT_ALLOWED);
                resp.send();
            }
        }
    };

    struct MockS3Factory final : public DB::HTTPRequestHandlerFactory
    {
        std::shared_ptr<MockS3Store> store;
        explicit MockS3Factory(std::shared_ptr<MockS3Store> s) : store(std::move(s)) {}
        std::unique_ptr<DB::HTTPRequestHandler> createRequestHandler(const DB::HTTPServerRequest &) override
        {
            return std::make_unique<MockS3Handler>(store);
        }
    };

    Poco::Net::ServerSocket s3_socket(Poco::Net::SocketAddress("127.0.0.1", 0));
    const uint16_t s3_port = s3_socket.address().port();
    Poco::ThreadPool s3_pool(2, 4);
    auto http_ctx = std::make_shared<SimHTTPContext>();
    Poco::Net::HTTPServerParams::Ptr params = new Poco::Net::HTTPServerParams();
    params->setMaxThreads(4);
    DB::HTTPServer s3_server(http_ctx, std::make_shared<MockS3Factory>(store), s3_pool, s3_socket, params);
    s3_server.start();

    const auto s3_dir = root_dir / "live_s3_snap";
    std::filesystem::create_directories(s3_dir);
    auto snap_disk = std::make_shared<DB::DiskLocal>("SimS3SnapDisk", s3_dir.string() + "/");
    const std::string snap_name = "snapshot_42.bin.zstd";
    {
        DB::WriteBufferFromFile wb((s3_dir / snap_name).string());
        wb.write("SIM_S3_SNAPSHOT_PAYLOAD", 23);
        wb.finalize();
    }

    std::ostringstream xml;
    xml << "<clickhouse><keeper_server><s3_snapshot>\n"
        << "  <endpoint>http://127.0.0.1:" << s3_port << "/keeper-bucket/</endpoint>\n"
        << "  <no_sign_request>true</no_sign_request>\n"
        << "</s3_snapshot></keeper_server></clickhouse>\n";
    std::istringstream iss(xml.str());
    Poco::AutoPtr<Poco::Util::XMLConfiguration> cfg(new Poco::Util::XMLConfiguration(iss));
    auto macros = std::make_shared<const DB::Macros>();

    DB::KeeperSnapshotManagerS3 uploader;
    uploader.startup(*cfg, macros);
    auto file_info = std::make_shared<DB::SnapshotFileInfo>(snap_name, snap_disk);
    uploader.uploadSnapshot(file_info, /*async_upload=*/false);
    uploader.shutdown();
    s3_server.stopAll(/*abort_current=*/true);

    std::lock_guard lk(store->mu);
    const std::string expected_key = "/keeper-bucket/" + snap_name;
    require(store->objects.contains(expected_key) && store->objects.at(expected_key) == "SIM_S3_SNAPSHOT_PAYLOAD",
        "KeeperSnapshotManagerS3 did not upload snapshot payload to mock S3");
    require(!store->objects.contains("/keeper-bucket/." + snap_name + "_LOCK"),
        "KeeperSnapshotManagerS3 leaked .LOCK object on mock S3");
    std::cout << "  [PASS] live: s3_snapshot_uploader_mock_s3\n";
}
#endif

/// 9. Live TCP session resumption, invalid/expired session rejection, read-only handshake,
/// and cross-client asynchronous watch push over real `KeeperTCPHandler`!
inline void testLiveTCPSessionResumptionInvalidSessionAndCrossClientWatch(const std::filesystem::path & root_dir)
{
    const auto base_dir = root_dir / "live_tcp_session_resume";
    std::filesystem::create_directories(base_dir / "logs");
    std::filesystem::create_directories(base_dir / "snapshots");

    Poco::Net::ServerSocket tcp_socket(Poco::Net::SocketAddress("127.0.0.1", 0));
    const uint16_t tcp_port = tcp_socket.address().port();
    const uint16_t raft_port = pickFreeLoopbackPort();
    auto config = makeLiveKeeperConfig(base_dir, tcp_port, raft_port, /*use_new_dispatcher=*/true);
    auto macros = std::make_shared<const DB::Macros>();

    auto dispatcher = std::make_shared<DB::KeeperDispatcher>();
    dispatcher->initialize(*config, /*standalone_keeper=*/true, /*start_async=*/true, macros);
    waitUntilActiveLeader(*dispatcher, 8000);

    Poco::ThreadPool server_pool(2, 16);
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

    SCOPE_EXIT({
        tcp_server.stop();
        shutdownDispatcher(dispatcher);
    });

    auto recv_exact = [](Poco::Net::StreamSocket & sock, char * dst, size_t n) -> bool
    {
        size_t got = 0;
        while (got < n)
        {
            const int r = sock.receiveBytes(dst + got, static_cast<int>(n - got));
            if (r <= 0)
                return false;
            got += static_cast<size_t>(r);
        }
        return true;
    };

    auto send_handshake = [&](Poco::Net::StreamSocket & sock, int64_t sid, const std::string & passwd, bool readonly_hs)
        -> std::tuple<int32_t, int32_t, int64_t, std::string>
    {
        DB::WriteBufferFromOwnString hs;
        Coordination::write(static_cast<int32_t>(readonly_hs ? Coordination::CLIENT_HANDSHAKE_LENGTH_WITH_READONLY : Coordination::CLIENT_HANDSHAKE_LENGTH), hs);
        Coordination::write(static_cast<int32_t>(0), hs);
        Coordination::write(static_cast<int64_t>(0), hs);
        Coordination::write(static_cast<int32_t>(10000), hs);
        Coordination::write(sid, hs);
        Coordination::write(passwd, hs);
        if (readonly_hs)
            Coordination::write(static_cast<int8_t>(1), hs);
        const std::string hs_str = hs.str();
        sock.sendBytes(hs_str.data(), static_cast<int>(hs_str.size()));

        char len_buf[4];
        if (!recv_exact(sock, len_buf, 4))
            return {-1, 0, 0, ""};
        DB::ReadBufferFromMemory len_rb(len_buf, 4);
        int32_t resp_len = 0;
        Coordination::read(resp_len, len_rb);
        if (resp_len <= 0 || resp_len > 256)
            return {-1, 0, 0, ""};
        std::string body(static_cast<size_t>(resp_len), '\0');
        if (!recv_exact(sock, body.data(), body.size()))
            return {-1, 0, 0, ""};
        DB::ReadBufferFromMemory rb(body.data(), body.size());
        int32_t proto = 0;
        int32_t timeout = 0;
        int64_t out_sid = 0;
        std::array<char, Coordination::PASSWORD_LENGTH> out_pw{};
        Coordination::read(proto, rb);
        Coordination::read(timeout, rb);
        Coordination::read(out_sid, rb);
        Coordination::read(out_pw, rb);
        return {proto, timeout, out_sid, std::string(out_pw.data(), out_pw.size())};
    };

    /// 1. Raw TCP timeout clamping (`100ms -> min_session_timeout_ms=1000ms`), ephemeral create,
    ///    explicit `OpNum::Close` cleanup, and `SessionExpired` (`timeout=0, sid=0`) on `previous_session_id != 0`!
    {
        Poco::Net::StreamSocket s1(Poco::Net::SocketAddress("127.0.0.1", tcp_port));
        s1.setReceiveTimeout(Poco::Timespan(3, 0));
        s1.setSendTimeout(Poco::Timespan(3, 0));
        /// Request 100ms timeout (below min_session_timeout_ms=1000ms) -> server MUST clamp up to 1000ms!
        DB::WriteBufferFromOwnString hs_clamp;
        Coordination::write(static_cast<int32_t>(Coordination::CLIENT_HANDSHAKE_LENGTH), hs_clamp);
        Coordination::write(static_cast<int32_t>(0), hs_clamp);
        Coordination::write(static_cast<int64_t>(0), hs_clamp);
        Coordination::write(static_cast<int32_t>(100), hs_clamp);
        Coordination::write(static_cast<int64_t>(0), hs_clamp);
        Coordination::write(std::string(16, '\0'), hs_clamp);
        const std::string hs_clamp_str = hs_clamp.str();
        s1.sendBytes(hs_clamp_str.data(), static_cast<int>(hs_clamp_str.size()));

        char len_buf[4];
        require(recv_exact(s1, len_buf, 4), "TCP clamp handshake length missing");
        DB::ReadBufferFromMemory len_rb(len_buf, 4);
        int32_t resp_len = 0;
        Coordination::read(resp_len, len_rb);
        std::string hbody(static_cast<size_t>(resp_len), '\0');
        require(recv_exact(s1, hbody.data(), hbody.size()), "TCP clamp handshake body missing");
        DB::ReadBufferFromMemory hrb(hbody.data(), hbody.size());
        int32_t proto1 = 0;
        int32_t to1 = 0;
        int64_t sid1 = 0;
        std::array<char, Coordination::PASSWORD_LENGTH> pw1_arr{};
        Coordination::read(proto1, hrb);
        Coordination::read(to1, hrb);
        Coordination::read(sid1, hrb);
        Coordination::read(pw1_arr, hrb);
        const std::string pw1(pw1_arr.data(), pw1_arr.size());
        require(proto1 == 0 && sid1 > 0 && to1 == 1000,
            "TCP handshake timeout clamping mismatch (got to=" + std::to_string(to1) + ")");

        /// Send Create ephemeral `/tcp_close_eph` on Socket 1.
        auto creq = std::make_shared<Coordination::ZooKeeperCreateRequest>();
        creq->xid = 1;
        creq->path = "/tcp_close_eph";
        creq->data = "alive";
        creq->is_ephemeral = true;
        creq->acls = {{Coordination::ACL::All, "world", "anyone"}};
        DB::WriteBufferFromOwnString wb;
        creq->write(wb, /*use_xid_64=*/false);
        const std::string payload = wb.str();
        s1.sendBytes(payload.data(), static_cast<int>(payload.size()));

        char rlen_buf[4];
        require(recv_exact(s1, rlen_buf, 4), "Create response length missing");
        DB::ReadBufferFromMemory rlen_rb(rlen_buf, 4);
        int32_t rlen = 0;
        Coordination::read(rlen, rlen_rb);
        std::string rbody(static_cast<size_t>(rlen), '\0');
        require(recv_exact(s1, rbody.data(), rbody.size()), "Create response body missing");

        auto zk_obs = makeOverDispatcherClient(dispatcher);
        require(zk_obs->get("/tcp_close_eph") == "alive", "Ephemeral node not visible over dispatcher");

        /// Send explicit `OpNum::Close` on Socket 1 -> ephemeral MUST be deleted promptly!
        auto close_req = std::make_shared<Coordination::ZooKeeperCloseRequest>();
        close_req->xid = Coordination::CLOSE_XID;
        DB::WriteBufferFromOwnString cwb;
        close_req->write(cwb, /*use_xid_64=*/false);
        const std::string cstr = cwb.str();
        s1.sendBytes(cstr.data(), static_cast<int>(cstr.size()));
        s1.close();

        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(4000);
        while (zk_obs->exists("/tcp_close_eph") && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        require(!zk_obs->exists("/tcp_close_eph"), "Ephemeral node not deleted after explicit Close over TCP");

        /// Attempting to continue an existing/previous session (`previous_session_id != 0`) MUST reply
        /// `HandshakeResult::SessionExpired` (`timeout == 0 && session_id == 0`) rather than silently issuing a new session!
        Poco::Net::StreamSocket s2(Poco::Net::SocketAddress("127.0.0.1", tcp_port));
        s2.setReceiveTimeout(Poco::Timespan(3, 0));
        s2.setSendTimeout(Poco::Timespan(3, 0));
        auto [proto2, to2, sid2, pw2] = send_handshake(s2, sid1, pw1, false);
        require(sid2 == 0 && to2 == 0,
            "KeeperTCPHandler did not reply SessionExpired (timeout=0, sid=0) for previous_session_id != 0");
        s2.close();
    }

    /// 2. Invalid / non-existent session_id MUST be rejected (`session_id == 0` or closed socket).
    {
        Poco::Net::StreamSocket sbad(Poco::Net::SocketAddress("127.0.0.1", tcp_port));
        sbad.setReceiveTimeout(Poco::Timespan(2, 0));
        sbad.setSendTimeout(Poco::Timespan(2, 0));
        auto [proto_bad, to_bad, sid_bad, pw_bad] = send_handshake(sbad, 0x777777LL, std::string(16, '\0'), false);
        require(sid_bad == 0, "KeeperTCPHandler accepted bogus non-existent session_id!");
        sbad.close();
    }

    /// 3. Read-only 45-byte handshake (`CLIENT_HANDSHAKE_LENGTH_WITH_READONLY`).
    {
        Poco::Net::StreamSocket sro(Poco::Net::SocketAddress("127.0.0.1", tcp_port));
        sro.setReceiveTimeout(Poco::Timespan(2, 0));
        sro.setSendTimeout(Poco::Timespan(2, 0));
        auto [proto_ro, to_ro, sid_ro, pw_ro] = send_handshake(sro, 0, std::string(16, '\0'), /*readonly_hs=*/true);
        require(sid_ro > 0 && to_ro > 0, "TCP 45-byte readonly handshake failed");
        sro.close();
    }

    /// 4. Cross-client async watch push over live TCP (`Session A` watches, `Session B` mutates!).
    {
        zkutil::ZooKeeperArgs args("127.0.0.1:" + std::to_string(tcp_port));
        args.connection_timeout_ms = 5000;
        args.session_timeout_ms = 10000;
        args.operation_timeout_ms = 5000;
        auto zk_a = zkutil::ZooKeeper::create(args, nullptr, nullptr);
        auto zk_b = zkutil::ZooKeeper::create(args, nullptr, nullptr);

        zk_a->create("/cross_watch", "v0", zkutil::CreateMode::Persistent);
        zk_a->create("/cross_dir", "", zkutil::CreateMode::Persistent);

        auto data_ev = std::make_shared<Poco::Event>();
        auto child_ev = std::make_shared<Poco::Event>();
        require(zk_a->get("/cross_watch", nullptr, data_ev) == "v0", "Cross-client initial get failed");
        require(zk_a->getChildren("/cross_dir", nullptr, child_ev).empty(), "Cross-client initial list failed");

        zk_b->set("/cross_watch", "v1");
        zk_b->create("/cross_dir/c1", "child1", zkutil::CreateMode::Persistent);

        require(data_ev->tryWait(3000), "Cross-client TCP data watch push did not arrive on Session A");
        require(child_ev->tryWait(3000), "Cross-client TCP child watch push did not arrive on Session A");
    }

    std::cout << "  [PASS] live: tcp_session_resumption_invalid_session_readonly_and_cross_client_watch\n";
}

/// 10. Live `ZooKeeper` Client Library Helpers (`ZooKeeperLock`, `ZooKeeperNodeCache`, `ZooKeeperCachingGetter`,
/// `multi` atomic rollback, and `listWithOptions`) against a real `KeeperDispatcher`!
inline void testLiveClientHelpersLockNodeCacheCachingGetterAndMulti(const std::filesystem::path & root_dir)
{
    const auto base_dir = root_dir / "live_client_helpers";
    std::filesystem::create_directories(base_dir / "logs");
    std::filesystem::create_directories(base_dir / "snapshots");

    Poco::Net::ServerSocket tcp_socket(Poco::Net::SocketAddress("127.0.0.1", 0));
    const uint16_t tcp_port = tcp_socket.address().port();
    const uint16_t raft_port = pickFreeLoopbackPort();
    auto config = makeLiveKeeperConfig(base_dir, tcp_port, raft_port, /*use_new_dispatcher=*/true);
    auto macros = std::make_shared<const DB::Macros>();

    auto dispatcher = std::make_shared<DB::KeeperDispatcher>();
    dispatcher->initialize(*config, /*standalone_keeper=*/true, /*start_async=*/true, macros);
    waitUntilActiveLeader(*dispatcher, 8000);

    Poco::ThreadPool server_pool(2, 16);
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

    SCOPE_EXIT({
        tcp_server.stop();
        shutdownDispatcher(dispatcher);
    });

    auto make_zk = [tcp_port]()
    {
        zkutil::ZooKeeperArgs args("127.0.0.1:" + std::to_string(tcp_port));
        args.connection_timeout_ms = 5000;
        args.session_timeout_ms = 10000;
        args.operation_timeout_ms = 5000;
        return zkutil::ZooKeeper::create(args, nullptr, nullptr);
    };

    auto zk1 = make_zk();
    auto zk2 = make_zk();

    /// 1. `zkutil::ZooKeeperLock` mutual exclusion.
    {
        zk1->create("/locks", "", zkutil::CreateMode::Persistent);
        auto lock1 = zkutil::createSimpleZooKeeperLock(zk1, "/locks", "res1", "holder1");
        auto lock2 = zkutil::createSimpleZooKeeperLock(zk2, "/locks", "res1", "holder2");
        require(lock1->tryLock(), "lock1 failed to acquire free ZooKeeperLock");
        require(lock1->isLocked(/*check_node=*/true), "lock1 should report locked");
        require(!lock2->tryLock(), "lock2 acquired contended ZooKeeperLock!");
        lock1->unlock();
        require(lock2->tryLock(), "lock2 failed to acquire unlocked ZooKeeperLock");
        lock2->unlock();
    }

    /// 2. `zkutil::ZooKeeperNodeCache` live watch-driven invalidation (missing -> created -> updated -> deleted).
    {
        zkutil::ZooKeeperNodeCache cache([&]() { return zk1; });
        auto ev = std::make_shared<Poco::Event>();
        auto z0 = cache.get("/nc_node", ev);
        require(!z0.exists, "NodeCache expected nonexistent initially");

        zk2->create("/nc_node", "nc-v1", zkutil::CreateMode::Persistent);
        require(ev->tryWait(3000), "NodeCache watch did not fire on create");
        ev->reset();
        auto z1 = cache.get("/nc_node", ev);
        require(z1.exists && z1.contents == "nc-v1", "NodeCache did not observe created payload");

        zk2->set("/nc_node", "nc-v2");
        require(ev->tryWait(3000), "NodeCache watch did not fire on set");
        ev->reset();
        auto z2 = cache.get("/nc_node", ev);
        require(z2.exists && z2.contents == "nc-v2", "NodeCache did not observe updated payload");

        zk2->remove("/nc_node");
        require(ev->tryWait(3000), "NodeCache watch did not fire on remove");
        auto z3 = cache.get("/nc_node", ev);
        require(!z3.exists, "NodeCache did not observe deletion");

        /// Two `ZooKeeperNodeCache` instances sharing one `ZooKeeper` session must BOTH observe updates (#135023).
        zk2->create("/nc_shared", "s-v1", zkutil::CreateMode::Persistent);
        zkutil::ZooKeeperNodeCache shared_c1([&]() { return zk1; });
        zkutil::ZooKeeperNodeCache shared_c2([&]() { return zk1; });
        auto ev_s1 = std::make_shared<Poco::Event>();
        auto ev_s2 = std::make_shared<Poco::Event>();
        require(shared_c1.get("/nc_shared", ev_s1).contents == "s-v1", "shared_c1 initial read failed");
        require(shared_c2.get("/nc_shared", ev_s2).contents == "s-v1", "shared_c2 initial read failed");
        zk2->set("/nc_shared", "s-v2");
        require(ev_s1->tryWait(3000), "shared_c1 watch did not fire");
        require(ev_s2->tryWait(3000) && shared_c2.get("/nc_shared", ev_s2).contents == "s-v2",
            "Second ZooKeeperNodeCache on same session missed watch invalidation (#135023)");
    }

    /// 3. `zkutil::ZooKeeperCachingGetter` cache hit & `resetCache`.
    {
        int factory_calls = 0;
        zkutil::ZooKeeperCachingGetter getter([&]()
        {
            ++factory_calls;
            return make_zk();
        });
        auto [c1, s1] = getter.getZooKeeper();
        require(s1 == zkutil::ZooKeeperCachingGetter::SessionStatus::New && factory_calls == 1,
            "CachingGetter first call expected New");
        auto [c2, s2] = getter.getZooKeeper();
        require(s2 == zkutil::ZooKeeperCachingGetter::SessionStatus::Cached && c2 == c1 && factory_calls == 1,
            "CachingGetter second call expected Cached");
        getter.resetCache();
        auto [c3, s3] = getter.getZooKeeper();
        require(s3 == zkutil::ZooKeeperCachingGetter::SessionStatus::New && factory_calls == 2,
            "CachingGetter after resetCache expected New");
    }

    /// 4. Live `ZooKeeper::multi` atomic rollback + `listWithOptions` over `KeeperOverDispatcher`.
    {
        Coordination::Requests bad_multi;
        bad_multi.push_back(zkutil::makeCreateRequest("/live_rb_ok", "temp", zkutil::CreateMode::Persistent));
        bad_multi.push_back(zkutil::makeCreateRequest("/live_rb_dup", "d1", zkutil::CreateMode::Persistent));
        bad_multi.push_back(zkutil::makeCreateRequest("/live_rb_dup", "d2", zkutil::CreateMode::Persistent));
        Coordination::Responses res;
        require(zk1->tryMulti(bad_multi, res) == Coordination::Error::ZNODEEXISTS,
            "Live multi expected ZNODEEXISTS rollback");
        require(!zk1->exists("/live_rb_ok") && !zk1->exists("/live_rb_dup"),
            "Live multi rollback leaked created nodes");

        zk1->create("/live_lwo", "", zkutil::CreateMode::Persistent);
        zk1->create("/live_lwo/p1", "pv1", zkutil::CreateMode::Persistent);
        zk1->create("/live_lwo/p1/sub", "sv1", zkutil::CreateMode::Persistent);
        zk1->create("/live_lwo/e1", "ev1", zkutil::CreateMode::Ephemeral);

        auto ov = std::make_unique<Coordination::KeeperOverDispatcher>(dispatcher, Poco::Timespan(10, 0));
        Coordination::ListOptions opts;
        opts.filter = Coordination::ListRequestType::EPHEMERAL_ONLY;
        opts.with_stat = true;
        opts.with_data = true;
        auto done = std::make_shared<Poco::Event>();
        Coordination::ListWithOptionsResponse got;
        ov->listWithOptions("/live_lwo", opts, [&](const Coordination::ListWithOptionsResponse & r)
        {
            got = r;
            done->set();
        }, {});
        require(done->tryWait(5000) && got.error == Coordination::Error::ZOK
            && got.names.size() == 1 && got.names[0] == "e1" && got.data[0] == "ev1",
            "Live KeeperOverDispatcher listWithOptions EPHEMERAL_ONLY failed");
    }

    std::cout << "  [PASS] live: client_helpers_lock_node_cache_caching_getter_and_multi\n";
}

/// 11. Real 3-node On-Disk LSMT Cluster (`use_lsmt_storage = true`, `storage_memory_only = false`)
/// + live 4th-node `learner: true` dynamic join/catch-up/remove + `HOT_RELOAD` `updateConfiguration`!
inline void testLive3NodeLSMTDiskLearnerJoinAndHotReload(const std::filesystem::path & root_dir)
{
    const auto base_dir = root_dir / "live_3node_lsmt_disk_learner";
    std::filesystem::create_directories(base_dir);

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

    auto make_lsmt_cfg = [&](int id, bool quorum_reads_val, uint64_t batch_size_val, uint64_t read_threads_val,
                             uint16_t extra_learner_port = 0)
    {
        const auto ndir = base_dir / ("node_" + std::to_string(id));
        std::filesystem::create_directories(ndir / "logs");
        std::filesystem::create_directories(ndir / "snapshots");
        std::filesystem::create_directories(ndir / "data");
        std::ostringstream xml;
        xml << "<clickhouse><keeper_server>\n"
            << "  <tcp_port>" << (id <= 3 ? tcp_ports[id - 1] : pickFreeLoopbackPort()) << "</tcp_port>\n"
            << "  <server_id>" << id << "</server_id>\n"
            << "  <enable_reconfiguration>true</enable_reconfiguration>\n"
            << "  <log_storage_path>" << (ndir / "logs").string() << "</log_storage_path>\n"
            << "  <snapshot_storage_path>" << (ndir / "snapshots").string() << "</snapshot_storage_path>\n"
            << "  <data_storage_path>" << (ndir / "data").string() << "</data_storage_path>\n"
            << "  <coordination_settings>\n"
            << "    <operation_timeout_ms>5000</operation_timeout_ms>\n"
            << "    <startup_timeout>8000</startup_timeout>\n"
            << "    <session_timeout_ms>10000</session_timeout_ms>\n"
            << "    <raft_logs_level>error</raft_logs_level>\n"
            << "    <heart_beat_interval_ms>50</heart_beat_interval_ms>\n"
            << "    <election_timeout_lower_bound_ms>150</election_timeout_lower_bound_ms>\n"
            << "    <election_timeout_upper_bound_ms>300</election_timeout_upper_bound_ms>\n"
            << "    <use_new_dispatcher>true</use_new_dispatcher>\n"
            << "    <optimize_read_order>true</optimize_read_order>\n"
            << "    <use_lsmt_storage>true</use_lsmt_storage>\n"
            << "    <storage_memory_only>false</storage_memory_only>\n"
            << "    <committed_memtable_size>4096</committed_memtable_size>\n"
            << "    <block_cache_size>8192</block_cache_size>\n"
            << "    <sorted_file_uncompressed_size>4096</sorted_file_uncompressed_size>\n"
            << "    <quorum_reads>" << (quorum_reads_val ? "true" : "false") << "</quorum_reads>\n"
            << "    <max_requests_batch_size>" << batch_size_val << "</max_requests_batch_size>\n"
            << "    <parallel_read_threads>" << read_threads_val << "</parallel_read_threads>\n"
            << "  </coordination_settings>\n"
            << "  <raft_configuration>\n";
        for (int sid = 1; sid <= 3; ++sid)
            xml << "    <server><id>" << sid << "</id><hostname>127.0.0.1</hostname><port>" << raft_ports[sid - 1] << "</port></server>\n";
        if (extra_learner_port > 0)
            xml << "    <server><id>4</id><hostname>127.0.0.1</hostname><port>" << extra_learner_port << "</port><can_become_leader>false</can_become_leader><priority>0</priority><start_as_follower>true</start_as_follower></server>\n";
        xml << "  </raft_configuration>\n</keeper_server></clickhouse>\n";
        std::istringstream iss(xml.str());
        return Poco::AutoPtr<Poco::Util::XMLConfiguration>(new Poco::Util::XMLConfiguration(iss));
    };

    auto macros = std::make_shared<const DB::Macros>();
    std::array<std::shared_ptr<DB::KeeperDispatcher>, 3> nodes;
    SCOPE_EXIT({
        for (auto & n : nodes)
        {
            shutdownDispatcher(n);
        }
    });

    for (int i = 0; i < 3; ++i)
    {
        auto cfg = make_lsmt_cfg(i + 1, false, 64, 2);
        nodes[i] = std::make_shared<DB::KeeperDispatcher>();
        nodes[i]->initialize(*cfg, /*standalone_keeper=*/true, /*start_async=*/true, macros);
    }


    int leader_idx = -1;
    waitUntil(10000, [&]()
    {
        for (int i = 0; i < 3; ++i)
        {
            if (nodes[i]->isServerActive() && nodes[i]->isLeader())
            {
                leader_idx = i;
                return true;
            }
        }
        return false;
    }, "3-node LSMT-Disk cluster did not elect leader");

    /// Write 35 nodes + a 6 KB huge value forcing LSMT memtable flushes & sorted-run merges on disk!
    const std::string huge_val(6144, 'L');
    {
        auto zk = makeOverDispatcherClient(nodes[leader_idx]);
        zk->create("/lsmt_huge", huge_val, zkutil::CreateMode::Persistent);
        for (int i = 0; i < 35; ++i)
            zk->create("/lsmt_k" + std::to_string(i), std::string(180, static_cast<char>('a' + (i % 26))), zkutil::CreateMode::Persistent);
        require(zk->get("/lsmt_huge") == huge_val, "3-node LSMT-Disk huge readback mismatch");
    }

    /// Dynamically add a 4th LSMT-disk node as a non-voting `learner: true` via `rcfg` `add_members`!
    {
        const uint16_t raft_port4 = pickFreeLoopbackPort();
        auto cfg4 = make_lsmt_cfg(4, false, 64, 2, raft_port4);
        auto node4 = std::make_shared<DB::KeeperDispatcher>();
        SCOPE_EXIT({
            shutdownDispatcher(node4);
        });
        node4->initialize(*cfg4, /*standalone_keeper=*/true, /*start_async=*/true, macros);

        {
            std::ostringstream js;
            js << R"({"preconditions":{"members":[1,2,3],"leaders":[)" << (leader_idx + 1)
               << R"(]},"actions":[{"add_members":[{"id":4,"endpoint":"127.0.0.1:)" << raft_port4
               << R"(","learner":true,"priority":0}]}]})";
            runRcfgCommand(*nodes[leader_idx], js.str(), "rcfg add_members learner(4)");
        }

        waitUntil(8000, [&]() { return node4->isServerActive() && node4->isObserver(); },
            "Dynamically added LSMT learner Node 4 did not become active observer");
        {
            const auto members = nodes[leader_idx]->getClusterMembersInfo();
            bool found_observer = false;
            for (const auto & m : members)
                if (m.server_id == 4 && m.is_observer)
                    found_observer = true;
            require(found_observer, "Cluster members info did not report Node 4 as observer/learner");
        }
        {
            auto zk4 = makeOverDispatcherClient(node4);
            require(zk4->get("/lsmt_huge") == huge_val, "LSMT learner Node 4 missing huge value");
        }

        /// Remove learner Node 4 cleanly via `rcfg` `remove_members`.
        {
            std::ostringstream js;
            js << R"({"preconditions":{"members":[1,2,3,4],"leaders":[)" << (leader_idx + 1)
               << R"(]},"actions":[{"remove_members":[4]}]})";
            runRcfgCommand(*nodes[leader_idx], js.str(), "rcfg remove_members learner(4)");
        }
        require(nodes[leader_idx]->getClusterMembersInfo().size() == 3, "Expected 3 members after removing learner 4");
    }

    /// Hot-reload `HOT_RELOAD` `CoordinationSettings` (`quorum_reads = true`, `max_requests_batch_size = 32`, `parallel_read_threads = 4`)!
    {
        auto hot_cfg = make_lsmt_cfg(leader_idx + 1, /*quorum_reads_val=*/true, /*batch_size_val=*/32, /*read_threads_val=*/4);
        nodes[leader_idx]->updateConfiguration(*hot_cfg, macros);

        auto zk = makeOverDispatcherClient(nodes[leader_idx]);
        zk->create("/after_hot_reload", "quorum-ok", zkutil::CreateMode::Persistent);
        require(zk->get("/after_hot_reload") == "quorum-ok", "Quorum read after hot reload failed");
        require(zk->get("/lsmt_huge") == huge_val, "Huge quorum read after hot reload failed");
    }

    std::cout << "  [PASS] live: 3node_lsmt_disk_learner_join_and_hot_reload\n";
}

/// 12. Live TCP `chroot` (`root_path`) isolation + graceful `KeeperDispatcher` shutdown & restart durability
/// (snapshot + post-snapshot changelog delta + sequential counter continuity)!
inline void testLiveTCPChrootIsolationAndRestartPersistence(const std::filesystem::path & root_dir)
{
    const auto base_dir = root_dir / "live_tcp_chroot_restart";
    std::filesystem::create_directories(base_dir / "logs");
    std::filesystem::create_directories(base_dir / "snapshots");
    const uint16_t raft_port = pickFreeLoopbackPort();
    auto macros = std::make_shared<const DB::Macros>();

    /// Boot #1: exercise `chroot` isolation over live TCP, take a snapshot, then write post-snapshot deltas.
    {
        Poco::Net::ServerSocket tcp_socket(Poco::Net::SocketAddress("127.0.0.1", 0));
        const uint16_t tcp_port = tcp_socket.address().port();
        auto config = makeLiveKeeperConfig(base_dir, tcp_port, raft_port, /*use_new_dispatcher=*/true);

        auto dispatcher = std::make_shared<DB::KeeperDispatcher>();
        dispatcher->initialize(*config, /*standalone_keeper=*/true, /*start_async=*/true, macros);
        waitUntilActiveLeader(*dispatcher, 8000);

        Poco::ThreadPool server_pool(2, 16);
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
        SCOPE_EXIT({
            tcp_server.stop();
            shutdownDispatcher(dispatcher);
        });

        zkutil::ZooKeeperArgs root_args("127.0.0.1:" + std::to_string(tcp_port));
        root_args.connection_timeout_ms = 5000;
        root_args.session_timeout_ms = 10000;
        root_args.operation_timeout_ms = 5000;
        auto zk_root = zkutil::ZooKeeper::create(root_args, nullptr, nullptr);
        zk_root->create("/tenant_a", "troot", zkutil::CreateMode::Persistent);

        /// Chrooted client sees `/tenant_a` as `/`!
        zkutil::ZooKeeperArgs chroot_args = root_args;
        chroot_args.chroot = "/tenant_a";
        auto zk_chroot = zkutil::ZooKeeper::create(chroot_args, nullptr, nullptr);
        zk_chroot->create("/k1", "v1", zkutil::CreateMode::Persistent);
        require(zk_root->get("/tenant_a/k1") == "v1", "Chroot create did not map under /tenant_a");

        auto watch_ev = std::make_shared<Poco::Event>();
        require(zk_chroot->get("/k1", nullptr, watch_ev) == "v1", "Chroot get mismatch");
        zk_root->set("/tenant_a/k1", "v2");
        require(watch_ev->tryWait(3000), "Chroot watch did not fire when root updated /tenant_a/k1");
        auto ch_children = zk_chroot->getChildren("/");
        require(std::find(ch_children.begin(), ch_children.end(), "k1") != ch_children.end()
            && std::find(ch_children.begin(), ch_children.end(), "clickhouse") != ch_children.end(),
            "Chroot getChildren(/) missing expected chrooted children (k1 + clickhouse session dir)");

        /// Create sequential node #0, trigger live snapshot, then write post-snapshot delta!
        const std::string seq0 = zk_root->create("/tenant_a/seq", "s0", zkutil::CreateMode::PersistentSequential);
        require(seq0.ends_with("0000000001") || seq0.ends_with("0000000000") || seq0.find("/tenant_a/seq") == 0,
            "Unexpected sequential node name: " + seq0);
        (void)dispatcher->createSnapshot();
        zk_root->create("/post_snap_delta", "delta-alive", zkutil::CreateMode::Persistent);
    }

    /// Boot #2: fresh `KeeperDispatcher` on the exact same `logs` + `snapshots` directories MUST recover snapshot + delta!
    {
        const uint16_t tcp_port2 = pickFreeLoopbackPort();
        auto config2 = makeLiveKeeperConfig(base_dir, tcp_port2, raft_port, /*use_new_dispatcher=*/true);
        auto dispatcher2 = std::make_shared<DB::KeeperDispatcher>();
        dispatcher2->initialize(*config2, /*standalone_keeper=*/true, /*start_async=*/true, macros);
        waitUntilActiveLeader(*dispatcher2, 8000);
        SCOPE_EXIT({
            shutdownDispatcher(dispatcher2);
        });

        auto zk2 = makeOverDispatcherClient(dispatcher2);
        require(zk2->get("/tenant_a/k1") == "v2", "Restart lost pre-snapshot /tenant_a/k1");
        require(zk2->get("/post_snap_delta") == "delta-alive", "Restart lost post-snapshot changelog delta");
        const std::string seq_next = zk2->create("/tenant_a/seq", "s1", zkutil::CreateMode::PersistentSequential);
        require(seq_next > "/tenant_a/seq0000000000", "Restart did not advance sequential counter: " + seq_next);
    }

    std::cout << "  [PASS] live: tcp_chroot_isolation_and_restart_persistence\n";
}

/// 13. Real 3-node cluster with forced chunked `InstallSnapshot` over live NuRaft ASIO TCP (`reserved_log_items = 0`,
/// `snapshot_transfer_chunk_size = 512`) + live multi-host `zkutil::ZooKeeper` failover with `ZooKeeperRetriesControl`!
inline void testLive3NodeChunkedSnapshotInstallAndMultiHostClientFailover(const std::filesystem::path & root_dir)
{
    const auto base_dir = root_dir / "live_3node_chunked_snap_failover";
    std::filesystem::create_directories(base_dir);

    const std::array<uint16_t, 3> raft_ports = {
        pickFreeLoopbackPort(),
        pickFreeLoopbackPort(),
        pickFreeLoopbackPort(),
    };
    std::array<Poco::Net::ServerSocket, 3> tcp_sockets;
    std::array<uint16_t, 3> tcp_ports{};
    for (int i = 0; i < 3; ++i)
    {
        tcp_sockets[i] = Poco::Net::ServerSocket(Poco::Net::SocketAddress("127.0.0.1", 0));
        tcp_ports[i] = tcp_sockets[i].address().port();
    }

    auto make_cfg = [&](int id, uint16_t extra_port4 = 0)
    {
        const auto ndir = base_dir / ("node_" + std::to_string(id));
        std::filesystem::create_directories(ndir / "logs");
        std::filesystem::create_directories(ndir / "snapshots");
        std::ostringstream xml;
        xml << "<clickhouse><keeper_server>\n"
            << "  <tcp_port>" << (id <= 3 ? tcp_ports[id - 1] : pickFreeLoopbackPort()) << "</tcp_port>\n"
            << "  <server_id>" << id << "</server_id>\n"
            << "  <enable_reconfiguration>true</enable_reconfiguration>\n"
            << "  <log_storage_path>" << (ndir / "logs").string() << "</log_storage_path>\n"
            << "  <snapshot_storage_path>" << (ndir / "snapshots").string() << "</snapshot_storage_path>\n"
            << "  <coordination_settings>\n"
            << "    <operation_timeout_ms>5000</operation_timeout_ms>\n"
            << "    <startup_timeout>8000</startup_timeout>\n"
            << "    <session_timeout_ms>10000</session_timeout_ms>\n"
            << "    <raft_logs_level>error</raft_logs_level>\n"
            << "    <heart_beat_interval_ms>50</heart_beat_interval_ms>\n"
            << "    <election_timeout_lower_bound_ms>150</election_timeout_lower_bound_ms>\n"
            << "    <election_timeout_upper_bound_ms>300</election_timeout_upper_bound_ms>\n"
            << "    <snapshot_distance>10</snapshot_distance>\n"
            << "    <reserved_log_items>0</reserved_log_items>\n"
            << "    <stale_log_gap>5</stale_log_gap>\n"
            << "    <fresh_log_gap>3</fresh_log_gap>\n"
            << "    <snapshot_transfer_chunk_size>512</snapshot_transfer_chunk_size>\n"
            << "    <use_new_dispatcher>true</use_new_dispatcher>\n"
            << "  </coordination_settings>\n"
            << "  <raft_configuration>\n";
        for (int sid = 1; sid <= 3; ++sid)
            xml << "    <server><id>" << sid << "</id><hostname>127.0.0.1</hostname><port>" << raft_ports[sid - 1] << "</port></server>\n";
        if (extra_port4 > 0)
            xml << "    <server><id>4</id><hostname>127.0.0.1</hostname><port>" << extra_port4 << "</port><start_as_follower>true</start_as_follower></server>\n";
        xml << "  </raft_configuration>\n</keeper_server></clickhouse>\n";
        std::istringstream iss(xml.str());
        return Poco::AutoPtr<Poco::Util::XMLConfiguration>(new Poco::Util::XMLConfiguration(iss));
    };

    auto macros = std::make_shared<const DB::Macros>();
    std::array<std::shared_ptr<DB::KeeperDispatcher>, 3> nodes;
    std::array<std::unique_ptr<DB::TCPServer>, 3> tcp_servers;
    Poco::ThreadPool server_pool(4, 24);

    auto stop_node = [&](int i)
    {
        if (tcp_servers[i])
        {
            tcp_servers[i]->stop();
            tcp_servers[i].reset();
        }
        shutdownDispatcher(nodes[i]);
    };
    SCOPE_EXIT({
        for (int i = 0; i < 3; ++i)
            stop_node(i);
    });

    for (int i = 0; i < 3; ++i)
    {
        auto cfg = make_cfg(i + 1);
        nodes[i] = std::make_shared<DB::KeeperDispatcher>();
        nodes[i]->initialize(*cfg, /*standalone_keeper=*/true, /*start_async=*/true, macros);
        auto factory = Poco::SharedPtr<DB::TCPServerConnectionFactory>(new DB::KeeperTCPHandlerFactory(
            [cfg]() -> const Poco::Util::AbstractConfiguration & { return *cfg; },
            nodes[i],
            /*receive_timeout_seconds=*/5,
            /*send_timeout_seconds=*/5,
            /*secure=*/false));
        Poco::Net::TCPServerParams::Ptr params = new Poco::Net::TCPServerParams();
        params->setMaxThreads(8);
        tcp_servers[i] = std::make_unique<DB::TCPServer>(factory, server_pool, tcp_sockets[i], params);
        tcp_servers[i]->start();
    }


    auto find_leader = [&]() -> int
    {
        for (int i = 0; i < 3; ++i)
            if (nodes[i] && nodes[i]->isServerActive() && nodes[i]->isLeader())
                return i;
        return -1;
    };

    int leader_idx = -1;
    waitUntil(10000, [&]()
    {
        leader_idx = find_leader();
        return leader_idx >= 0;
    }, "3-node chunked-snapshot cluster did not elect leader");

    /// Write 25 keys + 4 KB payload -> forces automatic snapshot + log compaction (`reserved_log_items = 0`).
    const std::string big_payload(4096, 'S');
    const std::string multi_hosts = "127.0.0.1:" + std::to_string(tcp_ports[0])
        + ",127.0.0.1:" + std::to_string(tcp_ports[1])
        + ",127.0.0.1:" + std::to_string(tcp_ports[2]);
    zkutil::ZooKeeperArgs client_args(multi_hosts);
    client_args.connection_timeout_ms = 3000;
    client_args.session_timeout_ms = 10000;
    client_args.operation_timeout_ms = 5000;
    auto zk = zkutil::ZooKeeper::create(client_args, nullptr, nullptr);
    zk->create("/snap_big", big_payload, zkutil::CreateMode::Persistent);
    for (int i = 0; i < 25; ++i)
        zk->create("/snap_k" + std::to_string(i), "val-" + std::to_string(i), zkutil::CreateMode::Persistent);
    /// Ensure leader has compacted logs past index 1 so Node 4 MUST install a chunked snapshot!
    (void)nodes[leader_idx]->createSnapshot();
    waitUntil(5000, [&]()
    {
        return nodes[leader_idx]->getKeeperLogInfo().first_log_idx > 1;
    }, "Leader did not compact logs before Node 4 chunked InstallSnapshot join");

    /// Join fresh Node 4 -> MUST catch up via live multi-chunk `InstallSnapshot` (`snapshot_transfer_chunk_size = 512`)!
    {
        const uint16_t raft_port4 = pickFreeLoopbackPort();
        auto cfg4 = make_cfg(4, raft_port4);
        auto node4 = std::make_shared<DB::KeeperDispatcher>();
        SCOPE_EXIT({
            shutdownDispatcher(node4);
        });
        node4->initialize(*cfg4, /*standalone_keeper=*/true, /*start_async=*/true, macros);

        {
            std::ostringstream js;
            js << R"({"preconditions":{"members":[1,2,3],"leaders":[)" << (leader_idx + 1)
               << R"(]},"actions":[{"add_members":[{"id":4,"endpoint":"127.0.0.1:)" << raft_port4
               << R"(","learner":false,"priority":1}]}]})";
            runRcfgCommand(*nodes[leader_idx], js.str(), "rcfg add_members(4) for chunked snapshot");
        }
        waitUntil(10000, [&]() { return node4->isServerActive() && node4->isFollower(); },
            "Node 4 did not complete live chunked InstallSnapshot");
        {
            auto zk4 = makeOverDispatcherClient(node4);
            require(zk4->get("/snap_big") == big_payload && zk4->get("/snap_k24") == "val-24",
                "Node 4 missing snapshot-installed keys");
        }
        {
            std::ostringstream js;
            js << R"({"preconditions":{"members":[1,2,3,4],"leaders":[)" << (leader_idx + 1)
               << R"(]},"actions":[{"remove_members":[4]}]})";
            runRcfgCommand(*nodes[leader_idx], js.str(), "rcfg remove_members(4)");
        }
    }

    /// Kill the leader's TCP listener + Dispatcher and verify multi-host `zkutil::ZooKeeper` + `ZooKeeperRetriesControl`
    /// fails over seamlessly to one of the 2 surviving nodes!
    stop_node(leader_idx);
    waitUntil(8000, [&]()
    {
        const int nl = find_leader();
        return nl >= 0 && nl != leader_idx;
    }, "Surviving 2/3 nodes failed to elect new leader for multi-host client failover");

    DB::ZooKeeperRetriesInfo retry_info(/*max_retries_=*/10, /*initial_backoff_ms_=*/50, /*max_backoff_ms_=*/250, nullptr);
    DB::ZooKeeperRetriesControl retry_ctrl("sim_multihost_failover", nullptr, retry_info);
    std::string recovered_val;
    retry_ctrl.retryLoop([&]()
    {
        if (zk->expired())
            zk = zk->startNewSession();
        recovered_val = zk->get("/snap_big");
        zk->create("/after_multihost_failover", "failover-ok", zkutil::CreateMode::Persistent);
    });
    require(recovered_val == big_payload && zk->get("/after_multihost_failover") == "failover-ok",
        "Multi-host ZooKeeper client failover failed");

    std::cout << "  [PASS] live: 3node_chunked_snapshot_install_and_multihost_client_failover\n";
}

/// 14. Live client `path_acls` (`apply_to_children`), Availability Zone preference (`prefer_local_availability_zone`),
/// dynamic `<raft_logs_level>` hot reload, and `<create_snapshot_on_exit>true</create_snapshot_on_exit>`!
inline void testLivePathACLsSnapshotOnExitDynamicLogLevelAndAZPreference(const std::filesystem::path & root_dir)
{
    const auto base_dir = root_dir / "live_path_acl_snap_exit_az";
    std::filesystem::create_directories(base_dir / "logs");
    std::filesystem::create_directories(base_dir / "snapshots");

    Poco::Net::ServerSocket tcp_socket1(Poco::Net::SocketAddress("127.0.0.1", 0));
    Poco::Net::ServerSocket tcp_socket2(Poco::Net::SocketAddress("127.0.0.1", 0));
    const uint16_t tcp_port1 = tcp_socket1.address().port();
    const uint16_t tcp_port2 = tcp_socket2.address().port();
    const uint16_t raft_port = pickFreeLoopbackPort();
    auto macros = std::make_shared<const DB::Macros>();

    auto make_xml = [&](const std::string & raft_log_lvl)
    {
        std::ostringstream xml;
        xml << "<clickhouse><keeper_server>\n"
            << "  <tcp_port>" << tcp_port1 << "</tcp_port>\n"
            << "  <server_id>1</server_id>\n"
            << "  <create_snapshot_on_exit>true</create_snapshot_on_exit>\n"
            << "  <log_storage_path>" << (base_dir / "logs").string() << "</log_storage_path>\n"
            << "  <snapshot_storage_path>" << (base_dir / "snapshots").string() << "</snapshot_storage_path>\n"
            << "  <coordination_settings>\n"
            << "    <operation_timeout_ms>5000</operation_timeout_ms>\n"
            << "    <startup_timeout>5000</startup_timeout>\n"
            << "    <session_timeout_ms>10000</session_timeout_ms>\n"
            << "    <raft_logs_level>" << raft_log_lvl << "</raft_logs_level>\n"
            << "    <snapshot_distance>100000</snapshot_distance>\n"
            << "    <use_new_dispatcher>true</use_new_dispatcher>\n"
            << "  </coordination_settings>\n"
            << "  <raft_configuration>\n"
            << "    <server><id>1</id><hostname>127.0.0.1</hostname><port>" << raft_port << "</port></server>\n"
            << "  </raft_configuration>\n</keeper_server></clickhouse>\n";
        std::istringstream iss(xml.str());
        return Poco::AutoPtr<Poco::Util::XMLConfiguration>(new Poco::Util::XMLConfiguration(iss));
    };

    auto cfg = make_xml("error");
    auto dispatcher = std::make_shared<DB::KeeperDispatcher>();
    dispatcher->initialize(*cfg, /*standalone_keeper=*/true, /*start_async=*/true, macros);
    waitUntilActiveLeader(*dispatcher, 8000);

    {
        Poco::ThreadPool server_pool(2, 16);
        auto make_tcp_srv = [&](Poco::Net::ServerSocket & sock)
        {
            auto factory = Poco::SharedPtr<DB::TCPServerConnectionFactory>(new DB::KeeperTCPHandlerFactory(
                [cfg]() -> const Poco::Util::AbstractConfiguration & { return *cfg; },
                dispatcher, 5, 5, false));
            Poco::Net::TCPServerParams::Ptr params = new Poco::Net::TCPServerParams();
            params->setMaxThreads(8);
            auto srv = std::make_unique<DB::TCPServer>(factory, server_pool, sock, params);
            srv->start();
            return srv;
        };
        auto srv1 = make_tcp_srv(tcp_socket1);
        auto srv2 = make_tcp_srv(tcp_socket2);
        SCOPE_EXIT({
            srv1->stop();
            srv2->stop();
        });

        /// 1. Client-side `ZooKeeperArgs::path_acls` (`apply_to_children = true`).
        {
            zkutil::ZooKeeperArgs args("127.0.0.1:" + std::to_string(tcp_port1));
            args.connection_timeout_ms = 5000;
            args.session_timeout_ms = 10000;
            args.operation_timeout_ms = 5000;
            args.path_acls["/secured"] = zkutil::ZooKeeperArgs::PathAclInfo{
                .acl = Coordination::ACL{Coordination::ACL::Read, "world", "anyone"},
                .apply_to_children = true,
            };
            auto zk = zkutil::ZooKeeper::create(args, nullptr, nullptr);
            zk->create("/secured", "root", zkutil::CreateMode::Persistent);
            zk->create("/secured/child", "data", zkutil::CreateMode::Persistent);

            auto acls = zk->getACL("/secured/child");
            bool has_read_only_acl = false;
            for (const auto & a : acls)
                if (a.permissions == Coordination::ACL::Read && a.scheme == "world" && a.id == "anyone")
                    has_read_only_acl = true;
            require(has_read_only_acl, "ZooKeeperArgs::path_acls did not attach child ACL to /secured/child");
        }

        /// 2. Availability Zone preference (`prefer_local_availability_zone = true`) selects `tcp_port2` (`az-local`)!
        {
            zkutil::ZooKeeperArgs az_args("127.0.0.1:" + std::to_string(tcp_port1) + ",127.0.0.1:" + std::to_string(tcp_port2));
            az_args.connection_timeout_ms = 5000;
            az_args.session_timeout_ms = 10000;
            az_args.operation_timeout_ms = 5000;
            az_args.availability_zones = {"az-remote", "az-local"};
            az_args.client_availability_zone = "az-local";
            az_args.prefer_local_availability_zone = true;
            auto zk_az = zkutil::ZooKeeper::create(az_args, nullptr, nullptr);
            const std::string connected = zk_az->getConnectedHostPort();
            require(connected == "127.0.0.1:" + std::to_string(tcp_port2),
                "AZ-aware client connected to wrong endpoint: " + connected);
        }

        /// 3. Dynamic `<raft_logs_level>` hot reload via `updateConfiguration` (`error -> warning -> error`)!
        {
            auto cfg_warn = make_xml("warning");
            dispatcher->updateConfiguration(*cfg_warn, macros);
            auto cfg_err = make_xml("error");
            dispatcher->updateConfiguration(*cfg_err, macros);
        }
    }

    /// 4. `<create_snapshot_on_exit>true</create_snapshot_on_exit>`: zero snapshots before exit, >= 1 after clean exit!
    auto count_snapshots = [&]() -> size_t
    {
        size_t n = 0;
        for (const auto & e : std::filesystem::directory_iterator(base_dir / "snapshots"))
            if (e.path().filename().string().starts_with("snapshot_"))
                ++n;
        return n;
    };
    require(count_snapshots() == 0, "Unexpected snapshot before shutdown despite snapshot_distance=100000");

    shutdownDispatcher(dispatcher);
    require(count_snapshots() >= 1, "create_snapshot_on_exit=true did not write a snapshot during clean shutdown");

    std::cout << "  [PASS] live: path_acls_snapshot_on_exit_dynamic_log_level_and_az_preference\n";
}

/// 15. Live 3-node cluster: rejoin a removed follower WITHOUT restarting it (`test_keeper_rejoin_removed_member_without_restart`)
/// AND atomic single-command `OpNum::Reconfig` leader replacement (`test_keeper_reconfig_replace_leader_in_one_command`)!
inline void testLive3NodeRejoinWithoutRestartAndAtomicLeaderReplace(const std::filesystem::path & root_dir)
{
    const auto base_dir = root_dir / "live_3node_rejoin_and_replace_leader";
    std::filesystem::create_directories(base_dir);

    const std::array<uint16_t, 4> raft_ports = {
        pickFreeLoopbackPort(),
        pickFreeLoopbackPort(),
        pickFreeLoopbackPort(),
        pickFreeLoopbackPort(),
    };

    auto make_cfg = [&](int id, bool include_node4 = false)
    {
        const auto ndir = base_dir / ("node_" + std::to_string(id));
        std::filesystem::create_directories(ndir / "logs");
        std::filesystem::create_directories(ndir / "snapshots");
        std::ostringstream xml;
        xml << "<clickhouse><keeper_server>\n"
            << "  <tcp_port>" << pickFreeLoopbackPort() << "</tcp_port>\n"
            << "  <server_id>" << id << "</server_id>\n"
            << "  <enable_reconfiguration>true</enable_reconfiguration>\n"
            << "  <log_storage_path>" << (ndir / "logs").string() << "</log_storage_path>\n"
            << "  <snapshot_storage_path>" << (ndir / "snapshots").string() << "</snapshot_storage_path>\n"
            << "  <coordination_settings>\n"
            << "    <operation_timeout_ms>5000</operation_timeout_ms>\n"
            << "    <startup_timeout>8000</startup_timeout>\n"
            << "    <session_timeout_ms>10000</session_timeout_ms>\n"
            << "    <sleep_before_leader_change_ms>200</sleep_before_leader_change_ms>\n"
            << "    <raft_logs_level>error</raft_logs_level>\n"
            << "    <heart_beat_interval_ms>50</heart_beat_interval_ms>\n"
            << "    <election_timeout_lower_bound_ms>150</election_timeout_lower_bound_ms>\n"
            << "    <election_timeout_upper_bound_ms>300</election_timeout_upper_bound_ms>\n"
            << "    <use_new_dispatcher>true</use_new_dispatcher>\n"
            << "  </coordination_settings>\n"
            << "  <raft_configuration>\n";
        for (int sid = 1; sid <= 3; ++sid)
            xml << "    <server><id>" << sid << "</id><hostname>127.0.0.1</hostname><port>" << raft_ports[sid - 1] << "</port></server>\n";
        if (include_node4)
            xml << "    <server><id>4</id><hostname>127.0.0.1</hostname><port>" << raft_ports[3] << "</port><start_as_follower>true</start_as_follower></server>\n";
        xml << "  </raft_configuration>\n</keeper_server></clickhouse>\n";
        std::istringstream iss(xml.str());
        return Poco::AutoPtr<Poco::Util::XMLConfiguration>(new Poco::Util::XMLConfiguration(iss));
    };

    auto macros = std::make_shared<const DB::Macros>();
    std::array<std::shared_ptr<DB::KeeperDispatcher>, 4> nodes;
    SCOPE_EXIT({
        for (auto & n : nodes)
        {
            shutdownDispatcher(n);
        }
    });

    for (int i = 0; i < 3; ++i)
    {
        auto cfg = make_cfg(i + 1);
        nodes[i] = std::make_shared<DB::KeeperDispatcher>();
        nodes[i]->initialize(*cfg, /*standalone_keeper=*/true, /*start_async=*/true, macros);
    }


    auto find_leader = [&]() -> int
    {
        for (int i = 0; i < 4; ++i)
            if (nodes[i] && nodes[i]->isServerActive() && nodes[i]->isLeader())
                return i;
        return -1;
    };

    int leader_idx = -1;
    waitUntil(10000, [&]()
    {
        leader_idx = find_leader();
        return leader_idx >= 0;
    }, "3-node rejoin cluster did not elect leader");
    const int fol_idx = (leader_idx + 1) % 3;
    const int fol_id = fol_idx + 1;

    /// 1. Remove follower `fol_id`, write `/while_removed`, then re-add SAME still-running `nodes[fol_idx]` WITHOUT restart!
    {
        {
            std::ostringstream js;
            js << R"({"preconditions":{"members":[1,2,3],"leaders":[)" << (leader_idx + 1)
               << R"(]},"actions":[{"remove_members":[)" << fol_id << R"(]}]})";
            runRcfgCommand(*nodes[leader_idx], js.str(), "rcfg remove follower");
        }
        auto zk_l = makeOverDispatcherClient(nodes[leader_idx]);
        zk_l->create("/while_removed", "written-while-out", zkutil::CreateMode::Persistent);

        {
            std::ostringstream js;
            js << R"({"actions":[{"add_members":[{"id":)" << fol_id << R"(,"endpoint":"127.0.0.1:)" << raft_ports[fol_idx]
               << R"(","learner":false,"priority":1}]}]})";
            runRcfgCommand(*nodes[leader_idx], js.str(), "rcfg re-add follower without restart");
        }
        waitUntil(8000, [&]()
        {
            return nodes[fol_idx]->isServerActive() && nodes[fol_idx]->isFollower();
        }, "Rejoined follower did not become active without restart");
        auto zk_f = makeOverDispatcherClient(nodes[fol_idx]);
        require(zk_f->get("/while_removed") == "written-while-out", "Rejoined follower missed /while_removed");
    }

    /// 2. Start Node 4 and atomically replace the current leader (`leader_id`) with Node 4 in ONE `OpNum::Reconfig` command!
    {
        auto cfg4 = make_cfg(4, /*include_node4=*/true);
        nodes[3] = std::make_shared<DB::KeeperDispatcher>();
        nodes[3]->initialize(*cfg4, /*standalone_keeper=*/true, /*start_async=*/true, macros);

        const int old_leader_idx = find_leader();
        const int old_leader_id = old_leader_idx + 1;
        const int other_idx = (old_leader_idx + 1) % 3;

        auto ov_other = std::make_unique<Coordination::KeeperOverDispatcher>(nodes[other_idx], Poco::Timespan(15, 0));
        auto done = std::make_shared<Poco::Event>();
        Coordination::ReconfigResponse rres;
        ov_other->reconfig(
            "server.4=127.0.0.1:" + std::to_string(raft_ports[3]),
            std::to_string(old_leader_id),
            "",
            -1,
            [&](const Coordination::ReconfigResponse & r)
            {
                rres = r;
                done->set();
            });
        require(done->tryWait(12000) && rres.error == Coordination::Error::ZOK,
            "Atomic single-command leader replacement OpNum::Reconfig failed");
        require(rres.value.find("server.4=") != std::string::npos
            && rres.value.find("server." + std::to_string(old_leader_id) + "=") == std::string::npos,
            "Atomic leader replacement config mismatch: " + rres.value);

        waitUntil(10000, [&]()
        {
            const int nl = find_leader();
            return nl >= 0 && nl != old_leader_idx && nodes[3]->isServerActive();
        }, "New 3-node cluster without old leader did not stabilize");

        auto zk4 = makeOverDispatcherClient(nodes[3]);
        require(zk4->get("/while_removed") == "written-while-out", "Replacement Node 4 missing prior state");
        zk4->create("/after_atomic_replace", "replaced-ok", zkutil::CreateMode::Persistent);
        require(zk4->get("/after_atomic_replace") == "replaced-ok", "Write after atomic leader replace failed");
    }

    std::cout << "  [PASS] live: 3node_rejoin_without_restart_and_atomic_leader_replace\n";
}

/// 16. Live 2-node cluster (`test_keeper_two_nodes_cluster`): quorum election, live TCP `sync`,
/// quorum loss on 1-node shutdown, automatic quorum restoration on rejoin, AND `test_keeper_read_during_close`!
inline void testLive2NodeClusterQuorumLossRecoveryAndReadDuringClose(const std::filesystem::path & root_dir)
{
    const auto base_dir = root_dir / "live_2node_cluster";
    std::filesystem::create_directories(base_dir);

    const std::array<uint16_t, 2> raft_ports = {
        pickFreeLoopbackPort(),
        pickFreeLoopbackPort(),
    };
    Poco::Net::ServerSocket tcp_socket1(Poco::Net::SocketAddress("127.0.0.1", 0));
    const uint16_t tcp_port1 = tcp_socket1.address().port();

    auto make_cfg = [&](int id)
    {
        const auto ndir = base_dir / ("node_" + std::to_string(id));
        std::filesystem::create_directories(ndir / "logs");
        std::filesystem::create_directories(ndir / "snapshots");
        std::ostringstream xml;
        xml << "<clickhouse><keeper_server>\n"
            << "  <tcp_port>" << (id == 1 ? tcp_port1 : pickFreeLoopbackPort()) << "</tcp_port>\n"
            << "  <server_id>" << id << "</server_id>\n"
            << "  <four_letter_word_white_list>*</four_letter_word_white_list>\n"
            << "  <log_storage_path>" << (ndir / "logs").string() << "</log_storage_path>\n"
            << "  <snapshot_storage_path>" << (ndir / "snapshots").string() << "</snapshot_storage_path>\n"
            << "  <coordination_settings>\n"
            << "    <operation_timeout_ms>1500</operation_timeout_ms>\n"
            << "    <startup_timeout>8000</startup_timeout>\n"
            << "    <session_timeout_ms>10000</session_timeout_ms>\n"
            << "    <raft_logs_level>error</raft_logs_level>\n"
            << "    <heart_beat_interval_ms>50</heart_beat_interval_ms>\n"
            << "    <election_timeout_lower_bound_ms>150</election_timeout_lower_bound_ms>\n"
            << "    <election_timeout_upper_bound_ms>300</election_timeout_upper_bound_ms>\n"
            << "    <use_new_dispatcher>true</use_new_dispatcher>\n"
            << "  </coordination_settings>\n"
            << "  <raft_configuration>\n"
            << "    <server><id>1</id><hostname>127.0.0.1</hostname><port>" << raft_ports[0] << "</port></server>\n"
            << "    <server><id>2</id><hostname>127.0.0.1</hostname><port>" << raft_ports[1] << "</port></server>\n"
            << "  </raft_configuration>\n</keeper_server></clickhouse>\n";
        std::istringstream iss(xml.str());
        return Poco::AutoPtr<Poco::Util::XMLConfiguration>(new Poco::Util::XMLConfiguration(iss));
    };

    auto macros = std::make_shared<const DB::Macros>();
    std::array<std::shared_ptr<DB::KeeperDispatcher>, 2> nodes;
    auto cfg1 = make_cfg(1);
    auto cfg2 = make_cfg(2);
    nodes[0] = std::make_shared<DB::KeeperDispatcher>();
    nodes[1] = std::make_shared<DB::KeeperDispatcher>();
    nodes[0]->initialize(*cfg1, /*standalone_keeper=*/true, /*start_async=*/true, macros);
    nodes[1]->initialize(*cfg2, /*standalone_keeper=*/true, /*start_async=*/true, macros);
    DB::FourLetterCommandFactory::registerCommands(*nodes[0]);

    Poco::ThreadPool server_pool(2, 16);
    auto tcp_factory = Poco::SharedPtr<DB::TCPServerConnectionFactory>(new DB::KeeperTCPHandlerFactory(
        [cfg1]() -> const Poco::Util::AbstractConfiguration & { return *cfg1; },
        nodes[0], 5, 5, false));
    Poco::Net::TCPServerParams::Ptr tcp_params = new Poco::Net::TCPServerParams();
    tcp_params->setMaxThreads(8);
    DB::TCPServer tcp_server(tcp_factory, server_pool, tcp_socket1, tcp_params);
    tcp_server.start();

    auto stop_node = [&](int i)
    {
        shutdownDispatcher(nodes[i]);
    };
    SCOPE_EXIT({
        tcp_server.stop();
        stop_node(0);
        stop_node(1);
    });


    waitUntil(8000, [&]()
    {
        return nodes[0]->isServerActive() && nodes[1]->isServerActive()
            && (nodes[0]->isLeader() || nodes[1]->isLeader());
    }, "2-node cluster did not reach active quorum");

    /// 1. Live TCP write + `ZooKeeper::sync` on 2-node cluster + concurrent reads during client close (`test_keeper_read_during_close`)!
    {
        zkutil::ZooKeeperArgs args("127.0.0.1:" + std::to_string(tcp_port1));
        args.connection_timeout_ms = 5000;
        args.session_timeout_ms = 10000;
        args.operation_timeout_ms = 5000;
        auto zk = zkutil::ZooKeeper::create(args, nullptr, nullptr);
        zk->create("/two_node_key", "quorum-2of2", zkutil::CreateMode::Persistent);
        require(zk->sync("/two_node_key") == "/two_node_key", "Live TCP ZooKeeper::sync failed");
        require(zk->get("/two_node_key") == "quorum-2of2", "2-node cluster get mismatch");

        /// Fire asynchronous reads right as the client closes its session (must never crash server!).
        std::vector<std::future<Coordination::GetResponse>> inflight;
        inflight.reserve(16);
        for (int i = 0; i < 16; ++i)
            inflight.push_back(zk->asyncTryGetNoThrow("/two_node_key"));
        zk.reset();
        for (auto & f : inflight)
        {
            try { (void)f.get(); }
            catch (...) {}
        }
        require(sendRawTCPFourLetterWord(tcp_port1, "ruok") == "imok",
            "Server unhealthy after read_during_close");
    }

    /// 2. Stop Node 2 -> 2-node cluster loses quorum (1/2 cannot commit writes), then restart Node 2 -> quorum restores!
    stop_node(1);
    std::this_thread::sleep_for(std::chrono::milliseconds(350));

    nodes[1] = std::make_shared<DB::KeeperDispatcher>();
    nodes[1]->initialize(*cfg2, /*standalone_keeper=*/true, /*start_async=*/true, macros);
    waitUntil(8000, [&]()
    {
        return nodes[0]->isServerActive() && nodes[1]->isServerActive()
            && (nodes[0]->isLeader() || nodes[1]->isLeader());
    }, "2-node cluster did not restore quorum after Node 2 restart");

    {
        zkutil::ZooKeeperArgs args("127.0.0.1:" + std::to_string(tcp_port1));
        args.connection_timeout_ms = 5000;
        args.session_timeout_ms = 10000;
        args.operation_timeout_ms = 5000;
        auto zk2 = zkutil::ZooKeeper::create(args, nullptr, nullptr);
        zk2->create("/two_node_restored", "back-2of2", zkutil::CreateMode::Persistent);
        require(zk2->get("/two_node_key") == "quorum-2of2" && zk2->get("/two_node_restored") == "back-2of2",
            "2-node cluster post-recovery read/write failed");
    }

    std::cout << "  [PASS] live: 2node_quorum_loss_recovery_sync_and_read_during_close\n";
}

/// 17. Live 3-node cluster exercising multi-threaded per-peer & commit `Changelog` `ReadAhead`
///     (small window + chunk size + idle reader eviction + reader recreation + bounded `latest_logs_cache`)
///     and background-thread NuRaft snapshot I/O (`nuraft_use_bg_thread_for_snapshot_io = true`).
inline void testLive3NodeReadAheadCommitCacheAndBgSnapIO(const std::filesystem::path & root_dir)
{
    const auto base_dir = root_dir / "live_3node_readahead_bg_snap";
    std::array<uint16_t, 3> raft_ports{pickFreeLoopbackPort(), pickFreeLoopbackPort(), pickFreeLoopbackPort()};
    std::array<uint16_t, 3> tcp_ports{pickFreeLoopbackPort(), pickFreeLoopbackPort(), pickFreeLoopbackPort()};

    auto make_cfg = [&](int id)
    {
        const auto ndir = base_dir / ("node_" + std::to_string(id));
        std::filesystem::create_directories(ndir / "logs");
        std::filesystem::create_directories(ndir / "snapshots");
        std::ostringstream xml;
        xml << "<clickhouse><keeper_server>\n"
            << "  <tcp_port>" << tcp_ports[static_cast<size_t>(id - 1)] << "</tcp_port>\n"
            << "  <server_id>" << id << "</server_id>\n"
            << "  <enable_ipv6>false</enable_ipv6>\n"
            << "  <log_storage_path>" << (ndir / "logs").string() << "</log_storage_path>\n"
            << "  <snapshot_storage_path>" << (ndir / "snapshots").string() << "</snapshot_storage_path>\n"
            << "  <four_letter_word_white_list>*</four_letter_word_white_list>\n"
            << "  <coordination_settings>\n"
            << "    <operation_timeout_ms>5000</operation_timeout_ms>\n"
            << "    <startup_timeout>8000</startup_timeout>\n"
            << "    <session_timeout_ms>10000</session_timeout_ms>\n"
            << "    <raft_logs_level>error</raft_logs_level>\n"
            << "    <heart_beat_interval_ms>100</heart_beat_interval_ms>\n"
            << "    <election_timeout_lower_bound_ms>250</election_timeout_lower_bound_ms>\n"
            << "    <election_timeout_upper_bound_ms>500</election_timeout_upper_bound_ms>\n"
            << "    <rotate_log_storage_interval>12</rotate_log_storage_interval>\n"
            << "    <snapshot_distance>200</snapshot_distance>\n"
            << "    <reserved_log_items>8</reserved_log_items>\n"
            << "    <latest_logs_cache_entry_count_threshold>6</latest_logs_cache_entry_count_threshold>\n"
            << "    <log_readahead_enabled>true</log_readahead_enabled>\n"
            << "    <log_readahead_window_bytes>4096</log_readahead_window_bytes>\n"
            << "    <log_readahead_chunk_size>4</log_readahead_chunk_size>\n"
            << "    <log_readahead_max_peer_readers>2</log_readahead_max_peer_readers>\n"
            << "    <log_readahead_eviction_timeout_ms>200</log_readahead_eviction_timeout_ms>\n"
            << "    <log_readahead_pool_threads>2</log_readahead_pool_threads>\n"
            << "    <log_readahead_commit_window_bytes>4096</log_readahead_commit_window_bytes>\n"
            << "    <nuraft_use_bg_thread_for_snapshot_io>true</nuraft_use_bg_thread_for_snapshot_io>\n"
            << "    <compress_logs>false</compress_logs>\n"
            << "    <force_sync>false</force_sync>\n"
            << "    <use_new_dispatcher>true</use_new_dispatcher>\n"
            << "  </coordination_settings>\n"
            << "  <raft_configuration>\n";
        for (int s = 1; s <= 3; ++s)
        {
            xml << "    <server><id>" << s << "</id><hostname>127.0.0.1</hostname><port>"
                << raft_ports[static_cast<size_t>(s - 1)] << "</port></server>\n";
        }
        xml << "  </raft_configuration>\n</keeper_server></clickhouse>\n";
        std::istringstream iss(xml.str());
        return Poco::AutoPtr<Poco::Util::XMLConfiguration>(new Poco::Util::XMLConfiguration(iss));
    };

    std::array<Poco::AutoPtr<Poco::Util::XMLConfiguration>, 3> cfgs{make_cfg(1), make_cfg(2), make_cfg(3)};
    std::array<std::shared_ptr<DB::KeeperDispatcher>, 3> nodes;
    auto macros = std::make_shared<DB::Macros>();

    auto stop_node = [&](size_t idx)
    {
        shutdownDispatcher(nodes[idx]);
    };
    SCOPE_EXIT({
        for (size_t i = 0; i < 3; ++i)
            stop_node(i);
    });

    for (size_t i = 0; i < 3; ++i)
    {
        nodes[i] = std::make_shared<DB::KeeperDispatcher>();
        nodes[i]->initialize(*cfgs[i], /*standalone_keeper=*/true, /*start_async=*/true, macros);
    }

    auto find_leader = [&]() -> int
    {
        for (int i = 0; i < 3; ++i)
            if (nodes[static_cast<size_t>(i)] && nodes[static_cast<size_t>(i)]->isServerActive() && nodes[static_cast<size_t>(i)]->isLeader())
                return i;
        return -1;
    };

    waitUntil(10000, [&]()
    {
        if (find_leader() < 0)
            return false;
        for (size_t i = 0; i < 3; ++i)
            if (!nodes[i]->isServerActive())
                return false;
        return true;
    }, "3-node ReadAhead cluster did not stabilize");

    const size_t leader_idx = static_cast<size_t>(find_leader());
    const size_t lag_idx = (leader_idx + 1) % 3;

    /// Phase 1: Write 20 entries across rotated changelogs so entries evict from `latest_logs_cache` (limit=6).
    auto zk = makeOverDispatcherClient(nodes[leader_idx]);
    const std::string payload(512, 'R');
    for (int i = 0; i < 20; ++i)
        zk->create("/ra_k" + std::to_string(i), payload + std::to_string(i), zkutil::CreateMode::Persistent);

    /// Phase 2: Stop one follower, append 30 more entries across rotated changelog files on the leader,
    /// then restart the follower so per-peer `ReadAheadReader` decodes and serves entries from disk!
    stop_node(lag_idx);
    for (int i = 20; i < 50; ++i)
        zk->create("/ra_k" + std::to_string(i), payload + std::to_string(i), zkutil::CreateMode::Persistent);

    nodes[lag_idx] = std::make_shared<DB::KeeperDispatcher>();
    nodes[lag_idx]->initialize(*cfgs[lag_idx], /*standalone_keeper=*/true, /*start_async=*/true, macros);
    waitUntil(10000, [&]() { return nodes[lag_idx]->isServerActive(); },
        "Restarted follower did not become active");
    {
        auto zkf = makeOverDispatcherClient(nodes[lag_idx]);
        waitUntil(10000, [&]()
        {
            try
            {
                return zkf->exists("/ra_k49");
            }
            catch (...)
            {
                return false;
            }
        }, "Restarted follower did not catch up via per-peer Changelog ReadAhead");
    }

    /// Phase 3: Wait past `log_readahead_eviction_timeout_ms` (200ms) so idle peer/commit readers evict,
    /// then append 15 more keys to trigger clean reader recreation and convergence!
    std::this_thread::sleep_for(std::chrono::milliseconds(450));
    for (int i = 50; i < 65; ++i)
        zk->create("/ra_k" + std::to_string(i), payload + std::to_string(i), zkutil::CreateMode::Persistent);

    {
        auto zkf = makeOverDispatcherClient(nodes[lag_idx]);
        waitUntil(8000, [&]()
        {
            try
            {
                return zkf->exists("/ra_k64");
            }
            catch (...)
            {
                return false;
            }
        }, "Follower did not converge after ReadAhead idle reader eviction + recreation");
    }

    /// Phase 4: Trigger snapshot + log compaction on leader, wipe follower state, and restart it
    /// to verify `nuraft_use_bg_thread_for_snapshot_io = true` live snapshot transfer!
    (void)nodes[leader_idx]->createSnapshot();
    waitUntil(5000, [&]()
    {
        return nodes[leader_idx]->getKeeperLogInfo().first_log_idx > 1;
    }, "Leader did not compact logs after snapshot for bg_thread_for_snapshot_io");

    stop_node(lag_idx);
    const auto lag_dir = base_dir / ("node_" + std::to_string(lag_idx + 1));
    std::filesystem::remove_all(lag_dir / "logs");
    std::filesystem::remove_all(lag_dir / "snapshots");
    std::filesystem::create_directories(lag_dir / "logs");
    std::filesystem::create_directories(lag_dir / "snapshots");

    nodes[lag_idx] = std::make_shared<DB::KeeperDispatcher>();
    nodes[lag_idx]->initialize(*cfgs[lag_idx], /*standalone_keeper=*/true, /*start_async=*/true, macros);
    waitUntil(10000, [&]() { return nodes[lag_idx]->isServerActive(); },
        "Wiped follower did not become active after bg snapshot install");
    {
        auto zkf = makeOverDispatcherClient(nodes[lag_idx]);
        waitUntil(10000, [&]()
        {
            try
            {
                return zkf->get("/ra_k0") == payload + "0" && zkf->get("/ra_k64") == payload + "64";
            }
            catch (...)
            {
                return false;
            }
        }, "Wiped follower failed snapshot install with nuraft_use_bg_thread_for_snapshot_io=true");
    }

    std::cout << "  [PASS] live: 3node_readahead_commit_cache_and_bg_snap\n";
}

/// 18. Live dispatcher queue-limit overload stress (both `KeeperRequestDispatcherNew` and
///     `KeeperRequestDispatcherOld` with tight request/response/batch/uncommitted limits + shutdown under load)
///     and HTTP 4LW allow-list enforcement (`#134123`).
inline void testLiveDispatcherOverloadQueueLimitsAndHttp4LWAllowList(const std::filesystem::path & root_dir)
{
    const auto base_dir = root_dir / "live_overload_and_http_4lw";
    std::vector<std::string> recorded_bugs;

    auto run_dispatcher_overload = [&](bool use_new_dispatcher, bool test_http_4lw_acl)
    {
        const auto sub_dir = base_dir / (use_new_dispatcher ? "new_disp" : "old_disp");
        std::filesystem::create_directories(sub_dir / "logs");
        std::filesystem::create_directories(sub_dir / "snapshots");
        const uint16_t raft_port = pickFreeLoopbackPort();
        Poco::Net::ServerSocket tcp_socket(Poco::Net::SocketAddress("127.0.0.1", 0));
        const uint16_t tcp_port = tcp_socket.address().port();

        std::ostringstream xml;
        xml << "<clickhouse><keeper_server>\n"
            << "  <tcp_port>" << tcp_port << "</tcp_port>\n"
            << "  <server_id>1</server_id>\n"
            << "  <enable_ipv6>false</enable_ipv6>\n"
            << "  <log_storage_path>" << (sub_dir / "logs").string() << "</log_storage_path>\n"
            << "  <snapshot_storage_path>" << (sub_dir / "snapshots").string() << "</snapshot_storage_path>\n"
            << "  <four_letter_word_white_list>ruok,mntr</four_letter_word_white_list>\n"
            << "  <coordination_settings>\n"
            << "    <operation_timeout_ms>5000</operation_timeout_ms>\n"
            << "    <startup_timeout>5000</startup_timeout>\n"
            << "    <session_timeout_ms>10000</session_timeout_ms>\n"
            << "    <raft_logs_level>error</raft_logs_level>\n"
            << "    <heart_beat_interval_ms>100</heart_beat_interval_ms>\n"
            << "    <election_timeout_lower_bound_ms>200</election_timeout_lower_bound_ms>\n"
            << "    <election_timeout_upper_bound_ms>400</election_timeout_upper_bound_ms>\n"
            << "    <use_new_dispatcher>" << (use_new_dispatcher ? "true" : "false") << "</use_new_dispatcher>\n"
            << "    <max_request_queue_size>16</max_request_queue_size>\n"
            << "    <max_request_queue_bytes_size>16384</max_request_queue_bytes_size>\n"
            << "    <max_response_queue_bytes_size>16384</max_response_queue_bytes_size>\n"
            << "    <max_in_flight_request_batches>2</max_in_flight_request_batches>\n"
            << "    <max_requests_batch_bytes_size>4096</max_requests_batch_bytes_size>\n"
            << "    <max_read_batch_size>4</max_read_batch_size>\n"
            << "    <max_read_batch_bytes_size>4096</max_read_batch_bytes_size>\n"
            << "    <parallel_read_threads>2</parallel_read_threads>\n"
            << "    <parallel_read_chunk_size>2</parallel_read_chunk_size>\n"
            << "    <parallel_read_min_batch>2</parallel_read_min_batch>\n"
            << "    <nuraft_max_uncommitted_log_entries>16</nuraft_max_uncommitted_log_entries>\n"
            << "    <force_sync>false</force_sync>\n"
            << "  </coordination_settings>\n"
            << "  <raft_configuration><server><id>1</id><hostname>127.0.0.1</hostname><port>" << raft_port << "</port></server></raft_configuration>\n"
            << "</keeper_server></clickhouse>\n";

        std::istringstream iss(xml.str());
        Poco::AutoPtr<Poco::Util::XMLConfiguration> config(new Poco::Util::XMLConfiguration(iss));

        auto dispatcher = std::make_shared<DB::KeeperDispatcher>();
        auto macros = std::make_shared<DB::Macros>();
        dispatcher->initialize(*config, /*standalone_keeper=*/true, /*start_async=*/false, macros);
        waitUntilActiveLeader(*dispatcher);

        DB::FourLetterCommandFactory::registerCommands(*dispatcher);
        DB::FourLetterCommandFactory::instance().initializeAllowList(*dispatcher);

        Poco::ThreadPool server_pool(2, 16);
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

        /// Part A: Verify restricted 4LW allow-list (`ruok,mntr` only -> `csnp` disabled!) on BOTH TCP and HTTP (`#134123`).
        if (test_http_4lw_acl)
        {
            require(sendRawTCPFourLetterWord(tcp_port, "ruok") == "imok", "Restricted-ACL TCP ruok failed");
            const auto tcp_csnp = sendRawTCPFourLetterWord(tcp_port, "csnp");
            require(tcp_csnp.find("not in the white list") != std::string::npos || tcp_csnp.empty(),
                "Restricted-ACL TCP csnp unexpectedly succeeded: " + tcp_csnp);

            Poco::Net::ServerSocket http_socket(Poco::Net::SocketAddress("127.0.0.1", 0));
            const uint16_t http_port = http_socket.address().port();
            auto keeper_http_client = std::make_shared<DB::KeeperHTTPClient>([dispatcher]() -> std::shared_ptr<zkutil::ZooKeeper>
            {
                return makeOverDispatcherClient(dispatcher);
            });
            auto main_factory = std::make_shared<DB::KeeperHTTPRequestHandlerFactory>("SimKeeperHTTP4LWACL");
            auto h = std::make_shared<DB::HandlingRuleHTTPHandlerFactory<DB::KeeperHTTPCommandsHandler>>(
                [dispatcher, keeper_http_client]()
                { return std::make_unique<DB::KeeperHTTPCommandsHandler>(dispatcher, keeper_http_client); });
            h->attachNonStrictPath("/api/v1/commands");
            h->allowRESTMethods();
            main_factory->addHandler(h);

            auto http_ctx = std::make_shared<SimHTTPContext>();
            Poco::Net::HTTPServerParams::Ptr http_params = new Poco::Net::HTTPServerParams();
            http_params->setMaxThreads(4);
            DB::HTTPServer http_server(http_ctx, main_factory, server_pool, http_socket, http_params);
            http_server.start();

            auto r_ruok = sendLoopbackHTTP(http_port, "GET", "/api/v1/commands?command=ruok");
            require(r_ruok.status == 200 && r_ruok.body.find("imok") != std::string::npos,
                "HTTP /api/v1/commands?command=ruok failed under restricted allow-list");

            auto r_csnp = sendLoopbackHTTP(http_port, "GET", "/api/v1/commands?command=csnp");
            if (r_csnp.status == 200 && r_csnp.body.find("not in the white list") == std::string::npos
                && r_csnp.body.find("disabled") == std::string::npos)
            {
                recorded_bugs.push_back(
                    "HTTP /api/v1/commands?command=csnp bypassed four_letter_word_white_list (status=200 body="
                    + r_csnp.body + ") (#134123)");
            }

            http_server.stopAll(/*abortCurrent=*/true);

            /// Concurrent `ZooKeeper::initSession` TOCTOU race check: 4 threads connecting to a fresh Keeper
            /// with the same `zookeeper_name` must not fail with `ZNODEEXISTS` on `/clickhouse/sessions/zookeeper/<uuid>`.
            std::mutex init_err_mu;
            std::string init_err;
            std::vector<std::thread> init_threads;
            for (int t = 0; t < 4; ++t)
            {
                init_threads.emplace_back([&]()
                {
                    try
                    {
                        zkutil::ZooKeeperArgs args("127.0.0.1:" + std::to_string(tcp_port));
                        args.connection_timeout_ms = 5000;
                        args.session_timeout_ms = 10000;
                        args.operation_timeout_ms = 5000;
                        auto zk_race = zkutil::ZooKeeper::create(args, nullptr, nullptr);
                        (void)zk_race->exists("/");
                    }
                    catch (const std::exception & e)
                    {
                        std::lock_guard lock(init_err_mu);
                        if (init_err.empty())
                            init_err = std::string("Concurrent ZooKeeper::initSession threw: ") + e.what();
                    }
                });
            }
            for (auto & th : init_threads)
                th.join();
            if (!init_err.empty())
                recorded_bugs.push_back(init_err);
        }

        /// Part B/C: Saturate tight request/response/batch/uncommitted queues with 4 concurrent TCP clients!
        const std::string big_val(3072, 'Q');
        std::atomic<int> committed_ok{0};
        std::atomic<int> read_ok{0};
        std::mutex worker_err_mu;
        std::string worker_err;
        std::vector<std::thread> workers;
        for (int t = 0; t < 4; ++t)
        {
            workers.emplace_back([&, t]()
            {
                try
                {
                    zkutil::ZooKeeperArgs args("127.0.0.1:" + std::to_string(tcp_port));
                    args.zookeeper_name = "ovl_client_" + std::to_string(t);
                    args.connection_timeout_ms = 5000;
                    args.session_timeout_ms = 10000;
                    args.operation_timeout_ms = 5000;
                    auto zk = zkutil::ZooKeeper::create(args, nullptr, nullptr);

                    std::vector<std::future<Coordination::CreateResponse>> wfuts;
                    wfuts.reserve(16);
                    for (int i = 0; i < 16; ++i)
                    {
                        wfuts.push_back(zk->asyncTryCreateNoThrow(
                            "/ovl_" + std::to_string(t) + "_" + std::to_string(i),
                            big_val,
                            zkutil::CreateMode::Persistent));
                    }
                    for (auto & f : wfuts)
                    {
                        auto r = f.get();
                        if (r.error == Coordination::Error::ZOK)
                            committed_ok.fetch_add(1, std::memory_order_relaxed);
                    }

                    std::vector<std::future<Coordination::GetResponse>> rfuts;
                    rfuts.reserve(16);
                    for (int i = 0; i < 16; ++i)
                        rfuts.push_back(zk->asyncTryGetNoThrow("/ovl_" + std::to_string(t) + "_" + std::to_string(i)));
                    for (auto & f : rfuts)
                    {
                        auto r = f.get();
                        if (r.error == Coordination::Error::ZOK && r.data == big_val)
                            read_ok.fetch_add(1, std::memory_order_relaxed);
                    }
                }
                catch (const std::exception & e)
                {
                    std::lock_guard lock(worker_err_mu);
                    if (worker_err.empty())
                        worker_err = e.what();
                }
            });
        }
        for (auto & th : workers)
            th.join();
        require(worker_err.empty(), "Overload stress worker failed: " + worker_err);

        require(committed_ok.load() > 0 && read_ok.load() == committed_ok.load(),
            "Overload stress consistency mismatch (committed=" + std::to_string(committed_ok.load())
            + " read_ok=" + std::to_string(read_ok.load()) + ")");
        require(sendRawTCPFourLetterWord(tcp_port, "ruok") == "imok",
            "Server unhealthy after queue-limit overload burst");

        /// Trigger graceful dispatcher shutdown while an async write burst is actively in flight!
        zkutil::ZooKeeperArgs drain_args("127.0.0.1:" + std::to_string(tcp_port));
        drain_args.connection_timeout_ms = 5000;
        drain_args.session_timeout_ms = 10000;
        drain_args.operation_timeout_ms = 3000;
        auto zk_drain = zkutil::ZooKeeper::create(drain_args, nullptr, nullptr);
        std::vector<std::future<Coordination::CreateResponse>> drain_futs;
        for (int i = 0; i < 24; ++i)
            drain_futs.push_back(zk_drain->asyncTryCreateNoThrow("/drain_" + std::to_string(i), big_val, zkutil::CreateMode::Persistent));

        dispatcher->signalShutdown();
        dispatcher->beginTCPConnectionDrain();
        dispatcher->shutdownBeforeConnectionsFinish();
        tcp_server.stop();
        dispatcher->shutdownAfterConnectionsFinish(/*closed_all_connections=*/true);
        zk_drain.reset();
        for (auto & f : drain_futs)
        {
            try
            {
                (void)f.get();
            }
            catch (...)
            {
            }
        }
    };

    run_dispatcher_overload(/*use_new_dispatcher=*/true, /*test_http_4lw_acl=*/true);
    run_dispatcher_overload(/*use_new_dispatcher=*/false, /*test_http_4lw_acl=*/false);

    if (!recorded_bugs.empty())
    {
        std::string msg;
        for (size_t i = 0; i < recorded_bugs.size(); ++i)
        {
            if (i > 0)
                msg += " | ";
            msg += recorded_bugs[i];
        }
        throw std::runtime_error(msg);
    }

    std::cout << "  [PASS] live: dispatcher_overload_queue_limits_and_http_4lw_acl\n";
}

}

inline const std::vector<std::string> & getKeeperLiveServerSuiteNames()
{
    static const std::vector<std::string> suites = {
        "dispatcher_tcp_http_4lw",
        "dispatcher_old_lifecycle",
        "tcp_password_auth_and_tls",
        "3node_forwarding_rcfg_failover_recovery",
        "tcp_session_resumption_and_watch",
        "client_helpers_lock_cache_multi",
        "3node_lsmt_disk_learner_hot_reload",
        "tcp_chroot_and_restart",
        "3node_chunked_snap_and_failover",
        "path_acls_snap_exit_log_az",
        "3node_rejoin_and_atomic_leader_replace",
        "2node_quorum_sync_read_during_close",
        "3node_readahead_commit_cache_and_bg_snap",
        "dispatcher_overload_queue_limits_and_http_4lw_acl",
#if USE_SSL
        "3node_tls_raft_peers",
#endif
#if USE_AWS_S3
        "s3_snapshot_uploader",
#endif
    };
    return suites;
}

inline void runKeeperLiveServerTests(
    const std::filesystem::path & state_dir,
    const std::string & suite_filter = "",
    uint16_t port_base = 0)
{
    require(!state_dir.empty(), "--state-dir is required for --live-server-tests");
    if (port_base != 0)
        live_server_detail::g_live_port_cursor.store(port_base, std::memory_order_relaxed);
    if (std::filesystem::exists(state_dir))
        std::filesystem::remove_all(state_dir);
    std::filesystem::create_directories(state_dir);

    /// Live server tests exercise real multi-threaded KeeperDispatcher, NuRaft ASIO, TCP, and HTTP,
    /// so virtual POSIX time must stay disabled here.
    g_virtual_time_enabled = false;
    DB::ServerUUID::setRandomForUnitTests();
    {
        std::istringstream empty_iss("<clickhouse/>");
        Poco::AutoPtr<Poco::Util::XMLConfiguration> empty_cfg(new Poco::Util::XMLConfiguration(empty_iss));
        DB::PlacementInfo::PlacementInfo::instance().initialize(*empty_cfg);
    }
    std::cout << std::unitbuf;

    bool matched = false;
    auto maybe_run = [&](const std::string & name, auto && fn)
    {
        if (!suite_filter.empty() && suite_filter != name)
            return;
        matched = true;
        fn(state_dir);
    };

    maybe_run("dispatcher_tcp_http_4lw", live_server_detail::testLiveDispatcherTcpHttpAnd4LW);
    maybe_run("dispatcher_old_lifecycle", live_server_detail::testLiveDispatcherOldLifecycle);
    maybe_run("tcp_password_auth_and_tls", live_server_detail::testLiveTCPPasswordAuthAndTLS);
    maybe_run("3node_forwarding_rcfg_failover_recovery", live_server_detail::testLive3NodeClusterForwardingFailoverAndForceRecovery);
    maybe_run("tcp_session_resumption_and_watch", live_server_detail::testLiveTCPSessionResumptionInvalidSessionAndCrossClientWatch);
    maybe_run("client_helpers_lock_cache_multi", live_server_detail::testLiveClientHelpersLockNodeCacheCachingGetterAndMulti);
    maybe_run("3node_lsmt_disk_learner_hot_reload", live_server_detail::testLive3NodeLSMTDiskLearnerJoinAndHotReload);
    maybe_run("tcp_chroot_and_restart", live_server_detail::testLiveTCPChrootIsolationAndRestartPersistence);
    maybe_run("3node_chunked_snap_and_failover", live_server_detail::testLive3NodeChunkedSnapshotInstallAndMultiHostClientFailover);
    maybe_run("path_acls_snap_exit_log_az", live_server_detail::testLivePathACLsSnapshotOnExitDynamicLogLevelAndAZPreference);
    maybe_run("3node_rejoin_and_atomic_leader_replace", live_server_detail::testLive3NodeRejoinWithoutRestartAndAtomicLeaderReplace);
    maybe_run("2node_quorum_sync_read_during_close", live_server_detail::testLive2NodeClusterQuorumLossRecoveryAndReadDuringClose);
    maybe_run("3node_readahead_commit_cache_and_bg_snap", live_server_detail::testLive3NodeReadAheadCommitCacheAndBgSnapIO);
    maybe_run("dispatcher_overload_queue_limits_and_http_4lw_acl", live_server_detail::testLiveDispatcherOverloadQueueLimitsAndHttp4LWAllowList);
#if USE_SSL
    maybe_run("3node_tls_raft_peers", live_server_detail::testLive3NodeTLSRaftCluster);
#endif
#if USE_AWS_S3
    maybe_run("s3_snapshot_uploader", live_server_detail::testLiveS3SnapshotUploader);
#endif

    if (!suite_filter.empty() && !matched)
        throw std::runtime_error("Unknown --live-suite: " + suite_filter);

    std::error_code ec;
    std::filesystem::remove_all(state_dir, ec);
    std::cout << "PASS keeper-sim-live-server-tests\n";
}

}
