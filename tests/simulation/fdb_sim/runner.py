#!/usr/bin/env python3
"""FoundationDB Joshua-style multi-core swarm runner for ClickHouse Keeper DST (`keeper_fdb_sim`).

Runs deterministic simulation seeds in parallel across CPU cores, verifies cross-run
determinism under host-delay perturbation, runs instrumentation and negative controls,
aggregates FoundationDB-style SOMETIMES coverage counters, and produces a structured
JSON campaign report with exact one-line reproduction commands.
"""

import argparse
from concurrent.futures import ThreadPoolExecutor, as_completed
import json
from pathlib import Path
import shutil
import subprocess
import sys
import time


DEFAULT_FULL_SUITE = [
    {"name": "mem-balanced-3n", "profile": "balanced", "nodes": 3, "storage": "mem", "seeds": [1, 2, 3, 4, 5], "duration_ms": 18000},
    {"name": "mem-chaos-3n", "profile": "chaos", "nodes": 3, "storage": "mem", "seeds": [11, 12, 13, 14, 15], "duration_ms": 20000},
    {"name": "mem-power-loss-3n", "profile": "power-loss", "nodes": 3, "storage": "mem", "seeds": [21, 22, 23, 24], "duration_ms": 20000},
    {"name": "mem-snapshot-storm-3n", "profile": "snapshot-storm", "nodes": 3, "storage": "mem", "seeds": [31, 32, 33, 34], "duration_ms": 20000},
    {"name": "mem-partition-5n", "profile": "partition-storm", "nodes": 5, "storage": "mem", "seeds": [41, 42, 43], "duration_ms": 20000},
    {"name": "lsmt-mem-chaos-3n", "profile": "chaos", "nodes": 3, "storage": "lsmt-mem", "seeds": [51, 52, 53], "duration_ms": 18000},
    {"name": "lsmt-disk-chaos-3n", "profile": "chaos", "nodes": 3, "storage": "lsmt-disk", "seeds": [61, 62, 63], "duration_ms": 18000},
]


def build_command(binary, state_dir, spec, seed, host_delay=False, strict_raft=False, unsafe_quorum=False, trace_file=None):
    cmd = [
        str(binary),
        "--seed", str(seed),
        "--nodes", str(spec["nodes"]),
        "--profile", spec["profile"],
        "--duration-ms", str(spec["duration_ms"]),
        "--state-dir", str(state_dir),
        "--json-summary",
    ]
    if spec.get("storage") == "lsmt-mem":
        cmd.append("--lsmt")
    elif spec.get("storage") == "lsmt-disk":
        cmd.append("--lsmt-disk")
    if host_delay:
        cmd.append("--host-delay")
    if strict_raft:
        cmd.append("--strict-raft-commit")
    if unsafe_quorum:
        cmd.append("--unsafe-quorum-control")
    if trace_file is not None:
        cmd.extend(["--trace-file", str(trace_file)])
    return cmd


def run_single(binary, work_dir, spec, seed, timeout_s=120, host_delay=False, strict_raft=False, keep_state=False):
    run_id = f"{spec['name']}_seed_{seed}{'_delay' if host_delay else ''}"
    state_dir = work_dir / f"state_{run_id}"
    cmd = build_command(binary, state_dir, spec, seed, host_delay=host_delay, strict_raft=strict_raft)
    t0 = time.monotonic()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout_s)
        elapsed = time.monotonic() - t0
    except subprocess.TimeoutExpired:
        if not keep_state and state_dir.exists():
            shutil.rmtree(state_dir, ignore_errors=True)
        return {
            "run_id": run_id,
            "suite": spec["name"],
            "seed": seed,
            "ok": False,
            "error": f"TIMEOUT after {timeout_s}s",
            "command": " ".join(cmd),
            "elapsed_s": round(time.monotonic() - t0, 3),
        }

    if not keep_state and state_dir.exists():
        shutil.rmtree(state_dir, ignore_errors=True)

    result = {
        "run_id": run_id,
        "suite": spec["name"],
        "seed": seed,
        "host_delay": host_delay,
        "exit_code": proc.returncode,
        "elapsed_s": round(elapsed, 3),
        "command": " ".join(cmd),
    }
    if proc.returncode != 0:
        result["ok"] = False
        result["error"] = (proc.stderr.strip() or proc.stdout.strip() or f"exit code {proc.returncode}")
        return result

    try:
        lines = [line for line in proc.stdout.splitlines() if line.strip().startswith("{")]
        summary = json.loads(lines[-1])
        result["ok"] = summary.get("verdict") == "PASS"
        result["summary"] = summary
    except Exception as exc:
        result["ok"] = False
        result["error"] = f"Failed to parse JSON summary: {exc}; stdout={proc.stdout[-300:]}"
    return result


def run_instrumentation_control(binary):
    proc = subprocess.run([str(binary), "--instrumentation-control"], capture_output=True, text=True, timeout=30)
    ok = proc.returncode == 0 and "PASS fdb-sim-instrumentation-control" in proc.stdout
    return {
        "name": "instrumentation-control",
        "ok": ok,
        "exit_code": proc.returncode,
        "output": (proc.stdout + proc.stderr).strip(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True, help="Path to keeper_fdb_sim executable")
    parser.add_argument("--output", type=Path, required=True, help="Output directory for campaign report")
    parser.add_argument("--suite", choices=["quick", "full", "custom"], default="full")
    parser.add_argument("--profile", default="chaos")
    parser.add_argument("--nodes", type=int, default=3)
    parser.add_argument("--storage", choices=["mem", "lsmt-mem", "lsmt-disk"], default="mem")
    parser.add_argument("--seeds", type=int, nargs="+", default=[1, 2, 3, 4, 5])
    parser.add_argument("--duration-ms", type=int, default=20000)
    parser.add_argument("--workers", type=int, default=4)
    parser.add_argument("--strict-raft-commit", action="store_true")
    parser.add_argument("--determinism-checks", type=int, default=3, help="Number of passing runs to re-verify with --host-delay")
    parser.add_argument("--timeout", type=int, default=120)
    args = parser.parse_args()

    binary = args.binary.resolve(strict=True)
    output = args.output.resolve()
    output.mkdir(parents=True, exist_ok=True)
    work_dir = output / "work"
    work_dir.mkdir(parents=True, exist_ok=True)

    # 1. Instrumentation control
    instr = run_instrumentation_control(binary)
    if not instr["ok"]:
        print(f"FAIL instrumentation control: {instr['output']}", file=sys.stderr)
        return 1

    # 2. Build job list
    if args.suite == "full":
        specs = DEFAULT_FULL_SUITE
    elif args.suite == "quick":
        specs = [
            {"name": "mem-chaos-3n", "profile": "chaos", "nodes": 3, "storage": "mem", "seeds": [1, 2], "duration_ms": 12000},
            {"name": "mem-snapshot-storm-3n", "profile": "snapshot-storm", "nodes": 3, "storage": "mem", "seeds": [31], "duration_ms": 12000},
            {"name": "mem-partition-5n", "profile": "partition-storm", "nodes": 5, "storage": "mem", "seeds": [41], "duration_ms": 12000},
            {"name": "lsmt-disk-chaos-3n", "profile": "chaos", "nodes": 3, "storage": "lsmt-disk", "seeds": [61], "duration_ms": 12000},
        ]
    else:
        specs = [{
            "name": f"{args.storage}-{args.profile}-{args.nodes}n",
            "profile": args.profile,
            "nodes": args.nodes,
            "storage": args.storage,
            "seeds": args.seeds,
            "duration_ms": args.duration_ms,
        }]

    jobs = [(spec, seed) for spec in specs for seed in spec["seeds"]]
    results = []
    t_start = time.monotonic()

    with ThreadPoolExecutor(max_workers=max(1, args.workers)) as pool:
        future_map = {
            pool.submit(run_single, binary, work_dir, spec, seed, args.timeout, False, args.strict_raft_commit): (spec, seed)
            for spec, seed in jobs
        }
        for fut in as_completed(future_map):
            res = fut.result()
            results.append(res)
            status = "PASS" if res["ok"] else "FAIL"
            extra = ""
            if res["ok"]:
                s = res["summary"]
                extra = f"events={s['executed_events']} commits={s['oracle']['model_commits']} hash={s['trace_hash']}"
            else:
                extra = res.get("error", "")[:120]
            print(f"[{status}] {res['run_id']} ({res['elapsed_s']}s) {extra}", flush=True)

    results.sort(key=lambda r: r["run_id"])
    failures = [r for r in results if not r["ok"]]

    # 3. Determinism verification: re-run passing seeds with --host-delay and assert identical trace_hash & final_digest
    determinism_checks = []
    passing_pairs = []
    seen_suites = set()
    for spec, seed in jobs:
        matching = next((r for r in results if r["suite"] == spec["name"] and r["seed"] == seed and r["ok"]), None)
        if matching and spec["name"] not in seen_suites and len(passing_pairs) < args.determinism_checks:
            seen_suites.add(spec["name"])
            passing_pairs.append((spec, seed, matching))

    for spec, seed, orig in passing_pairs:
        replay = run_single(binary, work_dir, spec, seed, args.timeout, host_delay=True, strict_raft=args.strict_raft_commit)
        same = (
            replay["ok"]
            and replay["summary"]["trace_hash"] == orig["summary"]["trace_hash"]
            and replay["summary"]["executed_events"] == orig["summary"]["executed_events"]
            and replay["summary"]["final_commit_index"] == orig["summary"]["final_commit_index"]
            and replay["summary"]["final_digest"] == orig["summary"]["final_digest"]
        )
        check = {
            "run_id": orig["run_id"],
            "ok": same,
            "orig_hash": orig["summary"]["trace_hash"],
            "replay_hash": replay["summary"]["trace_hash"] if replay["ok"] else None,
            "orig_events": orig["summary"]["executed_events"],
            "replay_events": replay["summary"]["executed_events"] if replay["ok"] else None,
        }
        determinism_checks.append(check)
        print(
            f"[{'PASS' if same else 'FAIL'}] determinism {orig['run_id']} "
            f"orig={check['orig_hash']} replay={check['replay_hash']}",
            flush=True,
        )

    # 4. Aggregate FDB SOMETIMES coverage across all passing runs
    coverage = {}
    totals = {
        "executed_events": 0,
        "model_commits": 0,
        "replica_responses_checked": 0,
        "digests_checked": 0,
        "deep_tree_checks": 0,
        "raft_state_checks": 0,
        "power_losses": 0,
        "snapshot_rpcs": 0,
        "watches_delivered": 0,
        "raft_figure8_violations": 0,
    }
    for r in results:
        if not r["ok"]:
            continue
        s = r["summary"]
        totals["executed_events"] += s["executed_events"]
        totals["model_commits"] += s["oracle"]["model_commits"]
        totals["replica_responses_checked"] += s["oracle"]["replica_responses_checked"]
        totals["digests_checked"] += s["oracle"]["digests_checked"]
        totals["deep_tree_checks"] += s["oracle"]["deep_tree_checks"]
        totals["raft_state_checks"] += s["oracle"]["raft_state_checks"]
        totals["power_losses"] += s["disk"]["power_losses"]
        totals["snapshot_rpcs"] += s["network"]["snapshot_rpcs"]
        totals["watches_delivered"] += s["oracle"]["watches_delivered"]
        totals["raft_figure8_violations"] += s["oracle"]["raft_figure8_violations"]
        for k, v in s.get("coverage", {}).items():
            coverage[k] = coverage.get(k, 0) + v

    det_failures = [d for d in determinism_checks if not d["ok"]]
    all_ok = not failures and not det_failures

    report = {
        "verdict": "PASS" if all_ok else "FAIL",
        "elapsed_s": round(time.monotonic() - t_start, 3),
        "instrumentation_control": instr,
        "total_runs": len(results),
        "passed_runs": len(results) - len(failures),
        "failed_runs": len(failures),
        "determinism_checks": determinism_checks,
        "totals": totals,
        "coverage": dict(sorted(coverage.items())),
        "runs": results,
    }
    report_path = output / "campaign_report.json"
    report_path.write_text(json.dumps(report, indent=2) + "\n")

    print("\n=== FDB-Style Simulation Campaign Summary ===")
    print(f"Verdict: {report['verdict']} ({report['passed_runs']}/{report['total_runs']} runs passed, "
          f"{len(determinism_checks) - len(det_failures)}/{len(determinism_checks)} determinism checks passed, "
          f"{report['elapsed_s']}s)")
    print(f"Totals: {json.dumps(totals)}")
    print("SOMETIMES Coverage:")
    for k, v in sorted(coverage.items()):
        print(f"  {k:36s}: {v}")
    print(f"Report: {report_path}")

    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
