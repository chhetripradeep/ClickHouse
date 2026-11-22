# FoundationDB-Inspired Deterministic Simulation Framework for ClickHouse Keeper

`tests/simulation/fdb_sim/` implements a single-threaded C++20 deterministic simulation testing (DST) framework for real ClickHouse Keeper (`KeeperStateMachine`, `KeeperStorage` Mem & LSMT, `KeeperLogStore`, `Changelog`, `KeeperStateManager`, `KeeperSnapshotManager`, `SessionExpiryQueue`) + pinned `NuRaft`, modeled on FoundationDB's `Sim2`, `AsyncFileNonDurable`, `BUGGIFY`, `SwizzleClogger`, `SOMETIMES`, and Joshua swarm testing.

## Architecture

- `fdb_sim/SimClock.h`: Nanosecond virtual clock, optional per-node clock skew, deterministic `SimRandom` + `thread_local_rng` seeding, linker wrappers (`__wrap_pthread_create`, `__wrap_clock_gettime`) guaranteeing zero uncontrolled OS threads and zero host-clock leaks, per-seed `BuggifyConfig` (including `batched_log_entries`, `tiny_log_rotation`, `frequent_snapshots`, `small_append_batch`, `slow_disk_flush`, `slow_commits`, `network_reordering`, `aggressive_clogging`, `asymmetric_partitions`, `clock_skew`, `torn_writes_on_power_loss`, `transient_disk_io_errors`, `delayed_snapshot_publish`, `lsmt_tiny_memtables`), and `CoverageTracker` (`SOMETIMES` counters).
- `fdb_sim/SimEngine.h`: `Sim2`-style discrete-event virtual-time min-heap scheduler ordered by `(due_ns, seq)`, per-node incarnation filtering and event purging, diagnostic `--host-delay` perturbation, streaming FNV-1a trace hashing, and isolated single-node `SimNodeSystemExit` recovery handling.
- `fdb_sim/SimCrashDisk.h`: `AsyncFileNonDurable`-inspired crash-consistent simulated disk (`SimCrashDisk` subclassing `DB::DiskLocal`) modeling volatile page cache (`live_dir`) vs. durable synced storage (`durable_dir`). Sudden power loss (`simulatePowerLoss`) reverts unsynced files, injects torn partial writes, or corrupts unsynced tails while preserving all `sync`-completed files. Also supports `simulateGracefulStop`, `simulateDiskWipe` (forcing real network Raft snapshot installation), and transient I/O faults (open, partial write, sync error, remove error).
- `fdb_sim/SimNetwork.h`: `Sim2` simulated network, `SimTimers` (`nuraft::delayed_task_scheduler` wired onto `SimEngine`), and `nuraft::global_mgr` driving NuRaft `appendStep` and `commitStep` single-threaded. Supports 3-node and 5-node topologies, symmetric/asymmetric partitions, `SwizzleClogger` link delays, jitter/reordering, packet drops, reply-only drops after execution, deferred NuRaft async durability callbacks, and multi-chunk `install_snapshot_request` RPCs.
- `fdb_sim/SimOracle.h`: Always-on inline `RaftSafetyOracle` (election safety, persisted term monotonicity, one vote per term across crashes, log matching, state-machine safety, rollback safety, and exact current-term quorum certification for Figure 8 detection) + independent `KeeperSemanticOracle` (reference ZooKeeper tree, response & `Multi` sub-response verification, cross-replica committed `KeeperDigest` equality, deep tree checks at any replica's `last_commit_index`, sequential node numbering, ephemeral cleanup on session expiry, watch delivery, and client-ack linearizability).
- `fdb_sim/SimCluster.h`: Manages an N-node (3 or 5) real Keeper cluster in one process with `SimKeeperStateManager` (isolated single-node `system_exit`), `SimKeeperStateMachine` (rollback & `apply_snapshot` verification), `SimNuRaftLogger`, production `PreAppendLogLeader` / `PreAppendLogFollower` / `AppendLogFailed` callbacks (real `zxid` + digest verification), manual `Changelog` scheduling (`Write`, `Completion`, `FileOperation`), manual `CreateSnapshotTask` scheduling, and manual LSMT `BackgroundWork` scheduling (`Flush`, `Merge`, `FileDelete`) via `KeeperContext::setManualLSMTBackgroundWork`.
- `fdb_sim/SimWorkloads.h`: Multi-session concurrent `ZooKeeperWorkload` (`Create` persistent/ephemeral/sequential, versioned `Set`/`Remove`/`Check`, atomic `Multi` with commit & rollback patterns, local `Get`/`List`/`Exists` with real watches, deliberate virtual-time session expiry, and a monotonic `/sim/lin_reg` linearizability register) + continuous `ChaosActor` (`SwizzleClogger`, symmetric/asymmetric partitions, graceful/power-loss/disk-wipe crashes, transient snapshot disk faults, and `figure8-hunt` mode).
- `fdb_sim/SimSwarm.h` & `fdb_sim/main.cpp`: Single-seed CLI (`keeper_fdb_sim`) plus a pure C++ POSIX multi-process parallel swarm runner (`--swarm`) with instrumentation control, negative unsafe-quorum control, cross-run `--host-delay` determinism verification, and aggregated `SOMETIMES` coverage reporting.

## Runtime Seams (`src/Coordination/`)

Production defaults are unchanged; every simulation seam is opt-in:

- `SessionExpiryQueue`: optional instance-local millisecond `Clock` callback propagated through `KeeperContext` and `KeeperStorage` across storage and snapshot recreation.
- `Changelog` / `KeeperLogStore` / `KeeperStateManager`: `Changelog::ExecutionMode::Manual` starts no changelog threads and exposes single-step `hasPendingTask` / `runTask` (`Write`, `Completion`, `FileOperation`) plus `discardUnexecutedWritesForTesting` and custom `KeeperLogStore` injection.
- LSMT `BackgroundWork` / `StorageState`: `KeeperContext::setManualLSMTBackgroundWork` starts no flush/merge threads, skips sleep-based `throttleWrite`, and exposes single-step `hasPendingFlush` / `runFlushStep`, `hasPendingMerge` / `runMergeStep`, and `hasPendingFileDeletes` / `runFileDeleteStep` sharing the exact production flush/merge code paths.

## Build & Run

Configure with `-DENABLE_KEEPER_SIMULATION=1` (and `-DENABLE_TESTS=1 -DENABLE_NURAFT=1`) and build `keeper_fdb_sim`:

```shell
cmake -S . -B build_keeper_sim -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_LIBRARIES=0 -DENABLE_NURAFT=1 -DENABLE_TESTS=1 \
    -DENABLE_BENCHMARKS=0 -DENABLE_UTILS=0 -DENABLE_EXAMPLES=0 \
    -DENABLE_RUST=0 -DENABLE_THINLTO=0 \
    -DENABLE_CLICKHOUSE_SELF_EXTRACTING=0 \
    -DENABLE_KEEPER_SIMULATION=1 \
    > build_keeper_sim/configure.log 2>&1
cmake --build build_keeper_sim --target keeper_fdb_sim \
    > build_keeper_sim/build_fdb_sim.log 2>&1
```

Run the pure C++ multi-core swarm campaign (covers 3-node & 5-node Mem, PowerLoss, SnapshotStorm, Chaos, LSMT-Mem, LSMT-Disk, controls, `--host-delay` determinism verification, and `SOMETIMES` coverage):

```shell
./build_keeper_sim/src/keeper_fdb_sim \
    --swarm --suite full --workers 6 \
    --output build_keeper_sim/fdb_swarm_out \
    > build_keeper_sim/test_fdb_swarm.log 2>&1
```

Run a single reproducible seed (supports `--nodes 3|5`, `--profile balanced|chaos|power-loss|partition-storm|snapshot-storm|figure8-hunt`, `--lsmt`, `--lsmt-disk`, `--strict-raft-commit`, `--keeper-log-level warning`, `--nuraft-log-level 3`, `--trace-file <path>`, `--host-delay`, `--json-summary`):

```shell
./build_keeper_sim/src/keeper_fdb_sim \
    --seed 61 --nodes 3 --profile chaos --lsmt-disk \
    --duration-ms 15000 --state-dir build_keeper_sim/fdb_s61 --json-summary
```

## Production Bugs Discovered Organically

1. **ClickHouse Keeper fatal digest mismatch & crash loop on `apply_snapshot` with stale uncommitted log tail** (`src/Coordination/KeeperStateMachine.cpp`, `src/Coordination/Changelog.cpp`):
   - Repro: `./build_keeper_sim/src/keeper_fdb_sim --seed 11 --nodes 3 --profile chaos --duration-ms 15000 --state-dir tmp/repro_bug1 --keeper-log-level warning`
   - When a node with a diverged uncommitted tail `[S+1..M]` from an old term receives `InstallSnapshot(S)` from a new leader, `Changelog::compact` retains entries `> S` (since `S <= max_log_id`), and `KeeperStateMachine::apply_snapshot` runs `preprocessUncommittedLogEntries` over `[S+1..M]` on top of the new leader's snapshot tree, hitting `assertDigest` -> `std::terminate`.

2. **NuRaft permanent `InstallSnapshot` livelock in streaming mode (`max_log_gap_in_stream_ > 0`)** (`contrib/NuRaft/src/handle_snapshot_sync.cxx`, `contrib/NuRaft/src/handle_append_entries.cxx`):
   - Repro: `./build_keeper_sim/src/keeper_fdb_sim --seed 42 --nodes 5 --profile partition-storm --duration-ms 15000 --state-dir tmp/repro_bug2 --nuraft-log-level 3`
   - When a follower declines an outdated snapshot (`handle_install_snapshot_resp`), NuRaft updates `p->set_next_log_idx` and clears `snapshot_sync_ctx`, but never calls `p->reset_stream`. The stale `last_streamed_log_idx` remains below `log_store_->start_index()`, causing `request_append_entries` to bypass `p->make_busy` and pass the stale streamed index into `create_append_entries_req`, permanently looping on `InstallSnapshotRequest` / `declined snapshot` even after all faults heal.
