# Replicated insert simulation experiment

This directory contains three deliberately separate backends:

- Python real-process reference workload: three Keeper voters and two ClickHouse
  replicas, with explicit insert and recovery correctness rules.
- C++ bounded NuRaft-core probe in `nuraft/`: three manually driven Raft nodes,
  seeded event selection, saved-decision replay, modeled node recovery, test-only
  virtual clocks, and trace-repeatability checks. It also compiles and tests the
  actual Keeper `SessionExpiryQueue` with an injected clock.
- C++ real Keeper state-machine probe: the same manual driver, built through the
  root project, with three actual `KeeperStateMachine` instances, real requests,
  controlled snapshot-task execution, and fresh-context snapshot restoration.
  Its optional real-persistence mode also runs actual `KeeperLogStore`,
  `Changelog`, and `KeeperStateManager` persistence and recovery.

None is a complete deterministic ClickHouse simulator. The reference workload
does not control server execution. The standalone probe exercises actual NuRaft
code without Keeper storage; the root-build probe adds real Keeper storage and
state machines but not production `KeeperServer` dispatch, ClickHouse queries,
or `ReplicatedMergeTree`.

## Run the reference workload

Requirements: Linux, Python 3.11 or newer, an explicit ClickHouse executable supporting
`deduplicate_insert`, and the Python dependencies in `requirements.txt`.
Use a virtual environment; do not install dependencies into the system Python.

From the repository root:

```shell
python3 -m venv build_simulation/venv
build_simulation/venv/bin/python -m pip install -r tests/simulation/requirements.txt
build_simulation/venv/bin/python tests/simulation/reference.py \
    --binary /absolute/path/to/clickhouse \
    --scenario baseline \
    --output build_simulation/baseline > build_simulation/test_baseline.log 2>&1
build_simulation/venv/bin/python tests/simulation/reference.py \
    --binary /absolute/path/to/clickhouse \
    --scenario lost-response \
    --output build_simulation/lost-response > build_simulation/test_lost_response.log 2>&1
```

Each output directory must be new. All listeners bind to loopback with separately
allocated ports. Startup fails on a port collision; it does not silently change the
configuration. Each node has independent configuration and data directories. The
runner terminates only its own child processes and preserves their state and logs.
There are five server processes (three Keeper voters and two replicas) plus the
workload/checker process. Containers are not required for this reference backend.

The executable's SHA-256 and the checkout revision are recorded separately. An
external binary is **not** assumed to have been built from this checkout. For a
source-matched result, build this checkout and pass that executable explicitly.

Run the contract tests without a server or third-party Python dependencies:

```shell
mkdir -p build_simulation
python3 -m unittest discover -s tests/simulation -p 'test_*.py' \
    > build_simulation/test_contract.log 2>&1
```

## Contract and scenarios

The workload generates 32 batches of 16 rows. Each row has a run ID, batch ID,
row ID, and independently generated payload. The table is a plain
`ReplicatedMergeTree` with one constant partition and no views, TTLs, or mutations.
Normal background merging remains enabled. The ordering key is not a uniqueness
constraint.

`contract.py` owns explicit query/table settings and expected input generation.
The runner checks effective insert settings, records `SHOW CREATE TABLE`, and
counts nonempty native-protocol data blocks: exactly one 16-row block per attempt.
It does not claim arbitrary multi-block inserts are atomic.

Both scenarios insert the entire workload with quorum two, synchronous inserts,
and deduplication enabled. Table deduplication retention is 10,000 blocks / 86,400
seconds, far beyond this small experiment. Keep runs below that retention budget;
deduplication is not an unlimited exactly-once guarantee.

In `lost-response`, batch 8's native-protocol terminal acknowledgement is withheld
from the driver by a connection adapter, which disconnects and raises an explicit
lost-acknowledgement exception. The workload records `unknown`, then retries the
same input and token on the original replica and on the other replica. Each attempt
has a distinct query ID. No server cancellation or timing guess triggers the fault.

This models **application-boundary acknowledgement loss**, not an on-the-wire TCP
packet drop or an interruption during the server commit. A separate network proxy
and further scenarios are needed to exercise those failure windows. Fault-injector
knowledge is evidence, not a successful acknowledgement to the workload.

After inserts, the runner synchronizes both replicas and compares all rows and
multiplicities with the input. It then kills and restarts each replica sequentially
with preserved storage, checking both replicas after each restart. Readiness is
condition-based and bounded; replica synchronization has a 120-second server and
client timeout. These are wall-clock reference-test budgets, not simulated time.

The checker rejects:

- Missing acknowledged rows, including after a later uncertain retry.
- Duplicate row identities, unexpected rows, or corrupted payloads.
- Partial unknown-outcome batches under the restricted one-block contract.
- Any difference from all 512 expected rows after successful reconciliation.

Before reconciliation, an unknown batch can be absent or complete. Other server
or transport exceptions are recorded as unknown and abort the run; the runner does
not silently retry unexpected errors or reclassify them as definite rejection.
The current scenarios do not continue exploring after an unresolved error.

Process kills preserve filesystem/cache state. Even with `fsync_after_insert`,
`fsync_part_directory`, and Keeper `force_sync`, this is **not a power-loss test**.
Equal logical contents are required; identical physical parts are not.

## Evidence

Each run preserves:

- `manifest.json`: settings, input dimensions, binary hash, source revision, and
  an explicit blocked deterministic feasibility gate.
- `expected.json`: complete generated input, independent of database reads.
- `history.jsonl`: submissions, outcomes, block counts, fault evidence, effective
  settings, validation results, process crashes, and terminal verdict.
- Six logical snapshots: both replicas after reconciliation and after each restart.
- Per-node configuration, persistent state, and console logs.

These files diagnose a reference run. They are not deterministic replay artifacts:
OS scheduling, clocks, UUIDs, port allocation, storage, networking, and background
execution remain uncontrolled. Expected transient connection errors can appear
during startup, acknowledgement loss, and restart; the terminal verdict and
validation events determine whether the experiment passed.

## Deterministic feasibility gate: blocked

The source assessment is pinned to ClickHouse revision
`2a49505f2c31cc1c1b0be339fbf786fc823e6599` and its NuRaft gitlink
`884294e9dbee183b53601bbe2c8c5753cec1a3db`. The full-runtime assessment below
remains source-based; the isolated C++ core probe described afterward is executable.
Production execution seams include an instance-local session-expiry clock and
opt-in manual execution of changelog queues, described below. The existing
threaded changelog mode remains the default. These seams do not make the rest
of Keeper deterministic.

### Node isolation

- `ContextSharedPart` in `src/Interpreters/Context.cpp` counts constructions and
  terminates on the second independent shared context. Destroying the first does
  not reset the construction count.
- `Context::makeGlobalContext` installs process-global state.
- `DatabaseCatalog::init` in `src/Interpreters/DatabaseCatalog.cpp` rejects a second
  initialization; ordinary callers use the process-global `DatabaseCatalog::instance`.

Removing guards is not an isolation implementation. Before admitting two simulated
replicas, identical database/table names must resolve to independent node-owned
catalogs, settings, data, background work, and lifecycle. Context copies do not
provide that isolation because they share the same shared part.

### Blocking and scheduling

- `ReplicatedMergeTreeSink::waitForQuorum` synchronously reads Keeper, blocks on a
  `Poco::Event`, and reads a real `Stopwatch`. Its timeout reports an unknown insert
  status. Its watch-recheck and replica-version checks must be preserved.
- `BackgroundSchedulePool` creates worker and timer threads; runnable task-type
  selection uses `thread_local_rng`, and delayed tasks use `Poco::Timestamp`.
- `MergeTreeBackgroundExecutor` exposes task steps, but
  `ExecutableLambdaAdapter::executeStep` can execute an entire blocking job.
- Keeper clients, request dispatch, changelog, and other maintenance work have
  independent threads. Replacing one scheduler does not control this work.

### NuRaft seams and limits

`KeeperServer::launchRaftServer` supplies a NuRaft RPC factory and delayed-task
scheduler, but currently obtains both from a concrete Asio service. The pinned
NuRaft `context` also accepts a custom global manager. Without one, Raft starts
its own commit and append threads. Direct timers and optional snapshot execution
remain additional boundaries; `nuraft_test_mode` is not a no-thread mode.

The C++ spike implements a manually driven three-node **NuRaft core**, with
explicitly bounded coverage, before changing real Keeper wiring. It measures
thread creation, drives election/network/append/commit work separately, and checks
withheld quorum. It does not substitute for the replicated-table acceptance workload.

### Build and run the C++ probe

Requirements: Linux, GCC with static `libstdc++`, CMake 3.20 or newer, Ninja,
and Python 3.11 or newer. No Boost, Asio, ClickHouse executable, or third-party
Python packages are required. Initialize only the pinned NuRaft submodule:

```shell
git submodule update --init contrib/NuRaft
mkdir -p build_simulation/nuraft
cmake -S tests/simulation/nuraft -B build_simulation/nuraft -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug -DCMAKE_CXX_COMPILER=g++ \
    > build_simulation/nuraft/configure.log 2>&1
cmake --build build_simulation/nuraft > build_simulation/nuraft/build.log 2>&1
python3 tests/simulation/nuraft/verify.py \
    --binary build_simulation/nuraft/nuraft_manual_probe \
    --output build_simulation/nuraft/verification \
    > build_simulation/nuraft/test_verification.log 2>&1
```

The verification output directory must be new. Choose a different directory and
log filename for subsequent runs. The standalone build compiles the actual pinned
core sources, excluding Asio and its launcher. It uses `USE_CLICKHOUSE_THREADS=0`,
not ClickHouse's production thread-pool integration. There are no sockets, worker
threads, disk-emulation threads, or persistent storage in this backend.

The C++ process contains three real `raft_server` instances, one memory log and
state machine per node, and test implementations of NuRaft's RPC factory, delayed
scheduler, and `global_mgr`. A test-derived server exposes its protected append
and commit execution methods. No vendored NuRaft files are patched. The standalone
target also builds the actual `SessionExpiryQueue.cpp`; it does not replace that
component with a model.

### Five-voter Figure 8 regression and independent Raft checks

The same standalone build also produces `nuraft_figure8_probe`. It reuses the
manual driver, but runs five voters with unchanged membership and one-entry
append batches. The selected schedule commits a common prefix, leaves entry 2
on nodes 1/2 in term 1, elects node 3 in term 2 without replicating its conflicting
configuration, then re-elects node 1 in term 3. Nodes 4/5 acknowledge only the
old term-1 entry; the term-3 configuration at index 3 stays on node 1 alone.
Missing-prefix probes are delivered so followers can reject them and drive real
backtracking; requests that could store the withheld configuration are dropped.
No term, vote, log entry, or acknowledgement is fabricated by this execution.

```shell
python3 tests/simulation/nuraft/verify.py --backend figure8 --seeds 1 --repeats 3 \
    --binary build_simulation/nuraft/nuraft_figure8_probe \
    --output build_simulation/nuraft/verification-figure8 \
    > build_simulation/nuraft/test_figure8.log 2>&1
```

**This regression currently fails on the pinned dependency.** Leader 1 advances
its target commit index from 1 to 2 in term 3, without a current-term quorum
certificate. Both the C++ assertion and `raft_model.py` detect the violation.
The verifier exits nonzero; a reproduced violation is not an expected-pass test.
The final `figure8-setup-complete` marker distinguishes the exercised assertion
from an incomplete election or catch-up schedule. This demonstrates a
current-term commitment-rule violation in the in-memory NuRaft execution, not
production Keeper data loss or a later overwrite of an applied entry.

The verifier also runs `--current-term-control`, which allows the original leader
to replicate and apply entry 2 on all five nodes in its creating term. It records
both scenarios, replays the saved event choices, requires byte-identical complete
histories, and retains commands, source/binary SHA-256 hashes, and exit codes in
`manifest.json`. A setup failure or disagreement between the two oracles fails
verification separately. Source hashes describe verification time; retain build
commands/logs to associate a binary with its inputs.

The independent Python oracle consumes `RAFT` JSON observations of terms,
persisted votes, roles, complete log payloads, modeled durable indices,
commit/applied indices, applied values, and delivered response endpoints. It
checks monotonicity, one vote per term, election safety/quorums, log matching,
leader append-only behavior, leader completeness, committed-prefix preservation,
state-machine agreement, and term-sensitive durable quorum certificates. It
recognizes commitment before acknowledgement when a current-term entry is
already on a durable majority. It does not trust the C++ `PASS`/`FAIL` result.
Sixteen deliberately corrupted histories check rejection of these violations and
missing evidence; those histories are checker controls, not executed failures.

Version 1 is deliberately limited to this five-voter, single-incarnation,
full-log, synchronous in-memory durability format. It rejects missing protocol
observations and unsupported incarnations. It does not retrofit protocol evidence
onto older Keeper traces, verify physical durability, check arbitrary membership
changes/snapshots/restarts, explore all schedules, establish liveness, or validate
linearizable reads. Passing the Keeper semantic checker remains a separate claim.

### What the C++ checks establish

1. Startup does not elect a leader or apply entries before a selected timer action.
2. A selected election elects node 1, with no second leader.
3. Appending `731` is accepted and locally logged but remains incomplete while both
   follower links are withheld. No state machine applies it without a majority.
4. Releasing node 2's link completes and applies the entry on nodes 1 and 2 while
   node 3 remains untouched.
5. A second, distinct value `-47` completes with the same follower withheld.
6. Releasing node 3 catches it up. Every state machine contains exactly
   `[731, -47]`, in that order, without duplicates.
7. Shutdown discards pending network callbacks and cancels timers before node
   destruction; no queued action subsequently executes against those nodes.

With `--recovery`, the scenario additionally destroys follower 3's runtime, commits
`902` on the surviving majority, reconstructs the follower, and checks catchup.
It then destroys leader 1's runtime, expires both surviving voters' election timers,
elects node 2 in a higher term, commits `-613`, and reconstructs the old leader.
All three must contain exactly `[731, -47, 902, -613]` at that checkpoint; the old
leader must return as a follower. Weak references check that the previous
server/state-machine objects were actually destroyed, rather than merely reusing
a paused instance.

This is **modeled runtime stop/restart over in-memory log/state images**.
Shutdown cancels work normally. It is not an abrupt process failure, durable disk
implementation, or physical power-loss test. RPC attempts to an
offline or replaced runtime produce queued errors. Per-node incarnation numbers
prevent a request captured for an old runtime from being delivered to its replacement.

The recovery scenario enables NuRaft's `parallel_log_appending_` and streaming
with `max_log_gap_in_stream_ = 8`. Before `902`, it pauses both leader 1's and
follower 2's modeled durability indices; follower 3 is offline. Follower 2 appends
without blocking and defers its response using NuRaft's actual async result.
A selected follower `durable` event calls `notify_log_append_completion`, releasing
that response. One durable voter still must not complete the client append or apply
the value. Only a subsequent leader `durable` event permits this majority to commit.
The transport queues completed responses rather than delivering them recursively;
it breaks the response/promise ownership cycle and preserves non-OK result codes.
A synchronous `flush` while paused fails rather than bypassing the driver.
This exercises NuRaft's real durability accounting and deferred-response path,
**not actual filesystem durability**.

After failover, a second interrupted-write phase submits `809` on leader 2 while
follower 1's durability is paused and follower 3's link is withheld. The driver
requires an actual deferred response and an incomplete client result. It destroys
follower 1, cancels that transport operation with a queued error to the live leader,
and replaces the follower's log with a copy containing only its modeled durable
prefix. The restarted follower must initially lack the interrupted entry. Selected
heartbeats must retransmit it; all three replicas must then contain exactly
`[731, -47, 902, -613, 809]`. This models losing an uncompleted append, not torn
sectors, partial file writes, or filesystem ordering.

A final phase submits `-1201` with the same withheld follower and paused durability,
then destroys the requesting leader while the response remains pending. Completing
the surviving follower's durability must execute the cancelled-response discard
path, without calling back into the destroyed sender, acknowledging the append, or
applying the uncommitted value. The final shutdown also requires an empty deferred
transport registry. An unacknowledged log entry may remain in a retained log image;
the checker does not confuse that with committed state.

With `--lost-reply`, follower 2 executes the first replication request, but its
response is replaced with a queued transport exception. Follower 3 remains
withheld. The checker requires that follower 2 has appended the entry, no state
machine has applied it, and the client's result remains pending. Explicit heartbeat
delivery then retries replication; eventual contents must still have no duplicates.
This is a modeled replication-response failure, not a client acknowledgement loss
or a TCP-level packet fault. The verifier combines it with recovery and leader loss.

The driver selects eligible network and background-work events using a seeded
`std::mt19937`. Withheld links retain requests/responses; they are released rather
than silently dropped. Timers are explicitly chosen by the scenario. In virtual
mode, firing a timer advances logical time to at least its recorded due time;
other timers do not fire automatically. This is a bounded schedule, not yet a
general virtual-time event explorer with fairness or exhaustive timer coverage.

Virtual mode uses election bounds of 150–300 ms and a 50 ms heartbeat. It retains
normal leadership-expiry behavior. Snapshots are disabled, and physical disk
operations are excluded; modeled completions and append responses use NuRaft's
nonblocking result API. Real-clock mode
is a limited control: zero election bounds, a 1,000,000 ms heartbeat, disabled
leadership expiry, and manually delivered timers. It is not a production-timing
comparison. Test mode preserves these parameter choices; it is not what prevents
threads. The custom manager prevents local append/commit threads.

GNU linker wrappers intercept directly linked `std::chrono::steady_clock::now`,
`std::chrono::system_clock::now`, and `pthread_create` calls. Virtual clocks advance
only through explicit scenario actions, and the system-clock value also carries
the seed used by NuRaft's election-timeout RNG. The driver counts clock calls,
rejects host-clock delegation in virtual mode, rejects thread creation, and checks
`/proc/self/task` after selected events and at lifecycle boundaries. Static
`libstdc++` is necessary to expose its thread-creation calls to the linker wrapper.

Three controls prevent an empty or ineffective test from appearing successful:

- `--instrumentation-control` creates a real `std::thread` and checks detection;
  it also verifies both virtual clocks follow explicit logical-time increments.
- `--unsafe-quorum-control` deliberately sets a commit quorum of one. The ordinary
  safety checker must fail with `No-quorum append falsely completed`; the runner
  requires that specific failure and exit code, rather than accepting any error.
- `--unsafe-durability-control` disables NuRaft's parallel-log durability accounting
  while keeping the delayed-log scenario. The checker must fail with
  `Undurable leader falsely counted toward majority`.

The default verification executes 50 subprocess runs: all three controls above,
two Keeper expiry checks, one real-clock reference,
three seeds × five repeats × with/without host-delay perturbation, one saved
replay per seed, lost-reply recovery plus delayed replay per seed, and five invalid
replay controls. Exactly seven runs must fail with their specified diagnostics.
Each seed must reproduce its entire normalized trace exactly, and different seeds
must vary the actual executed event order, not merely the printed timer values.
The 1 ms host delays are diagnostic perturbations, never synchronization or a way
to obtain progress. Each subprocess has a 30-second host watchdog.

The manifest records binary, probe-source, and expiry-component hashes, checkout revisions at
verification time, commands, exit codes, per-seed trace hashes, and the explicitly
blocked full ClickHouse gate. Revision observations do not attest binary build
inputs. Logs retain both measurements and verdicts. Trace records include event
sequence, message type/endpoints, term, locally appended index, target commit
index, applied index, timer scheduling, and applied values. Trace files exclude
measurement/verdict lines only; they do not discard different event orders.

`--record-schedule FILE` saves a versioned scenario/seed header and every selected
event's stable ID, kind, and endpoints. `--replay-schedule FILE` interprets those
choices instead of choosing an eligible event randomly. It rejects an ineligible
choice, a truncated schedule, unused trailing choices, or a mismatched header.
The verifier exercises each rejection and compares replay traces with recording
traces under host-delay perturbation. Replay requires the matching initial scenario
and seed; it does not yet replay arbitrary external workloads or minimize failures.

These results do not establish a cross-platform replay guarantee. The wrappers
are a build-local feasibility technique, not a production
clock abstraction. They do not intercept every possible syscall, dynamically
loaded library, condition-variable timeout, or future source of entropy. Adding
snapshots, disk emulation, Keeper dispatch, or new operations requires another
clock/thread/blocking audit and new acceptance scenarios.

### Explicit Keeper session-expiry clock

`SessionExpiryQueue` accepts an immutable, instance-local millisecond clock callback.
Its default preserves the existing `system_clock` behavior. `KeeperContext` owns
the callback and `KeeperStorage` copies it into each new expiry queue. This includes
storage recreated during snapshot application. A simulation clock must outlive its
users and be safe from every caller; capturing shared clock state by value avoids
dangling references and must not create a context ownership cycle.

`--keeper-expiry` checks the real queue's exact rounding boundaries, repeated polls,
renewal within/across buckets, removal, clearing, time jumps, independent instances,
and the default clock. It compares runs with and without host delays and rejects
global clock reads while using the injected clock.

Real-storage tests in `gtest_coordination.cpp` check the callback through
two independent `KeeperContext` instances with the same session identity but
different clock values, and through storage recreation for both Mem and LSMT.
The existing `ApplySnapshotReplacesCommittedState` test also checks restored-session expiry
through the real `KeeperStateMachine` snapshot path. Restoring a session re-arms
its timeout at restoration time; this seam does not change that existing policy.
These full-library tests require `unit_tests_dbms`; their execution status is
separate from the standalone probe results.

Build that target through the root CMake project, not the standalone probe.
The reduced-library configuration still needs the core submodules from
`ci/jobs/fast_test.py`, plus Google Test and Google Benchmark for the test target.
The all-DBMS unit target also includes IO tests that require Snappy and libarchive;
Snappy's framing implementation additionally requires CRC32C. Omitting them caused
the first build attempt to fail before any Keeper tests ran. These dependencies are
selected by their checked-out targets, not `ENABLE_SNAPPY`/`ENABLE_LIBARCHIVE` flags.
The following configuration was generated successfully with installed Clang 22;
it is not a small Keeper-only build:

```shell
mkdir -p build_keeper_sim
git submodule update --init contrib/googletest contrib/google-benchmark \
    contrib/snappy contrib/libarchive contrib/crc32c \
    > build_keeper_sim/bootstrap_keeper_test_deps.log 2>&1
cmake -S . -B build_keeper_sim -G Ninja \
    -DCMAKE_C_COMPILER=/usr/bin/clang-22 \
    -DCMAKE_CXX_COMPILER=/usr/bin/clang++-22 \
    -DLINKER_NAME=/usr/bin/ld.lld-22 -DCMAKE_BUILD_TYPE=Release \
    -DENABLE_LIBRARIES=0 -DENABLE_NURAFT=1 -DENABLE_TESTS=1 \
    -DENABLE_BENCHMARKS=0 -DENABLE_UTILS=0 -DENABLE_EXAMPLES=0 \
    -DENABLE_RUST=0 -DENABLE_THINLTO=0 \
    -DENABLE_CLICKHOUSE_SELF_EXTRACTING=0 \
    > build_keeper_sim/configure_keeper.log 2>&1
cmake --build build_keeper_sim --target unit_tests_dbms \
    > build_keeper_sim/build_keeper_tests.log 2>&1
```

Run the test executable from an otherwise-empty disposable working directory:
Keeper fixtures create relative `snapshots` and `logs` directories. Use this filter
for both storage implementations:

```text
Storage/CoordinationTest.TestSessionExpiry*/*:Storage/CoordinationTest.TestDeterministicPreprocess/*:Storage/CoordinationTest.ApplySnapshotReplacesCommittedState/*
```

Redirect the executable's output to a unique log under the build directory.
Passing these tests establishes real Keeper storage/state-machine behavior for
these cases; it does not establish deterministic production Keeper scheduling.

### Real Keeper state-machine driver

With the root build configured as above, enable the opt-in target:

```shell
cmake -S . -B build_keeper_sim -DENABLE_KEEPER_SIMULATION=1 \
    > build_keeper_sim/configure_manual_keeper.log 2>&1
cmake --build build_keeper_sim --target keeper_manual_probe \
    > build_keeper_sim/build_manual_keeper.log 2>&1
python3 tests/simulation/nuraft/verify.py --backend keeper \
    --binary build_keeper_sim/src/keeper_manual_probe \
    --output build_keeper_sim/verification-manual-keeper \
    > build_keeper_sim/test_verification_manual_keeper.log 2>&1
```

Use new output directories and log names on subsequent runs. This target is off
by default and excluded from ordinary builds. Unlike the standalone GCC target,
it links production ClickHouse libraries and NuRaft's ClickHouse thread-pool
integration. It still replaces transport, timers, and Raft background dispatch
with the manual driver. The unmodified NuRaft example memory-log implementation
has source-local warning relaxation; the probe retains the root warning policy.

Each scenario uses independent Keeper contexts and real request serialization,
preprocessing, commit, and storage operations. It allocates a session, creates
`/experiment`, withholds quorum and modeled durability, then releases their
completions separately. A lost replication reply causes retransmission of a
version-zero update; every node must contain `updated--47` at version exactly 1.
It then creates an ephemeral node owned by that session.

The snapshot phase invokes real `KeeperStateMachine::create_snapshot` and consumes
its queued `CreateSnapshotTask` as a selected driver event. Publication must not
precede that event. The driver destroys follower 3's server, state machine, and
context, checking weak-reference expiration, then constructs a fresh context
with committed index zero. A fresh state machine restores the on-disk snapshot
at index 5, including persistent data, the session, and its ephemeral node.
Finally, exact injected-clock expiry boundaries and replicated session closure
must remove the ephemeral node everywhere while preserving persistent data.
The restored follower must apply the later close without reapplying old entries.

The root verifier runs 41 subprocesses: one instrumentation control, 30 repeated
scenarios, three saved-schedule replays, and seven required failures. It records
snapshot paths and SHA-256 hashes as well as traces. Root schedules use
`keeper-mem-schedule-v2`; they are not interchangeable with standalone schedules.
Each successful scenario checks all 15 Keeper responses for `ZOK`.

The root-only `clock_gettime` wrapper virtualizes `CLOCK_REALTIME`,
`CLOCK_MONOTONIC`, and `CLOCK_MONOTONIC_RAW`. The last is required by `Stopwatch`.
The instrumentation control verifies both chrono clocks and an actual `Stopwatch`
advance by 17 ms, detects a deliberate thread, and detects a deliberately
delegated CPU-time clock call. The scenario rejects any wrapped host-clock
delegation during its measured interval. Three delegated raw-clock reads during
global initialization, before virtual mode, are reported separately from the
zero probe-period calls. This remains partial POSIX clock interception, not
universal control of all time APIs or entropy sources.

Successful Mem runs report
`clock_control=PARTIAL_POSIX persistence=MODELED_LOG_REAL_SNAPSHOT`.
In this default mode the snapshot is a real local file, but the Raft log and
manager remain modeled in memory and are retained across the follower
replacement. Snapshot I/O runs synchronously within the selected action;
its internal filesystem operations
are not individually scheduled or fault-injected. This is not production log
recovery, network snapshot installation, a complete process restart, or a
power-loss test. The full deterministic gate remains blocked.

The `--lsmt` admission probe currently fails immediately after the first real
state-machine initialization: five thread-creation attempts and six live threads.
`storage_memory_only=true` does not prevent `StorageState::startup` from creating
`BackgroundWork` and its flush/merge workers. Do not disable this check or treat
ordinary passing LSMT component tests as a deterministic-runtime admission.
Cooperative scheduling of that real work remains a separate runtime change.

### Real Keeper log and metadata driver

Use the same root-build executable with the real-persistence verifier:

```shell
python3 tests/simulation/nuraft/verify.py --backend keeper-log \
    --binary build_keeper_sim/src/keeper_manual_probe \
    --output build_keeper_sim/verification-real-persistence \
    > build_keeper_sim/test_real_persistence.log 2>&1
```

Use a new output directory and log name for every invocation. The verifier passes
`--real-log` to the executable. Each node has a real local log disk, snapshot
disk, and metadata disk. Logs are uncompressed, rotate every four entries, and
use `force_sync=true`.

`Changelog::ExecutionMode::Manual` starts no changelog workers. The driver selects
real queued append/flush, completion, and file-maintenance work as `log-write`,
`log-complete`, and `log-file` events. This mode requires serial startup,
disabled read-ahead, single-batch flushing, and no fsync throttling; unsupported
settings are rejected. It uses the existing writer, queues, and file operations,
not simulated successful writes. Configuration-entry flushes and rewrite barriers
perform synchronous real writes/fsync inside their selected caller action;
NuRaft completion callbacks remain queued to avoid lock re-entry. Individual
filesystem calls inside these actions are not separate scheduling decisions.

The driver uses the production `KeeperStateManager` with an explicitly supplied
manual log store. Its `save_state` and `read_state` persist and recover actual
checksummed term/vote files. Cluster configuration is recovered from the latest
log or snapshot configuration, matching the selection policy in
`KeeperServer::loadLatestConfig`; it is not a separate configuration file.
Follower replacement destroys the old server, state machine, context, log
store, and manager, then checks exact serialized state/configuration and log
entries 1–5 after reconstruction. Startup flushes the actual recovered log
prefix before advertising durability.

After restart, selected real vote requests exercise both an initially uncast
vote and retry of an existing vote. An eligible request must succeed; another
candidate in that term and a stale-term request must be rejected. The selected
vote must be present in the real state file afterward.

Three optional fault scenarios extend the baseline:

- `--log-loss` pauses leader/follower writes before global commit, discards only
  follower 3's unexecuted append/flush work, reconstructs it from prefix 5, and
  requires retransmission and exact application of close at index 6. Already
  executed writes are not rolled back. Normal store destruction flushes pending
  work, so it is not a substitute for this explicit injection.
- `--metadata-write-failure` fails a real metadata rewrite after opening and
  truncating the live state file. The production backup must recover the exact
  previous durable term/vote into a fresh manager. This is a direct failed
  `save_state` action, not a production request-handler exception/restart test.
  The verifier also combines this fault with log loss.
- `--snapshot-write-failure` throws after opening the real snapshot data file,
  before payload serialization. The failed task must report failure exactly
  once, publish nothing, and remove both its empty data file and incomplete
  marker. A retry must publish a snapshot that restores correctly. The combined
  scenario runs this failure/retry before the metadata and log-loss phases;
  these are sequential faults, not overlapping interrupted filesystem calls.

The default real-persistence verifier runs 472 subprocesses: one instrumentation control,
30 baseline repeats, three baseline replays, six log-loss record/replays,
12 metadata-rewrite record/replays (alone and combined), 12 snapshot-failure
record/replays (alone and combined), 24 deeper-fault record/replays, 24 generated
workload record/replays, 24 atomic-multi record/replays, 24 pipelined-multi
record/replays, 48 outstanding-transaction-fault record/replays, 168 additional
selected-leader-fault-batch record/replays, 36 partial-survivor record/replays,
36 live partial-survivor record/replays, and 24 expected failures. Its 447 successful
scenario traces must repeat under saved decisions and host delay. Changelog,
metadata, and snapshot bytes must also match within each repeat/replay group;
only snapshot filenames, not their contents, are excluded from byte comparisons.
Manifests retain snapshot, changelog, metadata, and binary hashes, plus
verification-time source hashes; these source observations do not
attest historical binary build inputs.

`--seeds` selects one or more distinct unsigned 32-bit seeds; the default is
`1 17 991`. `--repeats` defaults to five and requires at least two. Replay-rejection
and checker controls use the first selected seed, so campaigns need not include
seed 1. For example, adding `--seeds 4294967295 0 --repeats 2` runs the complete
scenario matrix and controls in 311 subprocesses, including the same 24 required
failures. Output directories must remain unique.

Negative controls require specific diagnostics, not arbitrary failures. They
include unsafe quorum/durability, graceful teardown instead of dropped writes,
missing metadata, removal of the backup after the interrupted rewrite, and five
invalid replay schedules. A disabled snapshot fault and two additional
fault-scenario replay mismatches are also required failures. The deeper-fault mode
and generated-workload identities each have an additional replay rejection;
atomic-multi, pipelined-multi, outstanding-fault, leader-failover, leader-partition,
partial-survivor, and live partial-survivor identities are also checked separately.
Real-persistence schedules use `keeper-real-log-schedule-v4`; metadata-failure
schedules append `-metadata-write-failure-MODE` and snapshot-failure schedules append
`-snapshot-write-failure-MODE`, including `open` for the original fault.
Generated workloads append `-generated-v1`; atomic-multi workloads additionally
append `-multi-v1`, and pipelined workloads additionally append `-pipeline-v1`.
Outstanding faults additionally append `-fault-MODE-v2`.
Nonzero leader fault batch selections append `-batch-N`; replay with another batch
selection must be rejected. The default batch zero retains its original identity.
Other backend/version/scenario
schedules cannot be replayed interchangeably.

Successful runs report
`clock_control=PARTIAL_POSIX persistence=REAL_LOG_REAL_SNAPSHOT_REAL_METADATA`.
This still uses Mem storage, manual dispatch, and local filesystem calls. It
does not model OS power loss, torn changelog sectors, filesystem cache loss,
network snapshot installation, or production Keeper lifecycle. The full
ClickHouse deterministic gate remains blocked.

### Partial writes, sync errors, and snapshot publication

The real-persistence driver also accepts:

- `--metadata-fault partial`: the real file writer writes a 13-byte prefix before
  throwing. The live file must contain exactly those bytes and the backup must
  exist before reconstruction. The fresh manager must recover the exact old
  checksummed state, configuration, and voting behavior.
- `--snapshot-fault partial`: the real snapshot writer writes a 13-byte prefix
  before throwing. Production cleanup must remove the incomplete data and marker.
- `--snapshot-fault sync`: all buffered snapshot bytes reach the real file before
  an injected exception replaces the underlying sync call. The recorded file size
  must exceed 13 bytes. The task must fail without publishing and clean up both files.
- `--snapshot-fault marker`: after snapshot writing/sync, removing its incomplete
  marker throws once. The file must contain more than 13 bytes, no snapshot may
  publish, and production exception cleanup must remove data and marker.

Every snapshot fault requires successful retry and restoration. The verifier runs
each boundary separately and combines them with partial metadata rewrite, lost
unexecuted log work, and generated operations. Mode-specific evidence and byte
counts are retained in traces. The original `--metadata-write-failure` and
`--snapshot-write-failure` flags still select the open/truncate boundary.

These are controlled in-process I/O exceptions, not abrupt process termination,
sector tears, directory-sync failure, or a filesystem/power-loss model. Cleanup
executes normally. In particular, a sync error does not imply that bytes vanished:
the snapshot test observes the full file before production cleanup removes it.
Metadata sync-error recovery is not modeled; it must permit an uncertain write
outcome rather than assuming recovery of the old state.

### Generated Keeper operations and independent checking

`--generated-workload` adds 76 sequential client operations before snapshotting:
12 semantic boundary cases and 64 seeded creates, sets, and removes. The generated
namespace includes nested paths, persistent and ephemeral nodes, version matches
and mismatches, duplicate creation, missing nodes, nonempty removal, and attempts
to create children below an ephemeral node. Payloads and operation selection use
a separate seeded generator, not the scheduler's random stream.

After every operation, all three state machines execute real local `get` and
`list` requests for every generated path and its parent within that namespace.
A separate valid observer session reads without renewing the workload session.
Simulation contexts set the
existing `parallel_read_threads` setting to zero: even a small batch otherwise
starts read workers. Production defaults are unchanged, and the thread admission
guard remains active immediately after each read.

`nuraft/keeper_model.py` computes expected response codes and tree state solely
from submissions. It checks payloads, versions, ephemeral owners, child lists,
all three write responses, and completeness of read observations. Failed operations
must leave state unchanged. The same model checks all paths after follower
reconstruction and after replicated session close. Baseline generated runs have
78 checkpoints; combined log-loss runs have 79, including two restorations.
Their snapshot index is 82 and the close index is 83; the non-generated scenario
retains indices 5 and 6.

The executable records observations; a probe `PASS` alone does not mean the model
accepted them. Run `nuraft/verify.py --backend keeper-log` to execute both checks.
The verifier also corrupts seven accepted histories: incorrect error, stale
version, wrong payload, wrong owner, missing child, missing read, and incomplete
close check must each be rejected. These are checker controls, not seven additional
database executions. Manifests retain model summaries and the rejection reasons.

### Atomic and pipelined Keeper transactions

`--multi-workload` includes the 76 single operations and adds 34 atomic multi
transactions: ten fixed cases and 24 seeded transactions, for 110 top-level
requests. Cases include dependent parent/child creation, set and version checks,
delete/recreate, ephemeral ownership, empty multi, and failure at the first,
middle, or last suboperation. The model applies each transaction to a copied
tree and publishes the copy only if every suboperation succeeds. It checks exact
subresponse codes as well as all replicas' resulting state. At the raw
state-machine boundary, a failed multi can have outer response code zero;
subresponses carry the failure. Earlier successful subresponses must not leave
mutations behind, and unexecuted trailing operations have error code `-2`.

Atomic-multi histories have 112 checkpoints normally and 113 with log loss,
including both restorations. Their snapshot and close indices are 116 and 117.
Seven additional corrupted-history controls require rejection of wrong
subresponses, leaked prefix mutations, rolled-back parent creation, execution of
skipped operations, missing results, nonempty results for an empty multi, and
corruption after restoration.

`--pipelined-workload` includes those requests and adds eight batches of four
outstanding multis, for 142 top-level requests including 66 multis. Each batch
queues dependent transactions while leader and follower log writes are paused.
No result, committed state, or local read may expose them before a durable
quorum. After writes resume, every response and replica's tree must match the
independent model. A failed set must not change the version seen by a later
transaction; a failed remove must restore the removed ephemeral node. Reads
check both sides of that boundary for all eight batches.

Pipelined histories have 128 checkpoints normally and 129 with log loss.
Their snapshot and close indices are 148 and 149. Six further checker controls
reject uncommitted visibility, a leaked removal, duplicate version increments,
missing precommit checks, missing responses, and recovered ephemeral-state loss.
Together, the generated, atomic, and pipelined modes run 20 checker controls.
Outstanding-fault checking adds four more, described below. These are not
additional database executions.

The larger workloads exposed a probe retention mismatch: recovery loaded only
100 old log entries but checked the entire prefix. The probe now retains at least
the snapshot index and diagnoses missing entries before serialization. This was
a harness defect, not an established production Keeper correctness bug.

This remains an independent ordered reference model, not a general concurrent
linearizability checker. Pipelining uses one workload session and ordered writes,
not arbitrary concurrent clients. Supported operations are open-ACL,
non-sequential create, versioned set/remove/check, atomic multi, get/list, and close.
It does not model watches, ACL changes, sequential names, concurrent sessions,
or faults at arbitrary operation boundaries. The outstanding-fault scenarios
below extend recovery into selected pipelined batches, not arbitrary
suboperations.

### Faults during outstanding transactions

`--pipeline-fault reply-loss` implies the pipelined workload and requires real
logs. In each of the eight batches, the driver withholds the other follower's
traffic and pauses leader/follower log writes before queuing four requests. The
selected follower's first replication reply is lost after its actual durability
completion, while the leader is still unwritten and the other follower remains
withheld. Every batch result must still be pending and no replica may have
applied the batch at this boundary. Deferred replies still take their normal
completion/cancellation path; they are not treated as synchronous results.
Heartbeat-driven recovery and subsequent replication must complete the batch
with exact state and versions.
This is replication-response loss, not lost client acknowledgement or a TCP fault.

`--pipeline-fault restart` also requires real logs. Before batches 0 and 1, the
driver creates real snapshots at committed indices 116 and 120. While each
four-request batch is accepted but unwritten on follower 3, with no durable
quorum, it stops that follower and explicitly discards its queued, unexecuted
append/flush work. The old server, state machine, context, log store, and manager
must all be destroyed. A fresh context/state machine loads the snapshot and a
fresh log store/manager recovers the exact committed prefix and term/vote.
The four unwritten entries must be absent. Before writes resume, all replicas'
local reads must match the pre-batch model; batch 1's recovery must also preserve
batch 0's committed changes. Retransmission and catch-up then require all four
responses on every replica and exact atomic results. The minority restart does
not replace the leader or model an unknown client outcome.

Restart histories have 130 checkpoints normally and 131 when combined with the
later log-loss scenario. They retain the two pre-batch snapshots plus the final
snapshot at 148; replay compares all three snapshot contents. Reply-loss histories
retain 128/129 checkpoints. Four new checker controls reject uncommitted state
after restart, loss of the previous batch, missing recovery checks, and a missing
fault event. Each mode runs alone and combined with the later snapshot sync
error, partial metadata rewrites, and close-entry log loss.

`--pipeline-fault leader-failover` fails leader 1 during the selected batch, after
follower 2 has durably written all four transactions but before any replica
applies them or the leader acknowledges them. Follower 3 is withheld, and the
leader's writes remain unexecuted. The old leader's server, state machine,
context, log store, and manager must be destroyed; its four queued writes are
discarded. The surviving voters elect node 2 in a later term. The new leader
commits the surviving transactions, while a fresh node 1 recovers snapshot and
byte-exact durable prefix `116+4N` for selected batch `N`, then catches up as a
follower. Batch zero remains the default. Subsequent batches and session closure
use the replacement leader; earlier batches' committed state must survive.

The original four command handles must never report success, including after
catch-up; they are logged as client-unknown. Later state-machine responses are
replicated execution observations, not successful acknowledgements to the old
client. This scenario does not retry uncertain transactions or establish a
client deduplication guarantee. All replicas' reads must match the independent
model, including rollback and exact versions. The election adds a configuration
entry, so the final snapshot is at 149 and closure at 150. These histories have
128/129 checkpoints and retain snapshots on nodes 1 and 3; replay compares
snapshot contents together with their owning node. Four additional corruption
controls reject a missing failure marker, loss or duplicate application of a
surviving transaction, and loss of the pre-failure committed prefix. This mode
also runs alone and combined with later persistence failures. The final-batch
case must still return execution results for all four surviving requests on all
replicas, unlike partition abandonment. Two additional checker controls reject
missing final-batch selection and falsely abandoning those surviving requests.

`--pipeline-fault leader-prefix` instead leaves only the first two of the four
outstanding requests on follower 2. Leader 1 has all four queued but unwritten;
follower 3 has none. After the first two requests reach follower 2, only local
leader work runs, withholding the remaining network traffic. Follower 2 then
durably completes precisely that prefix, without any application or client
acknowledgement. Destroying leader 1 removes its undelivered traffic and
unexecuted writes. The new leader must commit the two surviving requests and
its new-term configuration, not the discarded suffix.

All four original client outcomes remain unknown. The first two requests must
produce results on every replica, while the other two must produce none. The
first multi creates the parent and child; the second multi fails atomically.
The independent model therefore requires child value `pipeline-initial-419`,
version zero, and no ephemeral child from the discarded third request. It derives
that state from submitted transactions, not persisted database contents. Earlier
batches must survive. Final-batch loss of only the suffix must remain valid
through restoration and session closure without hiding missing prefix results.

There are 142 submissions including 66 multis, but 140 requests and 64 multis
produce execution results per replica. These histories have 128/129 checkpoints,
a final snapshot at 147, and close at 148. Seven checker mutations reject a missing
prefix boundary, a returned discarded request, loss of the surviving create,
leaked changes from the failed second multi, missing final-batch selection,
a returned final-batch suffix request, and fabricated discarded-request results
after the final pipeline has finished. The permanent matrix selects batches
0, 3, and 7, alone and combined with later persistence faults. Smoke validation
also exercises every batch. This is one fixed two-request survival boundary,
not arbitrary partial replication or suboperation interruption.

The last control exposed a checker false negative: discarded-request guards
originally applied only while a pipeline was active. Fabricated results for the
last discarded request on all three replicas could therefore be accepted after
the final `workload-pipeline-finished` marker. The guards now apply throughout
the history, including between pipeline completion and recovery. The exact
counterexample is rejected, while the hardened checker preserves summaries for
1,020 retained histories and rejection diagnostics for 156 prior mutations.
The actual recorded database histories contain no such late discarded results;
this is an oracle defect, not an observed production Keeper defect.

`--pipeline-fault leader-prefix-partition` retains the same two-request prefix
without destroying leader 1. All four requests are durable on the isolated old
leader; only the first two are durable on follower 2, and follower 3 has none.
No request has applied or completed before the isolated majority elects node 2.
The majority commits the two surviving requests and a new-term configuration.
Healing must deliver that configuration to the original live server and state
machine, replacing only indices `119+4N` through `120+4N`. Source assertions
compare the entire earlier log prefix, including the surviving requests,
byte-for-byte before and after healing. There is no restart to clear speculative
state or pending callbacks.

The first two requests execute on all replicas; the final two must never execute.
The independent model checks the same retained child value, version, atomic
failure, and absent ephemeral child as the destroyed-leader prefix case. Unlike
that case, an original client handle for a retained request may complete
successfully during healing, or remain uncertain if stepping down cancels it.
The driver records each outcome; the model permits successful completions only
for the retained prefix. A discarded suffix request must never report success.
These are NuRaft command-handle outcomes, not a production ZooKeeper-client
acknowledgement or retry guarantee.

The permanent matrix selects batches 0, 3, and 7, plain and combined with later
persistence failures. Each history submits 142 requests including 66 multis;
140 requests and 64 multis produce execution results per replica. Histories
have 128/129 checkpoints, one final snapshot at 147, and replicated close at 148.
Six additional checker controls reject missing fault/batch selection, lost
surviving state, falsely abandoning the whole batch, successful client completion
of a discarded request, and discarded subresults after the last pipeline ends.
These controls supplement the existing partial-prefix result/rollback controls.

`--pipeline-fault leader-partition` instead keeps the old leader's server and
state machine alive. Its four batch-0 entries are durable only on node 1, with
neither follower receiving them and no application or acknowledgement. Selected
election timers on the other voters elect node 2 in a later term while node 1
still believes itself leader in the old term. The majority commits a new-term
configuration at index 117. Healing delivers the conflicting entry to the same
live former leader, which must step down, replace its tail at indices 117–120,
and converge on index 117 without executing any of the four abandoned requests.
This exercises live conflicting-tail replacement, rather than restart cleanup.

The model discards only this explicitly abandoned batch's speculative effects,
requires no results for its four requests, and checks every replica against the
pre-batch tree after healing. Batch 1 then reuses `/generated/p0` with fresh
request IDs. Exact successful creates, rollback, version 1, and ephemeral
ownership must match a fresh namespace; stale preprocessing must not survive.
There are still 142 submitted requests including 66 multis, but only 138 requests
and 62 multis produce execution results on each replica. The four old client
command outcomes remain unknown. This is not a successful retry of those IDs.

`--pipeline-fault-batch N` selects any batch from 0 through 7 for
`leader-partition`, `leader-failover`, `leader-prefix`, or
`leader-prefix-partition`; zero is the default.
Nonzero selections require one of these modes. For selected partition batch `N`,
the abandoned request IDs are `210+4N` through `213+4N`, and conflicting log indices are
`117+4N` through `120+4N`.
Previously completed batches must survive. If a later batch exists, batch `N+1`
reuses `/generated/pN` with fresh request IDs; other batches retain their own
namespace. Batch 7 has no successor, so its namespace must remain absent through
recovery and closure. The checker permits missing final-request results only
after explicit abandonment of the completed final batch; it still requires all
state observations, recovery checks, and session closure. An earlier checker
incorrectly required results for this abandoned final batch; correcting that
assumption did not require a production Keeper change.

Partition histories have 128/129 checkpoints, a final snapshot at 145, and close
at 146. Four further corruption controls reject missing abandonment, leaked
minority state, falsely acknowledged abandoned requests, and stale versions in
the reused namespace. Two final-batch controls additionally reject a missing
batch-selection declaration and a result for an abandoned request. Every selected
batch runs alone and combined with later snapshot,
metadata, and log-loss recovery. Isolation holds selected node-1 work while the
other two voters progress; it is not arbitrary bidirectional network faulting or
automatic timer exploration.

The extra heartbeat time can cross session-expiry buckets. The harness derives
each replica's expected session deadline from observed commit/restoration times,
the requested 30-second timeout, and 500-ms rounding, checking immediately before
and at every boundary. It does not read expected deadlines from the expiry queue.
Leader completion is not assumed to imply that the restarted follower has caught
up; the driver delivers bounded heartbeat work until every replica has applied
the batch, then checks responses and reads.

These are explicit scenario windows, not arbitrary fault exploration. Normal
shutdown cleanup still executes, and only unexecuted writes are discarded;
already executed filesystem writes are not rolled back. These are not OS
process kills, a power-loss model, or general concurrent-client linearizability
tests. Leader changes are limited to the explicit surviving-batch and minority-tail
windows above; the other two modes retain the leader. Production Keeper code is
unchanged by these extensions.

### Current measured result and remaining work

The debug and ASan/UBSan standalone builds pass the verification suite, including
the seven expected negative-control failures. Repeated and saved-decision recovery
traces match with and without host delays and across these two builds on the tested
GCC/Linux environment. A separate development campaign also passed seeds 0–100
and 4,294,967,295 on both builds: 204 executions, with 102 matching cross-build
trace pairs. It combines response loss, deferred follower durability, recovery,
leader failover, volatile-tail loss, and late completion after sender destruction;
those logs are validation evidence, not additional default cases.
Virtual runs report no attempted background-thread creation, one live thread, and zero
delegations from the wrapped clocks to host time. These are observations of the
exercised paths, not proof of universal determinism. The Python contract tests
remain separate and unchanged.

The root Clang 22 Release build of `unit_tests_dbms` also succeeded. The targeted
clock/preprocessing/snapshot invocation passed eight cases, four each for Mem and
LSMT. The broader `Storage/CoordinationTest.*` invocation passed all 174 cases,
87 per implementation, with no skips or failures. The targeted cases are a subset
of that broader suite, not eight additional distinct tests. These are real Keeper
component results, including snapshot recreation of the injected expiry clock;
they do not run those components inside the standalone manual NuRaft driver.

The root real-Keeper driver additionally passes its 41-run verification: all 33
successful Mem scenarios have matching per-seed traces under host delays and
replay, create a real snapshot, restore it into a fresh state machine/context,
and continue replicated session closure. They report zero attempted threads,
one live thread, and zero wrapped host-clock delegations during the measured
scenario. These bounded results do not admit LSMT or production Keeper dispatch.

The preceding partial-survivor campaign passed 435 real-persistence executions,
including partial metadata recovery, snapshot partial-write/sync/publication
cleanup and retry, independent generated/atomic/pipelined workload checking, and
post-restart voting. Its initial 42 corrupted-history controls are rejected
separately.
A boundary-seed invocation with `--seeds 4294967295 0 --repeats 2` also passes
286 executions, including all 23 intended failures and all 42 initial checker controls.
The hardened checker adds the late-result regression described above, for 43 controls.
Its fresh `--seeds 17 --repeats 2` verification passes 155 executions, including
23 intended failures, and rejects all 43 mutations. No C++ runtime change was
needed for the checker hardening. The initial suites' retained histories were
also reprocessed with the hardened model without changing their summaries.
Across the default and boundary invocations, all 320 successful record/replay pairs match traces
and persisted contents, including all retained snapshots and their node identity.
The 420 outstanding-fault histories across those five seeds cover all five original fault
modes, including every partition and leader-failover batch, alone and combined
with later persistence faults. All 398 prior default and 261 prior boundary
executions retain identical trace streams and persisted content. Pipelined histories
independently check 142 submissions, 66 transactions, and eight four-request batches per history, including
precommit visibility and post-recovery state. Normal histories have 128
checkpoints and combined recovery histories have 129; pending-restart histories
add two precommit recovery checks, for 130/131. Minority-tail partition histories
include one explicitly abandoned batch, not eight executed batches. These remain ordered
single-session workloads, not concurrent-client linearizability evidence.

The live-leader partial-prefix extension passes the current 472-run default and
311-run boundary matrices, including 24 intended failures each. Both reject all
49 corrupted-history controls, and their 600 modeled histories match the
independent model. The 60 added histories span five seeds, batches 0/3/7,
plain/combined recovery, and record/replay. Across both matrices, all 350
successful replay pairs match traces and persisted contents. All 435 preceding
default cases and 286 preceding boundary cases retain their outcomes, model
summaries, traces, schedules, and normalized persisted bytes. Separate final
smokes exercise all eight batch positions, including both successful and
uncertain original handles for the retained prefix; no discarded request
reports success. The standalone normal/ASan and modeled-Keeper regressions also
pass, for 924 campaign executions overall: 855 successes and 69 required
negative outcomes. These totals exclude the eight final smokes and four
contract tests. Standalone sanitizer coverage does not imply sanitizer coverage
of the real-persistence Keeper driver.

A separate 33-seed partition campaign passes 2,100 executions: 2,080 successes
and 20 intended failures, with 32 rejected history mutations and 990 matching
record/replay pairs. Its 1,320 modeled histories were independently reprocessed,
and all 143,616 recorded persistence artifacts were rehashed without mismatch.
All 1,967 executions shared with the preceding leader campaign retain identical
trace streams. This campaign uses the preserved batch-zero partition executable
and verification-time checker; it does not extend selectable-batch coverage.
Historical source snapshots identify the checker used, not all compiler inputs.

A further generated-workload campaign passes 102 executions: seeds 0–15 and
4,294,967,295, each with partial, sync, and publication snapshot failures combined
with partial metadata rewrites and log loss. All 51 record/replay pairs match
traces and persisted bytes, and all 102 histories pass the independent model with
76 operations and 79 checkpoints. Replays inject host delay. These are additional
validation runs, not extra default verifier cases.
A separate development campaign passed 408 executions: seeds 0–100 and
4,294,967,295, with log loss alone and combined with failed metadata rewrites,
each recorded and replayed with host delay. Another 204 executions combine
snapshot failure/retry with metadata rewrites and log loss on those same seeds.
These earlier campaigns predate the deeper faults and generated workloads. They
retain trace and physical-persistence hashes; they are extra validation evidence,
not additional default verifier cases or evidence for the later scenarios.
The broader changelog/storage regression selected 555 cases: 478 passed and
77 explicitly skipped, with no failures.
Skipped cases are not coverage of their unsupported storage/compression paths.

Further runtime integration must preserve the gate:

- Extend explicit clock ownership beyond session expiry before relying on it for
  production Keeper integration; NuRaft still uses build-local interception here.
- Add scheduler-owned LSMT flush/merge work without bypassing storage semantics.
- Extend beyond the bounded Mem state-machine path to production Keeper dispatch,
  arbitrary-boundary log/metadata/snapshot I/O faults, remaining maintenance work,
  blocking waits, and lifecycle. Real disk I/O inside an atomic selected action
  does not establish power-loss durability.
- Resolve ClickHouse's node-owned context/catalog boundary before hosting two
  replicated-table nodes in one process. Do not remove singleton guards as a fix.
- Only then rerun the replicated-insert contract under scheduler-controlled
  faults and compare it with the real-process oracle, including unknown outcomes.

Those are runtime changes, not additional Python orchestration. Their acceptance
conditions remain below; the bounded C++ probe does not complete milestone 3.

### Conditions to open the gate

1. Independent node state and restart lifecycle, without process-singleton leakage.
2. No unregistered runnable work on the exercised path.
3. Scheduler-aware Keeper/quorum waits, fetches, cancellation, and shutdown.
4. Controlled behavior-relevant clocks and randomness, including NuRaft internals.
5. Repeated normalized traces match from the same initial state and decisions,
   including when host delays vary.
6. Quorum, cancellation, unknown outcomes, and recovery match this reference contract.

A seed, one worker thread, a fake timer, or a passing real-process workload does
not satisfy these conditions. In-process replicated-storage simulation requires
further runtime work; milestone 3 is not complete.
