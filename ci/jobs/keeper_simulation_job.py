import argparse
import os
import sys
import xml.etree.ElementTree as ET
from pathlib import Path

from ci.praktika.result import Result


def parse_junit_results(junit_path: Path):
    if not junit_path.exists():
        return []
    tree = ET.parse(junit_path)
    root = tree.getroot()
    subresults = []
    for tc in root.findall(".//testcase"):
        name = tc.attrib.get("name", "unknown")
        duration = float(tc.attrib.get("time", "0.0") or "0.0")
        failure = tc.find("failure")
        if failure is None:
            subresults.append(Result.create_from(name=name, status=Result.Status.OK, duration=duration))
        else:
            msg = failure.attrib.get("message", "") or (failure.text or "")
            subresults.append(
                Result.create_from(
                    name=name,
                    status=Result.Status.FAIL,
                    duration=duration,
                    info=msg.strip(),
                )
            )
    return subresults


def main():
    parser = argparse.ArgumentParser(description="Run deterministic ClickHouse Keeper simulation CI suite")
    parser.add_argument("--binary", default="./ci/tmp/keeper_sim", help="Path to keeper_sim binary")
    parser.add_argument("--suite", default="ci", help="Simulation swarm suite (default: ci)")
    parser.add_argument("--workers", type=int, default=0, help="Parallel worker count (0 = auto)")
    parser.add_argument("--seed-offset", type=int, default=0, help="Optional seed offset for deterministic rotation")
    parser.add_argument("--output", default="./ci/tmp/keeper_sim_ci", help="Output directory for logs and JUnit XML")
    args = parser.parse_args()

    os.environ["OPENSSL_CONF"] = "/dev/null"

    binary = Path(args.binary)
    if not binary.exists():
        fallback = Path("./build_keeper_sim/src/keeper_sim")
        if fallback.exists():
            binary = fallback

    out_dir = Path(args.output)
    out_dir.mkdir(parents=True, exist_ok=True)
    junit_path = out_dir / "junit.xml"

    cmd = (
        f"{binary} --swarm --suite {args.suite}"
        f" --workers {args.workers}"
        f" --seed-offset {args.seed_offset}"
        f" --output {out_dir}"
        f" --junit-xml {junit_path}"
    )

    R = Result.from_commands_run(
        name=f"Keeper Simulation ({args.suite})",
        command=cmd,
        with_log=True,
        with_info=True,
    )

    subresults = parse_junit_results(junit_path)
    if subresults:
        R.results = subresults

    R.complete_job()
    sys.exit(0 if R.is_ok() else 1)


if __name__ == "__main__":
    main()
