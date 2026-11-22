"""Focused Figure 8 execution, replay, provenance, and corrupted-history controls."""

import copy
import hashlib
import json
import subprocess
from pathlib import Path

from raft_model import RaftViolation, check_history


def checker_controls(control, figure8, output):
    """Mutations are fabricated checker tests, never reported as Raft executions."""
    results = []

    def reject(name, text, rule):
        path = output / f"corrupted-{name}.log"
        path.write_text(text)
        try:
            check_history(text)
        except RaftViolation as error:
            if error.rule != rule:
                raise RuntimeError(f"{name}: expected {rule}, got {error}") from error
            results.append(
                {"name": name, "log": path.name, "rule": rule, "error": str(error)}
            )
        else:
            raise RuntimeError(f"{name}: corrupted history accepted")

    def states(text):
        return [
            (index, json.loads(line[5:]))
            for index, line in enumerate(text.splitlines())
            if line.startswith('RAFT {"kind":"state"')
        ]

    def change(text, line_index, edit):
        lines = text.splitlines()
        record = json.loads(lines[line_index][5:])
        edit(record)
        lines[line_index] = "RAFT " + json.dumps(record, separators=(",", ":"))
        return "\n".join(lines) + "\n"

    observed = states(control)
    last = observed[-1][0]
    before_elected = next(
        index
        for index, record in reversed(observed)
        if record["nodes"][0]["term"] == 1 and not record["nodes"][0]["log"]
    )
    reject(
        "term-decrease",
        change(
            control, before_elected, lambda record: record["nodes"][0].update(term=0)
        ),
        "term-monotonicity",
    )
    reject(
        "commit-decrease",
        change(
            control,
            last,
            lambda record: record["nodes"][1].update(commit=1, applied=1, values=[]),
        ),
        "commit-monotonicity",
    )
    reject(
        "apply-decrease",
        change(
            control,
            last,
            lambda record: record["nodes"][1].update(applied=1, values=[]),
        ),
        "applied-monotonicity",
    )
    reject(
        "two-leaders",
        change(control, last, lambda record: record["nodes"][1].update(leader=1)),
        "election-safety",
    )
    if observed[-1][1]["nodes"][3]["vote"] != 1:
        raise RuntimeError("Double-vote control requires a recorded first vote")
    reject(
        "double-vote",
        change(control, last, lambda record: record["nodes"][3].update(vote=3)),
        "one-vote-per-term",
    )
    reject(
        "applied-values",
        change(control, last, lambda record: record["nodes"][1].update(values=[-47])),
        "applied-values",
    )

    first_copy = next(
        index for index, record in observed if len(record["nodes"][1]["log"]) == 2
    )

    def alter_payload(record):
        record["nodes"][1]["log"][1][2] = "d1ffffff"

    reject("log-matching", change(control, first_copy, alter_payload), "log-matching")
    reject("committed-prefix", change(control, last, alter_payload), "committed-prefix")

    lines = control.splitlines()
    del lines[observed[2][0]]
    reject("missing-observation", "\n".join(lines) + "\n", "trace")
    reject("missing-protocol", "PASS invented success\n", "trace")
    for message_type, rule in ((1, "election-quorum"), (3, "commit-quorum")):
        lines = [
            line
            for line in control.splitlines()
            if not (
                line.startswith('RAFT {"kind":"response"')
                and json.loads(line[5:])["type"] == message_type
            )
        ]
        reject(f"missing-{rule}", "\n".join(lines) + "\n", rule)
    response_line = next(
        index
        for index, line in enumerate(control.splitlines())
        if line.startswith('RAFT {"kind":"response"')
        and json.loads(line[5:])["type"] == 3
        and json.loads(line[5:])["accepted"]
    )
    reject(
        "response-endpoint",
        change(
            control,
            response_line,
            lambda record: record.update(next=record["next"] + 1),
        ),
        "response-endpoint",
    )
    reject(
        "undurable-ack",
        change(control, response_line, lambda record: record.update(end=100, next=101)),
        "durable-ack",
    )

    # The executed returning-election prefix is safe. Inject a premature commit
    # before any catch-up response; this must fail independently of the real bug.
    returning = [
        (index, record)
        for index, record in states(figure8)
        if record["phase"] == "returning-election" and record["nodes"][0]["leader"]
    ]
    index, record = returning[-1]
    prefix = "\n".join(figure8.splitlines()[: index + 1]) + "\n"
    check_history(prefix)
    reject(
        "old-term-commit",
        change(prefix, index, lambda record: record["nodes"][0].update(commit=2)),
        "current-term-commit",
    )

    # Build a deliberately impossible election from an actual state where x is
    # already on a current-term majority, but a lagging voter has not received it.
    # This tests leader completeness even before the old leader learns commitment.
    index, record = next(
        (index, record)
        for index, record in observed
        if sum(node["durable"] >= 2 for node in record["nodes"]) >= 3
        and any(node["durable"] == 1 for node in record["nodes"])
    )
    prefix = "\n".join(control.splitlines()[: index + 1]) + "\n"
    check_history(prefix)
    record = copy.deepcopy(record)
    candidate = next(node["id"] for node in record["nodes"] if node["durable"] == 1)
    for node in record["nodes"]:
        node.update(term=2, vote=candidate, leader=0)
    record["event"] += 1
    record["observation"] += 1
    prefix += "RAFT " + json.dumps(record) + "\n"
    voters = [node["id"] for node in record["nodes"] if node["id"] != candidate][:2]
    for voter in voters:
        prefix += (
            "RAFT "
            + json.dumps(
                {
                    "kind": "response",
                    "event": record["event"],
                    "rpc": 10000 + voter,
                    "type": 1,
                    "from": voter,
                    "to": candidate,
                    "request_term": 2,
                    "term": 2,
                    "accepted": 1,
                    "end": 1,
                    "next": 2,
                }
            )
            + "\n"
        )
    record["nodes"][candidate - 1]["leader"] = 1
    record["observation"] += 1
    prefix += "RAFT " + json.dumps(record) + "\n"
    reject("leader-completeness", prefix, "leader-completeness")
    return results


def verify(binary, output, repeats):
    binary = binary.resolve(strict=True)
    output = output.resolve()
    output.mkdir(parents=True, exist_ok=False)
    root = Path(__file__).resolve().parents[3]
    sources = [
        *sorted((root / "contrib/NuRaft/src").glob("*.cxx")),
        *sorted((root / "contrib/NuRaft/include").rglob("*.hxx")),
        *sorted((root / "contrib/NuRaft/examples").glob("in_memory*")),
    ]
    sources += [
        root / name
        for name in (
            "tests/simulation/nuraft/probe.cpp",
            "tests/simulation/nuraft/figure8.cpp",
            "tests/simulation/nuraft/CMakeLists.txt",
            "tests/simulation/nuraft/raft_model.py",
            "tests/simulation/nuraft/verify_raft.py",
            "tests/simulation/nuraft/verify.py",
            "src/Coordination/SessionExpiryQueue.cpp",
            "src/Coordination/SessionExpiryQueue.h",
        )
    ]
    manifest = {
        "backend": "figure8",
        "binary": str(binary),
        "binary_sha256": hashlib.sha256(binary.read_bytes()).hexdigest(),
        "checkout": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=root, text=True
        ).strip(),
        "nuraft": subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=root / "contrib/NuRaft", text=True
        ).strip(),
        "nuraft_worktree": subprocess.check_output(
            ["git", "status", "--porcelain"], cwd=root / "contrib/NuRaft", text=True
        ),
        "source_sha256": {
            str(path.relative_to(root)): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in sources
        },
        "provenance_note": "Source hashes are verification-time observations; preserve build logs and commands separately.",
        "seed": 1,
        "repeats": repeats,
        "runs": [],
        "checker_controls": [],
        "verdict": "running",
    }

    def save():
        (output / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")

    texts = {}
    violation = False
    save()
    for scenario, options in (
        ("current-term", ["--current-term-control"]),
        ("figure8", []),
    ):
        schedule = output / f"{scenario}.schedule"
        for repeat in range(repeats):
            command = [
                str(binary),
                *options,
                "--replay" if repeat else "--record",
                str(schedule),
            ]
            log = output / f"{scenario}-{repeat}.log"
            with log.open("w") as stream:
                process = subprocess.run(
                    command, stdout=stream, stderr=subprocess.STDOUT, timeout=30
                )
            text = log.read_text()
            result = {
                "command": command,
                "log": log.name,
                "exit_code": process.returncode,
                "sha256": hashlib.sha256(log.read_bytes()).hexdigest(),
            }
            manifest["runs"].append(result)
            save()
            try:
                result["model"] = check_history(text)
                result["classification"] = "safe-bounded-history"
            except RaftViolation as error:
                result.update(
                    classification="protocol-violation",
                    rule=error.rule,
                    error=str(error),
                )
            if scenario == "current-term":
                if (
                    process.returncode
                    or "PASS current-term quorum" not in text
                    or "model" not in result
                ):
                    save()
                    raise RuntimeError(f"Positive control failed: {log}")
            else:
                if "figure8-setup-complete" not in text:
                    result["classification"] = "setup-failure"
                    save()
                    raise RuntimeError(f"Figure 8 setup incomplete: {log}")
                if result.get("rule") == "current-term-commit":
                    if process.returncode != 1 or "FAIL CONSENSUS FIGURE8" not in text:
                        raise RuntimeError(
                            f"C++ assertion and independent checker disagree: {log}"
                        )
                    violation = True
                elif (
                    process.returncode
                    or "model" not in result
                    or "PASS Figure8" not in text
                ):
                    save()
                    raise RuntimeError(f"Unexpected Figure 8 result: {log}")
            if repeat:
                if text != texts[scenario]:
                    raise RuntimeError(f"Replay changed the complete history: {log}")
                result["byte_identical_replay"] = True
            else:
                texts[scenario] = text
            save()
    manifest["checker_controls"] = checker_controls(
        texts["current-term"], texts["figure8"], output
    )
    manifest["verdict"] = (
        "consensus-rule-violation" if violation else "safe-bounded-histories"
    )
    save()
    print(
        json.dumps(
            {
                "verdict": manifest["verdict"],
                "runs": len(manifest["runs"]),
                "rejected_corruptions": len(manifest["checker_controls"]),
                "manifest": str(output / "manifest.json"),
            }
        )
    )
    # A reproduced violation is a failing regression, never an expected success.
    return not violation
