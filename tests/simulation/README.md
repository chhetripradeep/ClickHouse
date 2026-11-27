# FoundationDB-Inspired Deterministic Simulation Framework for ClickHouse Keeper

`tests/simulation/keeper_sim/` implements a single-threaded C++20 deterministic simulation testing (DST) framework for real ClickHouse Keeper (`KeeperStateMachine`, `KeeperStorage` Mem & LSMT, `KeeperLogStore`, `Changelog`, `KeeperStateManager`, `KeeperSnapshotManager`, `SessionExpiryQueue`, `KeeperReconfiguration`, `KeeperFeatureFlags`, `FourLetterCommand`) + pinned `NuRaft`, modeled on FoundationDB's `Sim2`, `AsyncFileNonDurable`, `BUGGIFY`, `SwizzleClogger`, `SOMETIMES`, and Joshua swarm testing.

## Quick Start: PR CI Suite (~20-45s wall-clock)

Build `keeper_sim` once:

```shell
cmake -S . -B build_keeper_sim -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_LIBRARIES=0 -DENABLE_NURAFT=1 -DENABLE_TESTS=1 \
    -DENABLE_BENCHMARKS=0 -DENABLE_UTILS=0 -DENABLE_EXAMPLES=0 \
    -DENABLE_RUST=0 -DENABLE_THINLTO=0 \
    -DENABLE_CLICKHOUSE_SELF_EXTRACTING=0 \
    -DENABLE_KEEPER_SIMULATION=1 \
    > build_keeper_sim/configure.log 2>&1
ninja -C build_keeper_sim keeper_sim > build_keeper_sim/build_keeper_sim.log 2>&1
```

Run the full PR-CI suite in one command (auto-detects CPU cores, runs controls, 23 Keeper functional feature tests, 13 parallel live server suites covering 16 checks, 23 parallel distributed chaos/storage/consensus scenarios with streaming, bounded cache, and strict watch oracle enabled, `--host-delay` determinism replay, mandatory `59/59` `SOMETIMES` feature-coverage gate, and writes `junit.xml` without short-circuiting so upstream bug fixes automatically flip failing cases to green):

```shell
./build_keeper_sim/src/keeper_sim --ci --output build_keeper_sim/ci_out
```

Or run via the Praktika CI job script (`ci/jobs/keeper_simulation_job.py`):

```shell
python3 ./ci/jobs/keeper_simulation_job.py --binary ./build_keeper_sim/src/keeper_sim
```

Options for CI customization:
- `--workers N`: explicit parallelism (`0` = auto-detect `std::thread::hardware_concurrency()`, clamped to `[2, 16]`).
- `--seed-offset N`: deterministically shift all CI seeds (e.g., per PR commit SHA or nightly date) while keeping every failure 100% reproducible.
- `--junit-xml <path>`: write standard JUnit XML report for CI reporting.

## Complete Keeper Functionality Coverage

Every `--ci` run verifies ALL of the following Keeper subsystems end-to-end:

1. **Deterministic Foundation Controls**:
   - `--instrumentation-control`: verifies `__wrap_clock_gettime` virtual clock interception and `__wrap_pthread_create` zero-thread isolation.
   - `--unsafe-quorum-control`: negative safety control proving `RaftSafetyOracle` immediately catches split-brain commits when `custom_commit_quorum_size_ = 1`.
   - `--host-delay` determinism replay: proves bit-identical `trace_hash`, `executed_events`, `final_commit_index`, and `final_digest` across runs with injected host timing jitter.
2. **Functional Subsystem Self-Tests (`--feature-tests`, 23 tests, ~1.3s)**:
   - Binary wire-format serialize/deserialize round-trip across all 34 `Coordination::OpNum` types with both 32-bit and 64-bit XIDs and `OpenTelemetry::TracingContext`.
   - `KeeperFeatureFlags` (`fromApiVersion`, `enableFeatureFlag`, `disableFeatureFlag`) and `KeeperContext::isOperationSupported`.
   - Full ACL policy matrix on both `Mem` and `LSMT-Mem`: `check_node_acl_on_remove = true`, `superdigest` bypass, `cleanup_old_and_ignore_new_acl` (`block_acl`), `auth` fixup, and `/keeper` system-node read/write-protection invariants.
   - Exact virtual-time TTL GC (`mtime` refresh on `Set`, ACL bypass for internal GC session `-2`) and Container GC (non-empty preservation, emptied-container GC via session `-3`, and never-used container GC via `collectContainerCandidates`).
   - Orphaned-node startup recovery (`remove_orphaned_nodes_on_startup`, `findOrphanConflictInLogTail` rejecting conflicting log tails and persisting repaired snapshots on clean tails).
   - `RaftServerConfig` parsing, `joiningToClusterUpdates`, `leavingToClusterUpdates`, and `serializeClusterConfig`.
   - 4LW command codes (`IFourLetterCommand::toCode`/`toName`), `KeeperConnectionStats`, and `KeeperStateMachine::getStorageStatsAndAsynchronousMetrics`.
   - Multi-file `KeeperLogStore` / `Changelog` parallel startup read (`startup_read_max_streams = 4`), bounded `latest_logs_cache`, per-peer decoded read-ahead (`log_readahead_enabled = true`), commit decoded read-ahead (`log_readahead_commit_window_bytes > 0`), Zstd-compressed log rotation/compaction/`write_at` cross-file truncation.
   - `SessionExpiryQueue` bucket rounding / heartbeat extension / expiration, `KeeperStateManager` XML config parsing + `save_state`/`read_state` + `getRaftConfigurationDiff`, and `DB::buildRaftParams`.
   - Apache ZooKeeper snapshot + transaction log migration (`ZooKeeperDataReader`).
   - `KeeperReadThreadPool` (inline fallback, multi-chunk parallel reads, exception propagation) and `KeeperOverDispatcher::promotingMultiCallback`.
   - `KeeperClientCLI` (`KeeperClientBase` parser + full command suite + `formatKeeperNodeName`).
   - LSMT write-throttling (`StorageState::recalculateWriteThrottling`), `setKeeperFileDescriptorMetrics` sentinel handling, and `KeeperStateManager` SHA-256 `client_password_sha256_hex` parsing.
   - `CompactChildrenSet` differential verification across Empty/Single/Set/300-child fanout/demotion/copy/move.
   - Persistent & persistent-recursive watch semantics (`AddWatch`, `CheckWatch`, `RemoveWatch`, `SetWatch`, `SetWatch2`) on both `Mem` and `LSMT-Mem`.
   - Extended ops (`Create2`, `CheckNotExists`, `CheckStat`, `ListRecursive` limit, `RemoveRecursive` limit enforcement, `FilteredListWithStatsAndData`).
   - Cross-version snapshot compatibility matrix (`V5`–`V9` Zstd & uncompressed loaded into `Mem` & `LSMT-Mem`), fail-closed `CORRUPTED_DATA` rejection on corrupted latest snapshot, and recovery to retained `snapshots_to_keep = 3` predecessor.
   - On-disk `KeeperLSMTNodesStorage` flush, merge, huge 6 KB values, tombstones, snapshot, and reopen durability.
   - `InMemoryLogStore`, `SummingStateMachine`, LSMT `BlockCache` (SLRU probation/group-load/eviction), lock-free MVCC `SnapshotableHashTable` overlapping `ReadView`s + CoW + stale-node reclamation, and `ZooKeeperPathUtils`.
   - Exhaustive `ListWithOptions` (`ALL`/`PERSISTENT_ONLY`/`EPHEMERAL_ONLY`, `with_stat`, `with_data`, `recursive`, `max_results`, `truncated`, `shuffle`) and `Multi` atomic commit + rollback digest-preservation matrix on both `Mem` and `LSMT-Mem`.
   - `WriteBufferFromNuraftBuffer`/`ReadBufferFromNuraftBuffer`, LSMT `NodeRefCache` weak-pointer expiration & `combineActions` strict/loose transitions, `ZooKeeperRetriesControl`, `ShuffleHost::compare`, `ErrorCounter` + `SystemTablesDataTypes`, and `LoggerWrapper`.
   - Empty `Multi`/`MultiRead` (0 subrequests), `<feature_flags>` XML configuration parsing & validation (including `MULTI_READ` downgrade and `remove_orphaned_nodes_on_startup` + LSMT rejection), and Znode `ctime`/`mtime` persistence across snapshot reopen.
   - `zk_approximate_data_size` accounting accuracy across `Create`/`Set`/`Remove` on both `Mem` and `LSMT-Mem`, plus `KeeperLogStore` missing middle-file log-gap rejection on startup.
3. **Live Server, Multi-Node Cluster, Security, TCP, HTTP, Client Helpers & 4LW Integration Tests (`--live-server-tests`, 16 suites run in parallel isolated processes, ~9s wall-clock)**:
   - Real standalone `KeeperDispatcher` (`use_new_dispatcher = true`, `optimize_read_order = true`, `parallel_read_threads = 2`), real `KeeperServer`, `KeeperRequestDispatcher`, `KeeperStateManager`, `OpNum::Reconfig` end-to-end, `createSnapshot`, `getKeeperLogInfo`, `getClusterMembersInfo`, `getChangelogsStatus`, `updateConfiguration`, `KeeperAsynchronousMetrics` (`updateKeeperInformation`), and `max_memory_usage_soft_limit` (`ZOUTOFMEMORY` write rejection + read allowance).
   - 4LW allow-list enforcement (`ruok,isro` blocking `csnp`), then all 28+ 4LW commands via `FourLetterCommandFactory` (`ruok`, `mntr`, `srvr`, `stat`, `conf`, `cons`, `wchs`, `wchc`, `wchp`, `dump`, `envi`, `dirs`, `isro`, `apiv`, `csnp`, `lgif`, `rclc`, `clrs`, `ftfl`, `bpon`, `bpof`, `pfev`, `lgrq`, `srst`, `crst`, `rqld`, `ydld`, `rcfg`, plus `jmst` when `USE_JEMALLOC=1`) + `KeeperOverDispatcher`.
   - Real `KeeperTCPHandler` over ephemeral loopback TCP: raw TCP 4LW, plain `zkutil::ZooKeeper` client, LZ4-compressed + 64-bit XID + OpenTelemetry tracing client (`ZOOKEEPER_PROTOCOL_VERSION_WITH_TRACING`), `last_zxid_seen` `SESSION_REFUSED` rejection, `max_request_size` enforcement, raw TCP session timeout clamping (`min_session_timeout_ms`), explicit `OpNum::Close` ephemeral cleanup, `SessionExpired` (`timeout=0, sid=0`) rejection on `previous_session_id != 0`, 45-byte read-only handshake, and cross-client asynchronous data/child watch push.
   - Real HTTP control API over ephemeral loopback HTTP: `KeeperHTTPReadinessHandler` (`/ready`), `KeeperDashboardWebUIRequestHandler` (`/dashboard`), `KeeperDashboardContentRequestHandler` (`/dashboard/content`), `KeeperJemalloc*` (`/jemalloc`, `/jemalloc/`, `/jemalloc/status`), `KeeperNotFoundHandler` (404 path hints), `KeeperHTTPCommandsHandler` (`/api/v1/commands` 4LW & CLI), and `KeeperHTTPStorageHandler` (`/api/v1/storage` `POST`/`GET`/`PUT`/`GET?children=true`/`DELETE`).
   - Legacy `KeeperRequestDispatcherOld` (`use_new_dispatcher = false`) full lifecycle and shutdown.
   - TCP `client_password` authentication (wrong-password rejection & valid-password acceptance) + real TLS 1.3 `SecureServerSocket` (`tcp_port_secure` & `secure://127.0.0.1:<port>` client with ephemeral OpenSSL-generated X.509 certificate).
   - Real `ZooKeeper` client library helpers against live `KeeperDispatcher`: `ZooKeeperLock` mutual exclusion, `ZooKeeperNodeCache` live watch invalidation, `ZooKeeperCachingGetter`, `multi` atomic rollback, and `listWithOptions`.
   - Real 3-node loopback `KeeperDispatcher` cluster: quorum election, peer-health introspection, follower->leader `client_req_stream` forwarding & local read-after-write, live 4th-node voter dynamic join (`add_members`) + catch-up + removal (`remove_members`), live `rcfg` `set_priority` + `transfer_leadership`, leader shutdown automatic failover on 2/3 majority, and `forceRecovery` (`KeeperServer::enterRecoveryMode`) recovering committed data on a single surviving node.
   - Real 3-node on-disk LSMT (`use_lsmt_storage = true`, `storage_memory_only = false`) loopback cluster with huge 6 KB values, live 4th-node non-voting `learner: true` dynamic join + catch-up + removal, and `HOT_RELOAD` `updateConfiguration` (`quorum_reads = true`, `max_requests_batch_size`, `parallel_read_threads`).
   - Real TCP `chroot` (`root_path`) isolation (mapped writes, prefix-stripped watches/listings) + graceful `KeeperDispatcher` shutdown & restart persistence across snapshot + post-snapshot changelog delta + sequential counter continuity.
   - Real 3-node cluster forcing live multi-chunk `InstallSnapshot` over NuRaft ASIO TCP (`reserved_log_items = 0`, `snapshot_transfer_chunk_size = 512`) to a freshly joined 4th node + multi-host `zkutil::ZooKeeper` client automatic failover via `ZooKeeperRetriesControl` after killing the leader's TCP/Raft node.
   - Live client-side `ZooKeeperArgs::path_acls` (`apply_to_children = true`), AZ-local host preference (`prefer_local_availability_zone = true`), dynamic `<raft_logs_level>` hot reload via `updateConfiguration`, and automatic `<create_snapshot_on_exit>true</create_snapshot_on_exit>` on clean shutdown.
   - Live 3-node cluster rejoining a removed follower without restarting it + atomic single-command `OpNum::Reconfig` leader replacement (`joining` Node 4 while `leaving` old leader).
   - Live 2-node cluster quorum election, TCP `ZooKeeper::sync`, concurrent in-flight reads during session close (`read_during_close`), quorum loss on peer shutdown, and automatic quorum restoration on peer restart.
   - Real 3-node TLS-encrypted Peer-to-Peer Raft cluster (`raft_configuration.secure = true`, when `USE_SSL=1`) verifying quorum election, follower->leader replication over TLS Raft RPCs, and live TLS certificate hot reload (`CertificateReloader::tryLoad`/`tryLoadClient`), plus `KeeperSnapshotManagerS3` mock-S3 upload + `.LOCK` coordination (when `USE_AWS_S3=1`).
4. **23 Parallel Distributed Cluster Scenarios (3-node & 5-node, all fault & storage modes enabled without bug-avoidance skips)**:
   - **All Storage Engines**: `Mem`, `LSMT-Mem`, `LSMT-Disk` (with tiny memtables, block cache eviction, sorted-run merges, huge 2–10 KB values, real `DiskLocal` streaming snapshots, and data-disk I/O faults), and **5-node Mixed heterogeneous clusters** (Mem + LSMT-Mem + LSMT-Disk in one Raft quorum as a continuous differential oracle!).
   - **All ZooKeeper & Keeper Ops**: `Create`, `Create2`, `CreateIfNotExists`, `CreateContainer`, `CreateTTL`, `Set`, `Remove`, `TryRemove`, `RemoveRecursive`, `Check`, `CheckNotExists`, `CheckStat`, `Multi` (commit & rollback), `MultiRead`, `Get`, `Exists`, `List`, `SimpleList`, `FilteredList`, `FilteredListWithStatsAndData`, `ListRecursive`, `ListWithOptions`, `Auth`, `GetACL`, `SetACL`, `Sync`, `Heartbeat`, `SessionID`, explicit `Close`, session expiry, `AddWatch`/`CheckWatch`/`RemoveWatch`/`SetWatch`/`SetWatch2`, `/keeper` system nodes, and live 4LW metrics (`getStorageStats`, `dumpWatches*`, `dumpSessionsAndEphemerals`).
   - **All Fault & Consensus Profiles**: `balanced`, `chaos`, `power-loss`, `partition-storm`, `snapshot-storm`, NuRaft streaming mode + bounded log caches + strict `--watch-oracle` across all scenarios, `quorum_reads` + `use_xid_64` + tracing, `remote_snapshots` + `encrypted_logs` + dual-disk migration, `dynamic_membership`, `backpressure` + `full_consensus` + `leadership_expiry`, `lazy_startup` + `mixed_versions` + `snapshot_on_exit`, `auto_forward` + `client_req_stream` + `hot_reconfig`, `local_disk_cache` + `huge_lsmt_values`, `snapshot_versions_mix` (`V5`–`V9`) + `same_path_multi_churn`, `priority_torture` + `member_rotation` + `async_replication`, `torn_snapshots` + `broken_tail_writeat` + `compress_logs`, `snapshot_vote_safety` (`#134119`), `reconfig_race` + `reconfig_overlap`, `batch_cache_retry` + `batch_cache_snapshot` + `remote_snap_faults` + `log_size_cache`, and `late_session_race` + `rollback_torture` + `snapshot_torture`.
   - **Mandatory `SOMETIMES` Coverage Gate (`59/59` features)**: fails CI if any required Keeper feature counter drops to `0`.

## Architecture

- `keeper_sim/SimClock.h`: Nanosecond virtual clock (`simTimeMs` / `simWallTimeMs`), per-node clock skew, deterministic `SimRandom` + `thread_local_rng` seeding, linker wrappers (`__wrap_pthread_create`, `__wrap_clock_gettime`), per-seed `BuggifyConfig`, and `CoverageTracker`.
- `keeper_sim/SimEngine.h`: `Sim2`-style discrete-event virtual-time min-heap scheduler ordered by `(due_ns, seq)`, per-node incarnation filtering, `--host-delay` perturbation, FNV-1a trace hashing, and isolated `SimNodeSystemExit` recovery handling.
- `keeper_sim/SimCrashDisk.h`: `AsyncFileNonDurable`-inspired crash-consistent simulated disk (`SimCrashDisk` subclassing `DB::DiskLocal`) modeling volatile page cache vs. durable synced storage, torn writes, disk wipes, and transient I/O faults.
- `keeper_sim/SimNetwork.h`: `Sim2` simulated network, `SimTimers`, and `nuraft::global_mgr` driving NuRaft `appendStep` and `commitStep` single-threaded.
- `keeper_sim/SimOracle.h`: Always-on inline `RaftSafetyOracle` + independent `KeeperSemanticOracle` verifying responses, `Multi` subresponses, cross-replica committed `KeeperDigest`, deep trees, ACL policies (`check_node_acl_on_remove`, `superdigest`, `block_acl`), ephemerals, watches, TTL/Container GC, and linearizability.
- `keeper_sim/SimCluster.h`: Manages an N-node (3 or 5) real Keeper cluster with manual `Changelog`, `CreateSnapshotTask`, and LSMT `BackgroundWork` scheduling, plus live 4LW metrics verification.
- `keeper_sim/SimWorkloads.h`: Multi-session concurrent `ZooKeeperWorkload` + continuous `ChaosActor`.
- `keeper_sim/SimFeatureTests.h`: Fast functional verifier (23 tests) covering wire protocol, feature flags & XML config, ACL/system-node policies, TTL/Container GC, orphan recovery, reconfiguration parsing, 4LW stats, parallel startup + changelog read-ahead + log-gap rejection, `SessionExpiryQueue`, `KeeperStateManager`, `buildRaftParams`, `ZooKeeperDataReader`, `KeeperReadThreadPool`, `KeeperOverDispatcher`, `KeeperClientCLI`, LSMT write-throttling & `BlockCache`/`NodeRefCache`/`combineActions`, `CompactChildrenSet`, MVCC `SnapshotableHashTable`, `InMemoryLogStore`/`SummingStateMachine`, `ListWithOptions`/`Multi` rollback, `ZooKeeperRetriesControl`/`ShuffleHost`/`ErrorCounter`, empty `Multi`/`MultiRead`, and `zk_approximate_data_size`.
- `keeper_sim/SimLiveServerTests.h`: Live single-node, 2-node, and 3-node/4-node Keeper integration verifier (16 suites) covering `KeeperDispatcher`, `KeeperRequestDispatcher` & `KeeperRequestDispatcherOld`, `KeeperServer`, `OpNum::Reconfig`, all 28+ 4LW commands + `rcfg`, `KeeperTCPHandler` (plain, LZ4+XID64+Tracing, TLS 1.3, password auth, `chroot`, session timeout clamping, `SessionExpired`, cross-client watch push, `sync`, `read_during_close`), `KeeperHTTP*`, `ZooKeeperLock`/`ZooKeeperNodeCache`/`ZooKeeperCachingGetter`/`path_acls`/AZ preference, `create_snapshot_on_exit`, dynamic log level & settings hot reload, LSMT-disk 3-node + learner join, forced chunked `InstallSnapshot` + multi-host failover, rejoin without restart, atomic single-command leader replace, `forceRecovery`, 3-node TLS Raft + cert reload, and mock-S3 snapshot upload.
- `keeper_sim/SimSwarm.h` & `keeper_sim/main.cpp`: Single-seed CLI plus pure C++ POSIX multi-process parallel swarm runner (`--ci` / `--swarm --suite <ci|quick|full|hunt..hunt17>`).

## Single-Seed Debugging

Reproduce any failing seed deterministically (with optional `--keeper-log-level warning --nuraft-log-level 3 --trace-file tmp/trace.log`):

```shell
./build_keeper_sim/src/keeper_sim \
    --seed 106 --nodes 3 --profile chaos --lsmt-disk --deep-lsmt-stress \
    --no-streaming --no-bounded-cache \
    --duration-ms 13000 --state-dir tmp/repro_s106 --json-summary
```
