"""Verify bounded NuRaft or real Keeper repeatability, not full ClickHouse determinism."""

import argparse
import hashlib
import json
import subprocess
from pathlib import Path

from keeper_model import check_history


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repeats", type=int, default=5)
    parser.add_argument("--seeds", type=int, nargs="+", default=[1, 17, 991])
    parser.add_argument(
        "--backend", choices=("nuraft", "keeper", "keeper-log", "figure8"), default="nuraft"
    )
    args = parser.parse_args()
    if args.repeats < 2:
        parser.error("--repeats must be at least two")
    if len(set(args.seeds)) != len(args.seeds) or any(
        seed < 0 or seed > 4294967295 for seed in args.seeds
    ):
        parser.error("--seeds must be unique uint32 values")
    if args.backend == "figure8":
        if args.seeds != [1]:
            parser.error("Figure 8 uses one selected schedule; specify --seeds 1")
        from verify_raft import verify

        if not verify(args.binary, args.output, args.repeats):
            parser.exit(1, "FAIL reproduced Raft current-term commitment violation; inspect manifest\n")
        return
    control_seed = args.seeds[0]
    binary = args.binary.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[3]
    keeper_backend = args.backend != "nuraft"
    real_logs = args.backend == "keeper-log"
    manifest = {
        "backend": args.backend,
        "binary": str(binary),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "checkout_at_verification": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=root, text=True
        ).strip(),
        "nuraft_at_verification": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=root / "contrib/NuRaft", text=True
        ).strip(),
        "source_sha256_at_verification": {
            name: hashlib.sha256((root / name).read_bytes()).hexdigest()
            for name in (
                "tests/simulation/nuraft/CMakeLists.txt",
                "tests/simulation/nuraft/probe.cpp",
                "tests/simulation/nuraft/verify.py",
                "tests/simulation/nuraft/keeper_model.py",
                "src/Coordination/SessionExpiryQueue.h",
                "src/Coordination/SessionExpiryQueue.cpp",
                "src/CMakeLists.txt",
                "src/Coordination/KeeperContext.h",
                "src/Coordination/KeeperContext.cpp",
                "src/Coordination/KeeperStorage.cpp",
                "src/Coordination/KeeperStateMachine.cpp",
                "src/Coordination/Changelog.h",
                "src/Coordination/Changelog.cpp",
                "src/Coordination/KeeperLogStore.h",
                "src/Coordination/KeeperLogStore.cpp",
                "src/Coordination/KeeperStateManager.h",
                "src/Coordination/KeeperStateManager.cpp",
                "src/Coordination/KeeperSnapshotManager.h",
                "src/Coordination/KeeperSnapshotManager.cpp",
            )
        },
        "provenance_note": "Checkout hashes describe verification time, not an attestation of binary build inputs.",
        "full_clickhouse_deterministic_gate": "blocked",
        "seeds": args.seeds,
        "repeats": args.repeats,
        "runs": [],
        "checker_controls": [],
        "verdict": "running",
    }
    manifest_path = output / "manifest.json"

    def save():
        manifest_path.write_text(json.dumps(manifest, indent=2) + "\n")

    def run(name, options, expected_failure=None):
        if keeper_backend:
            options = [*options, "--state-dir", str(output / f"{name}-state")]
        if real_logs:
            options.append("--real-log")
        command = [str(binary), *options]
        log = output / f"{name}.log"
        with log.open("w") as stream:
            result = subprocess.run(
                command, stdout=stream, stderr=subprocess.STDOUT, timeout=30
            )
        text = log.read_text()
        manifest["runs"].append(
            {"command": command, "log": log.name, "exit_code": result.returncode}
        )
        if keeper_backend:
            manifest["runs"][-1]["snapshot_files"] = [
                {
                    "path": str(path.relative_to(output)),
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                }
                for path in sorted((output / f"{name}-state").glob("*/snapshot_*"))
            ]
        if real_logs:
            manifest["runs"][-1]["metadata_files"] = [
                {
                    "path": str(path.relative_to(output)),
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                }
                for path in sorted((output / f"{name}-state").glob("*/metadata/*"))
                if path.is_file()
            ]
            manifest["runs"][-1]["log_files"] = [
                {
                    "path": str(path.relative_to(output)),
                    "sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
                }
                for path in sorted(
                    (output / f"{name}-state").glob("*/logs/changelog_*")
                )
            ]
        save()
        if expected_failure:
            if result.returncode != 1 or f"FAIL {expected_failure}" not in text:
                raise RuntimeError(f"{name}: did not detect the deliberate fault")
        elif result.returncode or "PASS " not in text:
            raise RuntimeError(f"{name}: probe failed; inspect {log}")
        elif real_logs and name != "instrumentation-control":
            metadata = manifest["runs"][-1]["metadata_files"]
            if {str(Path(*item["path"].split("/")[1:])) for item in metadata} != {
                f"{node}/metadata/state" for node in (1, 2, 3)
            }:
                raise RuntimeError(f"{name}: expected three persisted Raft state files")
            if "keeper-metadata-recovered-exact 3 " not in text:
                raise RuntimeError(f"{name}: metadata recovery evidence missing")
            if "keeper-vote-retry-and-conflict-checked 3 " not in text:
                raise RuntimeError(f"{name}: restarted vote safety evidence missing")
            fault = (
                options[options.index("--pipeline-fault") + 1]
                if "--pipeline-fault" in options
                else ""
            )
            expected_snapshots = {
                "restart": 3,
                "leader-failover": 2,
                "leader-prefix": 2,
            }.get(fault, 1)
            if len(manifest["runs"][-1]["snapshot_files"]) != expected_snapshots:
                raise RuntimeError(
                    f"{name}: expected {expected_snapshots} published snapshots after retry"
                )
            batch = (
                int(options[options.index("--pipeline-fault-batch") + 1])
                if "--pipeline-fault-batch" in options
                else 0
            )
            if fault in ("leader-failover", "leader-prefix") and (
                text.count(f"workload-pipeline-fault {batch} {fault}\n") != 1
                or text.count("keeper-pipeline-leader-replaced 1 2 ") != 1
                or text.count(f"keeper-pipeline-prefix-recovered 1 {116 + 4 * batch}\n")
                != 1
                or any(
                    text.count(f"keeper-pipeline-client-unknown {xid}\n") != 1
                    for xid in range(210 + 4 * batch, 214 + 4 * batch)
                )
            ):
                raise RuntimeError(
                    f"{name}: outstanding leader failure evidence missing"
                )
            if fault in ("leader-prefix", "leader-prefix-partition") and (
                text.count(
                    f"keeper-pipeline-surviving-prefix {batch} 2 {118 + 4 * batch}\n"
                )
                != 1
                or text.count(f"workload-pipeline-prefix {batch} 2\n") != 1
            ):
                raise RuntimeError(f"{name}: partial surviving prefix evidence missing")
            if fault in ("leader-partition", "leader-prefix-partition"):
                partial = fault == "leader-prefix-partition"
                if (
                    text.count(f"workload-pipeline-fault {batch} {fault}\n") != 1
                    or text.count(f"workload-pipeline-aborted {batch}\n")
                    != int(not partial)
                    or text.count("keeper-pipeline-leader-replaced 1 2 ") != 1
                    or text.count("keeper-pipeline-partition-healed 1\n") != 1
                    or text.count(
                        f"keeper-pipeline-conflicting-tail-replaced 1 {117 + 4 * batch + 2 * partial} {120 + 4 * batch}\n"
                    )
                    != 1
                    or any(
                        text.count(f"keeper-pipeline-client-unknown {xid}\n")
                        + text.count(f"workload-pipeline-client-committed {xid}\n")
                        != 1
                        for xid in range(210 + 4 * batch, 214 + 4 * batch)
                    )
                ):
                    raise RuntimeError(
                        f"{name}: conflicting minority tail evidence missing"
                    )
        if not expected_failure and any(
            option in options
            for option in (
                "--generated-workload",
                "--multi-workload",
                "--pipelined-workload",
                "--pipeline-fault",
            )
        ):
            manifest["runs"][-1]["model_check"] = check_history(text)
            if "--log-loss" in options and "workload-checked restored-2" not in text:
                raise RuntimeError(f"{name}: second recovery model check missing")
            save()
        return text

    def trace(text):
        lines = [line for line in text.splitlines() if line.startswith("TRACE ")]
        if not lines:
            raise RuntimeError("Empty execution trace")
        return "\n".join(lines) + "\n"

    def log_hashes():
        return {
            str(Path(*item["path"].split("/")[1:])): item["sha256"]
            for item in manifest["runs"][-1]["log_files"]
        }

    def metadata_hashes():
        return {
            str(Path(*item["path"].split("/")[1:])): item["sha256"]
            for item in manifest["runs"][-1]["metadata_files"]
        }

    def snapshot_hash():
        return sorted(
            (item["path"].split("/")[1], item["sha256"])
            for item in manifest["runs"][-1]["snapshot_files"]
        )

    def checker_controls(text, kind="generated"):
        # Corrupt a real, already accepted history, rather than fabricating a passing fixture.
        mutations = {
            "wrong-result": ("workload-result 1 102 -110", "workload-result 1 102 0"),
            "stale-version": (
                "/generated/a 0 updated-937 1 0",
                "/generated/a 0 updated-937 0 0",
            ),
            "wrong-payload": (
                "/generated/a 0 updated-937 1 0",
                "/generated/a 0 corrupted-12 1 0",
            ),
            "wrong-owner": (
                "/generated/e 0 ephemeral-593 0 1",
                "/generated/e 0 ephemeral-593 0 0",
            ),
            "missing-child": (
                "workload-list 1 /generated 0 1 a",
                "workload-list 1 /generated 0 0",
            ),
            "missing-read": (
                "workload-get 2 /generated 0 root-83 0 0",
                "discarded-read",
            ),
            "missing-close-check": ("workload-checked closed", "discarded-close-check"),
        }
        if kind == "multi":
            mutations = {
                "multi-wrong-suberror": (
                    "workload-multi-result 1 177 3 0 -103 -2",
                    "workload-multi-result 1 177 3 0 0 0",
                ),
                "multi-prefix-leak": (
                    "/generated/txn/a 0 inside-941 1 0",
                    "/generated/txn/a 0 must-rollback-431 2 0",
                ),
                "multi-create-leak": (
                    "/generated/txn2 -101 - 0 0",
                    "/generated/txn2 0 parent-283 0 0",
                ),
                "multi-skipped-create": (
                    "/generated/rolled-back -101 - 0 0",
                    "/generated/rolled-back 0 unexecuted-523 0 0",
                ),
                "multi-missing-result": (
                    "workload-multi-result 2 177 3 0 -103 -2",
                    "discarded-multi-result",
                ),
                "multi-empty-result": (
                    "workload-multi-result 1 185 0",
                    "workload-multi-result 1 185 1 0",
                ),
                "multi-recovered-leak": (
                    "/generated/a 0 updated-937 1 0",
                    "/generated/a 0 corrupted-rollback 2 0",
                ),
            }
        elif kind == "pipeline":
            mutations = {
                "pipeline-uncommitted-leak": (
                    "/generated/p0 -101 - 0 0",
                    "/generated/p0 0 pipeline-parent-607 0 0",
                ),
                "pipeline-remove-leak": (
                    "/generated/p1/e 0 pipeline-ephemeral-257 0 1",
                    "/generated/p1/e -101 - 0 0",
                ),
                "pipeline-duplicate-apply": (
                    "/generated/p0/a 0 pipeline-final-953 2 0",
                    "/generated/p0/a 0 pipeline-final-953 3 0",
                ),
                "pipeline-missing-precommit-check": (
                    "workload-checked pipeline-before-0",
                    "discarded-precommit-check",
                ),
                "pipeline-missing-result": (
                    "workload-multi-result 1 211 2 0 -101",
                    "discarded-pipeline-result",
                ),
                "pipeline-recovered-leak": (
                    "/generated/p1/e 0 pipeline-ephemeral-257 0 1",
                    "/generated/p1/e -101 - 0 0",
                ),
            }
        elif kind == "pending":
            mutations = {
                "pending-uncommitted-leak": (
                    "/generated/p0 -101 - 0 0",
                    "/generated/p0 0 pipeline-parent-607 0 0",
                ),
                "pending-prefix-loss": (
                    "/generated/p0/a 0 pipeline-final-953 2 0",
                    "/generated/p0/a -101 - 0 0",
                ),
                "pending-missing-recovery-check": (
                    "workload-checked pipeline-recovered-0",
                    "discarded-recovery-check",
                ),
                "pending-missing-fault": (
                    "workload-pipeline-fault 0 restart",
                    "discarded-pending-fault",
                ),
            }
        elif kind == "leader":
            mutations = {
                "leader-missing-fault": (
                    "workload-pipeline-fault 0 leader-failover",
                    "discarded-leader-fault",
                ),
                "leader-lost-surviving-transaction": (
                    "/generated/p0/a 0 pipeline-final-953 2 0",
                    "/generated/p0/a -101 - 0 0",
                ),
                "leader-duplicate-application": (
                    "/generated/p0/a 0 pipeline-final-953 2 0",
                    "/generated/p0/a 0 pipeline-final-953 3 0",
                ),
                "leader-lost-old-prefix": (
                    "/generated/a 0 updated-937 1 0",
                    "/generated/a -101 - 0 0",
                ),
            }
        elif kind == "partition":
            mutations = {
                "partition-missing-abort": (
                    "workload-pipeline-aborted 0",
                    "discarded-abort",
                ),
                "partition-visibility-leak": (
                    "/generated/p0 -101 - 0 0",
                    "/generated/p0 0 pipeline-parent-607 0 0",
                ),
                "partition-acknowledged-abandoned": (
                    "workload-pipeline-aborted 0",
                    "workload-result 1 210 0",
                ),
                "partition-reused-version": (
                    "/generated/p0/a 0 pipeline-committed-821 1 0",
                    "/generated/p0/a 0 pipeline-committed-821 2 0",
                ),
            }
        elif kind == "partition-last":
            mutations = {
                "partition-last-missing-selection": (
                    "workload-pipeline-fault-batch 7",
                    "discarded-batch-selection",
                ),
                "partition-last-returned-abandoned": (
                    "workload-pipeline-aborted 7",
                    "workload-pipeline-aborted 7\nTRACE 0 workload-result 1 238 0",
                ),
            }
        elif kind == "leader-last":
            mutations = {
                "leader-last-missing-selection": (
                    "workload-pipeline-fault-batch 7",
                    "discarded-batch-selection",
                ),
                "leader-last-abandoned-survivor": (
                    "workload-pipeline-fault 7 leader-failover",
                    "workload-pipeline-fault 7 leader-failover\nTRACE 0 workload-pipeline-aborted 7",
                ),
            }
        elif kind == "prefix":
            mutations = {
                "prefix-missing-boundary": (
                    "workload-pipeline-prefix 0 2",
                    "discarded-prefix-boundary",
                ),
                "prefix-tail-returned": (
                    "workload-pipeline-prefix 0 2",
                    "workload-pipeline-prefix 0 2\nTRACE 0 workload-result 1 212 0",
                ),
                "prefix-lost-survivor": (
                    "/generated/p0/a 0 pipeline-initial-419 0 0",
                    "/generated/p0/a -101 - 0 0",
                ),
                "prefix-rollback-leak": (
                    "/generated/p0/a 0 pipeline-initial-419 0 0",
                    "/generated/p0/a 0 pipeline-rollback-683 1 0",
                ),
            }
        elif kind == "prefix-last":
            mutations = {
                "prefix-last-missing-selection": (
                    "workload-pipeline-fault-batch 7",
                    "discarded-batch-selection",
                ),
                "prefix-last-tail-returned": (
                    "workload-pipeline-prefix 7 2",
                    "workload-pipeline-prefix 7 2\nTRACE 0 workload-result 1 240 0",
                ),
                "prefix-last-late-tail-returned": (
                    "workload-pipeline-finished 7\n",
                    "workload-pipeline-finished 7\n"
                    + "".join(
                        f"TRACE 0 workload-result {node} 241 0\n"
                        f"TRACE 0 workload-multi-result {node} 241 3 0 -110 -2\n"
                        for node in (1, 2, 3)
                    ),
                ),
            }
        elif kind == "live-prefix":
            mutations = {
                "live-prefix-missing-fault": (
                    "workload-pipeline-fault 0 leader-prefix-partition",
                    "discarded-live-prefix-fault",
                ),
                "live-prefix-lost-survivor": (
                    "/generated/p0/a 0 pipeline-initial-419 0 0",
                    "/generated/p0/a -101 - 0 0",
                ),
                "live-prefix-abandoned-survivors": (
                    "workload-pipeline-prefix 0 2",
                    "workload-pipeline-aborted 0",
                ),
                "live-prefix-acknowledged-tail": (
                    "workload-pipeline-prefix 0 2",
                    "workload-pipeline-prefix 0 2\nTRACE 0 workload-pipeline-client-committed 212",
                ),
            }
        elif kind == "live-prefix-last":
            mutations = {
                "live-prefix-last-missing-selection": (
                    "workload-pipeline-fault-batch 7",
                    "discarded-batch-selection",
                ),
                "live-prefix-last-late-subresults": (
                    "workload-pipeline-finished 7\n",
                    "workload-pipeline-finished 7\n"
                    + "".join(
                        f"TRACE 0 workload-multi-result {node} 241 3 0 -110 -2\n"
                        for node in (1, 2, 3)
                    ),
                ),
            }
        scoped_phases = {
            "multi-prefix-leak": "step-177",
            "multi-create-leak": "step-181",
            "multi-skipped-create": "step-177",
            "multi-recovered-leak": "restored-1",
            "pipeline-uncommitted-leak": "pipeline-before-0",
            "pipeline-remove-leak": "pipeline-after-1",
            "pipeline-duplicate-apply": "pipeline-after-0",
            "pipeline-recovered-leak": "restored-1",
            "pending-uncommitted-leak": "pipeline-recovered-0",
            "pending-prefix-loss": "pipeline-recovered-1",
            "leader-lost-surviving-transaction": "pipeline-after-0",
            "leader-duplicate-application": "pipeline-after-0",
            "live-prefix-lost-survivor": "pipeline-after-0",
            "leader-lost-old-prefix": "pipeline-after-0",
            "partition-visibility-leak": "pipeline-after-0",
            "partition-reused-version": "pipeline-after-1",
            "prefix-lost-survivor": "pipeline-after-0",
            "prefix-rollback-leak": "pipeline-after-0",
        }
        for name, (old, new) in mutations.items():
            prefix, separator, tail = "", "", text
            if name in scoped_phases:
                prefix, separator, tail = text.partition(
                    f"workload-check {scoped_phases[name]}\n"
                )
                if not separator:
                    raise RuntimeError(f"Checker control {name}: checkpoint missing")
            if old not in tail:
                raise RuntimeError(f"Checker control {name}: mutation did not match")
            corrupted = prefix + separator + tail.replace(old, new, 1)
            path = output / f"checker-control-{name}.log"
            path.write_text(corrupted)
            try:
                check_history(corrupted)
            except ValueError as error:
                manifest["checker_controls"].append(
                    {"log": path.name, "rejection": str(error)}
                )
            else:
                raise RuntimeError(f"Checker accepted {name}")
        save()

    save()
    try:
        run("instrumentation-control", ["--instrumentation-control"])
        if not keeper_backend:
            keeper = run("keeper-expiry", ["--keeper-expiry"])
            keeper_delayed = run(
                "keeper-expiry-host-delay", ["--keeper-expiry", "--host-delay"]
            )
            if keeper != keeper_delayed:
                raise RuntimeError("Keeper expiry result changed with host delays")
            run("real-clock-reference", [])
        run(
            "unsafe-quorum-control",
            ["--virtual-time", "--unsafe-quorum-control"],
            expected_failure=(
                "Keeper no-quorum append falsely completed"
                if keeper_backend
                else "No-quorum append falsely completed"
            ),
        )
        run(
            "unsafe-durability-control",
            ["--virtual-time", "--unsafe-durability-control"],
            expected_failure=(
                "Keeper undurable majority falsely completed append"
                if keeper_backend
                else "Undurable leader falsely counted toward majority"
            ),
        )
        if real_logs:
            run(
                "graceful-shutdown-loss-control",
                ["--virtual-time", "--unsafe-log-loss-control"],
                expected_failure="Keeper disk recovery changed log tail",
            )
            run(
                "metadata-loss-control",
                ["--virtual-time", "--unsafe-metadata-loss-control"],
                expected_failure="Keeper restart lost term/vote state",
            )
            run(
                "metadata-backup-loss-control",
                ["--virtual-time", "--unsafe-metadata-backup-control"],
                expected_failure="Keeper restart lost term/vote state",
            )
            run(
                "snapshot-write-control",
                ["--virtual-time", "--unsafe-snapshot-write-control"],
                expected_failure="Keeper failed snapshot was reported successful",
            )
        hashes = []
        decision_hashes = []
        recovery_hashes = {}
        for seed in manifest["seeds"]:
            expected = None
            expected_logs = None
            expected_metadata = None
            expected_snapshot = None
            schedule = output / f"seed-{seed}.schedule"
            for repeat in range(args.repeats):
                for delayed in (False, True):
                    name = f"seed-{seed}-repeat-{repeat}-delay-{int(delayed)}"
                    options = ["--virtual-time", "--seed", str(seed)]
                    if repeat == 0 and not delayed:
                        options.extend(["--record-schedule", str(schedule)])
                    if delayed:
                        options.append("--host-delay")
                    text = run(name, options)
                    if keeper_backend:
                        for marker in (
                            "clock_control=PARTIAL_POSIX persistence="
                            + (
                                "REAL_LOG_REAL_SNAPSHOT_REAL_METADATA"
                                if real_logs
                                else "MODELED_LOG_REAL_SNAPSHOT"
                            ),
                            "keeper-durable-majority-and-catchup",
                            "keeper-lost-reply-update-exact",
                            "keeper-snapshot-follower-restored",
                            "keeper-expiry-close-replicated",
                            "keeper-shutdown-clean",
                        ):
                            if marker not in text:
                                raise RuntimeError(
                                    f"{name}: Keeper evidence missing: {marker}"
                                )
                        if len(manifest["runs"][-1]["snapshot_files"]) != 1:
                            raise RuntimeError(
                                f"{name}: expected one real Keeper snapshot"
                            )
                        if real_logs:
                            if (
                                "keeper-log-recovered-exact 3 5" not in text
                                or "keeper-metadata-recovered-exact 3 " not in text
                                or len(log_hashes()) != 6
                            ):
                                raise RuntimeError(
                                    f"{name}: real log recovery evidence missing"
                                )
                            if expected_logs is None:
                                expected_logs = log_hashes()
                                expected_metadata = metadata_hashes()
                                expected_snapshot = snapshot_hash()
                            elif log_hashes() != expected_logs:
                                raise RuntimeError(
                                    f"{name}: persisted log bytes diverged"
                                )
                            if metadata_hashes() != expected_metadata:
                                raise RuntimeError(
                                    f"{name}: persisted metadata bytes diverged"
                                )
                            if snapshot_hash() != expected_snapshot:
                                raise RuntimeError(f"{name}: snapshot bytes diverged")
                    elif "host_clock_reads=0" not in text:
                        raise RuntimeError(f"{name}: host clock delegation detected")
                    actual = trace(text)
                    (output / f"{name}.trace").write_text(actual)
                    if expected is None:
                        expected = actual
                    elif actual != expected:
                        raise RuntimeError(
                            f"{name}: normalized execution trace diverged"
                        )
            replay = run(
                f"seed-{seed}-replay",
                [
                    "--virtual-time",
                    "--seed",
                    str(seed),
                    "--host-delay",
                    "--replay-schedule",
                    str(schedule),
                ],
            )
            if trace(replay) != expected:
                raise RuntimeError(f"Seed {seed}: saved-decision replay diverged")
            if real_logs and log_hashes() != expected_logs:
                raise RuntimeError(f"Seed {seed}: replayed log bytes diverged")
            if real_logs and metadata_hashes() != expected_metadata:
                raise RuntimeError(f"Seed {seed}: replayed metadata bytes diverged")
            if real_logs and snapshot_hash() != expected_snapshot:
                raise RuntimeError(f"Seed {seed}: replayed snapshot bytes diverged")
            (output / f"seed-{seed}-replay.trace").write_text(trace(replay))
            if not keeper_backend or real_logs:
                recovery_schedule = output / f"seed-{seed}-recovery.schedule"
                recovery_options = (
                    ["--log-loss"] if real_logs else ["--recovery", "--lost-reply"]
                )
                recovery = run(
                    f"seed-{seed}-recovery",
                    [
                        "--virtual-time",
                        "--seed",
                        str(seed),
                        *recovery_options,
                        "--record-schedule",
                        str(recovery_schedule),
                    ],
                )
                recovered_logs = log_hashes() if real_logs else None
                recovered_metadata = metadata_hashes() if real_logs else None
                recovered_snapshot = snapshot_hash() if real_logs else None
                recovery_replay = run(
                    f"seed-{seed}-recovery-replay",
                    [
                        "--virtual-time",
                        "--seed",
                        str(seed),
                        *recovery_options,
                        "--host-delay",
                        "--replay-schedule",
                        str(recovery_schedule),
                    ],
                )
                if trace(recovery) != trace(recovery_replay):
                    raise RuntimeError(f"Seed {seed}: recovery replay diverged")
                if real_logs:
                    for text in (recovery, recovery_replay):
                        if (
                            "keeper-log-unwritten-tail-discarded 3 6" not in text
                            or "keeper-log-loss-retransmitted-exact" not in text
                        ):
                            raise RuntimeError(
                                f"Seed {seed}: interrupted-write evidence missing"
                            )
                    if log_hashes() != recovered_logs:
                        raise RuntimeError(f"Seed {seed}: recovery log bytes diverged")
                    if metadata_hashes() != recovered_metadata:
                        raise RuntimeError(
                            f"Seed {seed}: recovery metadata bytes diverged"
                        )
                    if snapshot_hash() != recovered_snapshot:
                        raise RuntimeError(
                            f"Seed {seed}: recovery snapshot bytes diverged"
                        )
                (output / f"seed-{seed}-recovery.trace").write_text(trace(recovery))
                (output / f"seed-{seed}-recovery-replay.trace").write_text(
                    trace(recovery_replay)
                )
                recovery_hashes[seed] = hashlib.sha256(
                    trace(recovery).encode()
                ).hexdigest()
            if real_logs:
                for scenario, scenario_options in (
                    ("metadata-rewrite", ["--metadata-write-failure"]),
                    (
                        "metadata-rewrite-log-loss",
                        ["--metadata-write-failure", "--log-loss"],
                    ),
                    ("snapshot-retry", ["--snapshot-write-failure"]),
                    (
                        "snapshot-metadata-log-loss",
                        [
                            "--snapshot-write-failure",
                            "--metadata-write-failure",
                            "--log-loss",
                        ],
                    ),
                    ("metadata-partial", ["--metadata-fault", "partial"]),
                    ("snapshot-partial", ["--snapshot-fault", "partial"]),
                    ("snapshot-sync", ["--snapshot-fault", "sync"]),
                    ("snapshot-publication", ["--snapshot-fault", "marker"]),
                    ("generated", ["--generated-workload"]),
                    ("multi", ["--multi-workload"]),
                    ("pipeline", ["--pipelined-workload"]),
                    *(
                        (f"pending-{mode}", ["--pipeline-fault", mode])
                        for mode in (
                            "reply-loss",
                            "restart",
                            "leader-failover",
                            "leader-partition",
                        )
                    ),
                    *(
                        (
                            f"pending-{mode}-recovery",
                            [
                                "--pipeline-fault",
                                mode,
                                "--snapshot-fault",
                                "sync",
                                "--metadata-fault",
                                "partial",
                                "--log-loss",
                            ],
                        )
                        for mode in (
                            "reply-loss",
                            "restart",
                            "leader-failover",
                            "leader-partition",
                        )
                    ),
                    *(
                        (
                            f"partition-batch-{batch}{'-recovery' if recovery else ''}",
                            [
                                "--pipeline-fault",
                                "leader-partition",
                                "--pipeline-fault-batch",
                                str(batch),
                                *(
                                    [
                                        "--snapshot-fault",
                                        "sync",
                                        "--metadata-fault",
                                        "partial",
                                        "--log-loss",
                                    ]
                                    if recovery
                                    else []
                                ),
                            ],
                        )
                        for batch in range(1, 8)
                        for recovery in (False, True)
                    ),
                    *(
                        (
                            f"leader-batch-{batch}{'-recovery' if recovery else ''}",
                            [
                                "--pipeline-fault",
                                "leader-failover",
                                "--pipeline-fault-batch",
                                str(batch),
                                *(
                                    [
                                        "--snapshot-fault",
                                        "sync",
                                        "--metadata-fault",
                                        "partial",
                                        "--log-loss",
                                    ]
                                    if recovery
                                    else []
                                ),
                            ],
                        )
                        for batch in range(1, 8)
                        for recovery in (False, True)
                    ),
                    *(
                        (
                            f"prefix-batch-{batch}{'-recovery' if recovery else ''}",
                            [
                                "--pipeline-fault",
                                "leader-prefix",
                                "--pipeline-fault-batch",
                                str(batch),
                                *(
                                    [
                                        "--snapshot-fault",
                                        "sync",
                                        "--metadata-fault",
                                        "partial",
                                        "--log-loss",
                                    ]
                                    if recovery
                                    else []
                                ),
                            ],
                        )
                        for batch in (0, 3, 7)
                        for recovery in (False, True)
                    ),
                    *(
                        (
                            f"live-prefix-batch-{batch}{'-recovery' if recovery else ''}",
                            [
                                "--pipeline-fault",
                                "leader-prefix-partition",
                                "--pipeline-fault-batch",
                                str(batch),
                                *(
                                    [
                                        "--snapshot-fault",
                                        "sync",
                                        "--metadata-fault",
                                        "partial",
                                        "--log-loss",
                                    ]
                                    if recovery
                                    else []
                                ),
                            ],
                        )
                        for batch in (0, 3, 7)
                        for recovery in (False, True)
                    ),
                    *(
                        (
                            f"generated-{mode}-recovery",
                            [
                                "--generated-workload",
                                "--snapshot-fault",
                                mode,
                                "--metadata-fault",
                                "partial",
                                "--log-loss",
                            ],
                        )
                        for mode in ("partial", "sync", "marker")
                    ),
                    *(
                        (
                            f"multi-{mode}-recovery",
                            [
                                "--multi-workload",
                                "--snapshot-fault",
                                mode,
                                "--metadata-fault",
                                "partial",
                                "--log-loss",
                            ],
                        )
                        for mode in ("partial", "sync", "marker")
                    ),
                    *(
                        (
                            f"pipeline-{mode}-recovery",
                            [
                                "--pipelined-workload",
                                "--snapshot-fault",
                                mode,
                                "--metadata-fault",
                                "partial",
                                "--log-loss",
                            ],
                        )
                        for mode in ("partial", "sync", "marker")
                    ),
                ):
                    name = f"seed-{seed}-{scenario}"
                    scenario_schedule = output / f"{name}.schedule"
                    options = [
                        "--virtual-time",
                        "--seed",
                        str(seed),
                        *scenario_options,
                    ]
                    recorded = run(
                        name, [*options, "--record-schedule", str(scenario_schedule)]
                    )
                    recorded_logs = log_hashes()
                    recorded_metadata = metadata_hashes()
                    recorded_snapshot = snapshot_hash()
                    replayed = run(
                        f"{name}-replay",
                        [
                            *options,
                            "--host-delay",
                            "--replay-schedule",
                            str(scenario_schedule),
                        ],
                    )
                    for text in (recorded, replayed):
                        expected_restarts = 2 if "--log-loss" in scenario_options else 1
                        expected_rewrites = (
                            expected_restarts
                            if "--metadata-write-failure" in scenario_options
                            or "--metadata-fault" in scenario_options
                            else 0
                        )
                        if (
                            text.count("keeper-metadata-rewrite-interrupted 3")
                            != expected_rewrites
                        ):
                            raise RuntimeError(
                                f"{name}: metadata rewrite fault missing"
                            )
                        if (
                            text.count("keeper-metadata-recovered-exact 3 ")
                            != expected_restarts
                        ):
                            raise RuntimeError(f"{name}: metadata recovery missing")
                        expected_snapshot_failures = int(
                            "--snapshot-write-failure" in scenario_options
                            or "--snapshot-fault" in scenario_options
                        )
                        if (
                            text.count("keeper-snapshot-failed-write-cleaned 3")
                            != expected_snapshot_failures
                        ):
                            raise RuntimeError(
                                f"{name}: snapshot cleanup evidence missing"
                            )
                        if (
                            "--metadata-fault" in scenario_options
                            and text.count("keeper-metadata-disk-fault partial 13")
                            != expected_rewrites
                        ):
                            raise RuntimeError(
                                f"{name}: actual metadata prefix evidence missing"
                            )
                        if "--snapshot-fault" in scenario_options:
                            mode = scenario_options[
                                scenario_options.index("--snapshot-fault") + 1
                            ]
                            if text.count(f"keeper-snapshot-disk-fault {mode} ") != 1:
                                raise RuntimeError(
                                    f"{name}: snapshot fault boundary evidence missing"
                                )
                    if (
                        trace(recorded) != trace(replayed)
                        or log_hashes() != recorded_logs
                        or metadata_hashes() != recorded_metadata
                        or snapshot_hash() != recorded_snapshot
                    ):
                        raise RuntimeError(
                            f"{name}: persistence recovery replay diverged"
                        )
                    (output / f"{name}.trace").write_text(trace(recorded))
                    (output / f"{name}-replay.trace").write_text(trace(replayed))
                    if seed == control_seed and scenario == "generated":
                        checker_controls(recorded)
                    if seed == control_seed and scenario == "multi":
                        checker_controls(recorded, kind="multi")
                    if seed == control_seed and scenario == "pipeline":
                        checker_controls(recorded, kind="pipeline")
                    if seed == control_seed and scenario == "pending-restart":
                        checker_controls(recorded, kind="pending")
                    if seed == control_seed and scenario == "pending-leader-failover":
                        checker_controls(recorded, kind="leader")
                    if seed == control_seed and scenario == "pending-leader-partition":
                        checker_controls(recorded, kind="partition")
                    if seed == control_seed and scenario == "partition-batch-7":
                        checker_controls(recorded, kind="partition-last")
                    if seed == control_seed and scenario == "leader-batch-7":
                        checker_controls(recorded, kind="leader-last")
                    if seed == control_seed and scenario == "prefix-batch-0":
                        checker_controls(recorded, kind="prefix")
                    if seed == control_seed and scenario == "prefix-batch-7":
                        checker_controls(recorded, kind="prefix-last")
                    if seed == control_seed and scenario == "live-prefix-batch-0":
                        checker_controls(recorded, kind="live-prefix")
                    if seed == control_seed and scenario == "live-prefix-batch-7":
                        checker_controls(recorded, kind="live-prefix-last")
            hashes.append(hashlib.sha256(expected.encode()).hexdigest())
            decisions = "\n".join(
                line
                for line in expected.splitlines()
                if line.split()[2].startswith(
                    ("request-", "response-", "append", "commit")
                )
            )
            decision_hashes.append(hashlib.sha256(decisions.encode()).hexdigest())
        if len(set(hashes)) != len(hashes):
            raise RuntimeError("Distinct seeds did not produce distinct traces")
        if len(set(decision_hashes)) != len(decision_hashes):
            raise RuntimeError(
                "Distinct seeds did not vary the executed event schedule"
            )
        original = (output / f"seed-{control_seed}.schedule").read_text().splitlines()
        if real_logs:
            for option in ("--metadata-write-failure", "--snapshot-write-failure"):
                run(
                    f"replay-reject-{option[2:]}",
                    [
                        "--virtual-time",
                        "--seed",
                        str(control_seed),
                        option,
                        "--replay-schedule",
                        str(output / f"seed-{control_seed}.schedule"),
                    ],
                    expected_failure="Replay header does not match seed/version/scenario",
                )
            for label, options, schedule in (
                (
                    "fault-mode",
                    ["--snapshot-fault", "partial"],
                    f"seed-{control_seed}-snapshot-sync.schedule",
                ),
                (
                    "generated",
                    ["--generated-workload"],
                    f"seed-{control_seed}.schedule",
                ),
                (
                    "multi",
                    ["--multi-workload"],
                    f"seed-{control_seed}-generated.schedule",
                ),
                (
                    "pipeline",
                    ["--pipelined-workload"],
                    f"seed-{control_seed}-multi.schedule",
                ),
                (
                    "pipeline-fault",
                    ["--pipeline-fault", "restart"],
                    f"seed-{control_seed}-pending-reply-loss.schedule",
                ),
                (
                    "leader-fault",
                    ["--pipeline-fault", "leader-failover"],
                    f"seed-{control_seed}-pending-restart.schedule",
                ),
                (
                    "partition-fault",
                    ["--pipeline-fault", "leader-partition"],
                    f"seed-{control_seed}-pending-leader-failover.schedule",
                ),
                (
                    "partition-batch",
                    [
                        "--pipeline-fault",
                        "leader-partition",
                        "--pipeline-fault-batch",
                        "1",
                    ],
                    f"seed-{control_seed}-pending-leader-partition.schedule",
                ),
                (
                    "leader-batch",
                    [
                        "--pipeline-fault",
                        "leader-failover",
                        "--pipeline-fault-batch",
                        "1",
                    ],
                    f"seed-{control_seed}-pending-leader-failover.schedule",
                ),
                (
                    "prefix-fault",
                    ["--pipeline-fault", "leader-prefix"],
                    f"seed-{control_seed}-pending-leader-failover.schedule",
                ),
                (
                    "live-prefix-fault",
                    ["--pipeline-fault", "leader-prefix-partition"],
                    f"seed-{control_seed}-prefix-batch-0.schedule",
                ),
            ):
                run(
                    f"replay-reject-{label}",
                    [
                        "--virtual-time",
                        "--seed",
                        str(control_seed),
                        *options,
                        "--replay-schedule",
                        str(output / schedule),
                    ],
                    expected_failure="Replay header does not match seed/version/scenario",
                )
        version = original[0].split()[0]
        invalid_cases = {
            "ineligible": (
                [original[0], "999999 invalid 1 2"],
                "Replay choice is not eligible",
            ),
            "truncated": ([original[0]], "Replay schedule exhausted or malformed"),
            "trailing": (
                original + ["999999 unused 1 2"],
                "Replay schedule has unused choices",
            ),
            "wrong-seed": (
                [f"{version} {(control_seed + 1) % 4294967296} 0 0", *original[1:]],
                "Replay header does not match seed/version/scenario",
            ),
            "wrong-scenario": (
                [f"{version} {control_seed} 1 0", *original[1:]],
                "Replay header does not match seed/version/scenario",
            ),
        }
        for name, (lines, failure) in invalid_cases.items():
            path = output / f"invalid-{name}.schedule"
            path.write_text("\n".join(lines) + "\n")
            run(
                f"replay-reject-{name}",
                [
                    "--virtual-time",
                    "--seed",
                    str(control_seed),
                    "--replay-schedule",
                    str(path),
                ],
                expected_failure=failure,
            )
        manifest["trace_sha256_by_seed"] = dict(zip(manifest["seeds"], hashes))
        manifest["recovery_trace_sha256_by_seed"] = recovery_hashes
        manifest["verdict"] = (
            "pass-bounded-keeper-repeatability"
            if keeper_backend
            else "pass-bounded-core-repeatability"
        )
        save()
        print(
            f"PASS {len(manifest['runs'])} runs, including expected-failure control; "
            "full ClickHouse deterministic gate remains blocked"
        )
    except BaseException:
        manifest["verdict"] = "failed"
        save()
        raise


if __name__ == "__main__":
    main()
