"""Logical contract shared by the reference workload and future simulation backend."""

import hashlib
from collections import Counter
from dataclasses import dataclass

BATCHES = 32
ROWS_PER_BATCH = 16
INSERT_SETTINGS = {
    "async_insert": 0,
    "deduplicate_insert": "enable",
    "insert_quorum": 2,
    "insert_quorum_parallel": 0,
    "insert_quorum_timeout": 10000,
    "max_insert_threads": 1,
}
TABLE_SETTINGS = {
    "replicated_deduplication_window": 10000,
    "replicated_deduplication_window_seconds": 86400,
    "fsync_after_insert": 1,
    "fsync_part_directory": 1,
}


def batch_rows(run_id, batch_id):
    return [
        (
            run_id,
            batch_id,
            row_id,
            hashlib.sha256(f"{run_id}:{batch_id}:{row_id}".encode()).hexdigest(),
        )
        for row_id in range(ROWS_PER_BATCH)
    ]


@dataclass(frozen=True)
class Attempt:
    batch_id: int
    replica: str
    query_id: str
    outcome: str

    def __post_init__(self):
        if not 0 <= self.batch_id < BATCHES:
            raise ValueError("Batch outside the experiment")
        if self.outcome not in ("success", "unknown"):
            raise ValueError("Only confirmed success or unknown outcomes are supported")


def check_rows(run_id, attempts, rows, *, require_all=False):
    """Return violations, preserving row multiplicities and uncertain batch outcomes.

    Whole-batch checks require the reference runner's one-block, one-partition input.
    A later failed attempt never revokes an earlier successful acknowledgement.
    """
    submitted = {attempt.batch_id for attempt in attempts}
    required = {
        attempt.batch_id for attempt in attempts if attempt.outcome == "success"
    }
    if require_all:
        required = set(range(BATCHES))
    expected = {
        batch_id: Counter(batch_rows(run_id, batch_id))
        for batch_id in submitted | required
    }
    actual = Counter(tuple(row) for row in rows)
    allowed = Counter()
    for batch in expected.values():
        allowed.update(batch)

    violations = []
    unexpected = actual - allowed
    if unexpected:
        violations.append(
            f"Unexpected, corrupted, or duplicate rows: {list(unexpected.items())[:3]}"
        )
    for batch_id, batch in sorted(expected.items()):
        present = actual & batch
        if batch_id in required and present != batch:
            violations.append(
                f"Missing rows from required batch {batch_id}: {sum((batch - present).values())}"
            )
        elif batch_id not in required and present and present != batch:
            violations.append(f"Partial unknown-outcome batch {batch_id}")
    return violations
