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

Run the full PR-CI suite in one command (auto-detects CPU cores, runs controls, full Keeper functional feature tests, 14 parallel distributed chaos/storage/consensus scenarios, `--host-delay` determinism replay, mandatory `SOMETIMES` feature-coverage gate, and writes `junit.xml`):

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
2. **Functional Subsystem Self-Tests (`--feature-tests`, ~1s)**:
   - Binary wire-format serialize/deserialize round-trip across all 34 `Coordination::OpNum` types with both 32-bit and 64-bit XIDs and `OpenTelemetry::TracingContext`.
   - `KeeperFeatureFlags` (`fromApiVersion`, `enableFeatureFlag`, `disableFeatureFlag`) and `KeeperContext::isOperationSupported`.
   - Full ACL policy matrix on both `Mem` and `LSMT-Mem`: `check_node_acl_on_remove = true`, `superdigest` bypass, `cleanup_old_and_ignore_new_acl` (`block_acl`), `auth` fixup, and `/keeper` system-node read/write-protection invariants.
   - Exact virtual-time TTL GC (`mtime` refresh on `Set`, ACL bypass for internal GC session `-2`) and Container GC (non-empty preservation, emptied-container GC via session `-3`).
   - Orphaned-node startup recovery (`remove_orphaned_nodes_on_startup`, `findOrphanConflictInLogTail` rejecting conflicting log tails and persisting repaired snapshots on clean tails).
   - `RaftServerConfig` parsing, `joiningToClusterUpdates`, `leavingToClusterUpdates`, and `serializeClusterConfig`.
   - 4LW command codes (`IFourLetterCommand::toCode`/`toName`), `KeeperConnectionStats`, and `KeeperStateMachine::getStorageStatsAndAsynchronousMetrics`.
   - Multi-file `KeeperLogStore` / `Changelog` parallel startup read (`startup_read_max_streams = 4`), bounded `latest_logs_cache`, per-peer decoded read-ahead (`log_readahead_enabled = true`), and commit decoded read-ahead (`log_readahead_commit_window_bytes > 0`) across file boundaries and backward rewinds.
3. **14 Parallel Distributed Cluster Scenarios (3-node & 5-node, ~15-35s total)**:
   - **All Storage Engines**: `Mem`, `LSMT-Mem`, `LSMT-Disk` (with tiny memtables, block cache eviction, sorted-run merges, and data-disk I/O faults), and **5-node Mixed heterogeneous clusters** (Mem + LSMT-Mem + LSMT-Disk in one Raft quorum as a continuous differential oracle!).
   - **All ZooKeeper & Keeper Ops**: `Create`, `Create2`, `CreateIfNotExists`, `CreateContainer`, `CreateTTL`, `Set`, `Remove`, `TryRemove`, `RemoveRecursive`, `Check`, `CheckNotExists`, `CheckStat`, `Multi` (commit & rollback), `MultiRead`, `Get`, `Exists`, `List`, `SimpleList`, `FilteredList`, `FilteredListWithStatsAndData`, `ListRecursive`, `ListWithOptions`, `Auth`, `GetACL`, `SetACL`, `Sync`, `Heartbeat`, `SessionID`, explicit `Close`, session expiry, `AddWatch`/`CheckWatch`/`RemoveWatch`/`SetWatch`/`SetWatch2`, `/keeper` system nodes, and live 4LW metrics (`getStorageStats`, `dumpWatches*`, `dumpSessionsAndEphemerals`).
   - **All Fault & Consensus Profiles**: `balanced`, `chaos`, `power-loss` (unsynced dirty-file revert + torn writes), `partition-storm` (asymmetric partitions + `SwizzleClogger`), `snapshot-storm` (Zstd/LZ4, chunked transfer, wipe-and-rejoin), `quorum_reads` + `use_xid_64` + tracing, `remote_snapshots` + `encrypted_logs` + dual-disk migration, `dynamic_membership` (`add_srv`/`remove_srv`/`use_new_joiner_type`), `backpressure` (`bpon`/`bpoff`) + `full_consensus` + `leadership_expiry`, `lazy_startup` + `mixed_versions` (V0/V1 batches, V8/V9 snapshots) + `snapshot_on_exit`.
   - **Mandatory `SOMETIMES` Coverage Gate**: fails CI if any required Keeper feature counter drops to `0`.

## Architecture

- `keeper_sim/SimClock.h`: Nanosecond virtual clock (`simTimeMs` / `simWallTimeMs`), per-node clock skew, deterministic `SimRandom` + `thread_local_rng` seeding, linker wrappers (`__wrap_pthread_create`, `__wrap_clock_gettime`), per-seed `BuggifyConfig`, and `CoverageTracker`.
- `keeper_sim/SimEngine.h`: `Sim2`-style discrete-event virtual-time min-heap scheduler ordered by `(due_ns, seq)`, per-node incarnation filtering, `--host-delay` perturbation, FNV-1a trace hashing, and isolated `SimNodeSystemExit` recovery handling.
- `keeper_sim/SimCrashDisk.h`: `AsyncFileNonDurable`-inspired crash-consistent simulated disk (`SimCrashDisk` subclassing `DB::DiskLocal`) modeling volatile page cache vs. durable synced storage, torn writes, disk wipes, and transient I/O faults.
- `keeper_sim/SimNetwork.h`: `Sim2` simulated network, `SimTimers`, and `nuraft::global_mgr` driving NuRaft `appendStep` and `commitStep` single-threaded.
- `keeper_sim/SimOracle.h`: Always-on inline `RaftSafetyOracle` + independent `KeeperSemanticOracle` verifying responses, `Multi` subresponses, cross-replica committed `KeeperDigest`, deep trees, ACL policies (`check_node_acl_on_remove`, `superdigest`, `block_acl`), ephemerals, watches, TTL/Container GC, and linearizability.
- `keeper_sim/SimCluster.h`: Manages an N-node (3 or 5) real Keeper cluster with manual `Changelog`, `CreateSnapshotTask`, and LSMT `BackgroundWork` scheduling, plus live 4LW metrics verification.
- `keeper_sim/SimWorkloads.h`: Multi-session concurrent `ZooKeeperWorkload` + continuous `ChaosActor`.
- `keeper_sim/SimFeatureTests.h`: Fast deterministic functional verifier covering wire protocol, feature flags, ACL/system-node policies, TTL/Container GC, orphan recovery, reconfiguration parsing, 4LW stats, and parallel startup + changelog read-ahead.
- `keeper_sim/SimSwarm.h` & `keeper_sim/main.cpp`: Single-seed CLI plus pure C++ POSIX multi-process parallel swarm runner (`--ci` / `--swarm --suite <ci|quick|full|hunt..hunt17>`).

## Single-Seed Debugging

Reproduce any failing seed deterministically (with optional `--keeper-log-level warning --nuraft-log-level 3 --trace-file tmp/trace.log`):

```shell
./build_keeper_sim/src/keeper_sim \
    --seed 106 --nodes 3 --profile chaos --lsmt-disk --deep-lsmt-stress \
    --no-streaming --no-bounded-cache \
    --duration-ms 13000 --state-dir tmp/repro_s106 --json-summary
```
