"""Independent safety checks for complete, fixed-membership, in-memory Raft histories.

Consumes RAFT JSON records, not the probe's PASS/FAIL verdict or TRACE assertions.
Version 1 requires five voters, logs starting at index 1, no restarts, snapshots,
membership changes, asynchronous durability, or missing observations. This is a
bounded safety oracle, not a liveness proof or a production durability model.
"""

import argparse
import json
from pathlib import Path


class RaftViolation(ValueError):
    def __init__(self, rule, location, detail):
        self.rule = rule
        super().__init__(f"Raft {rule} at {location}: {detail}")


def check_history(text):
    previous = {}
    leaders = {}
    votes = {}
    log_prefixes = {}
    committed = {}  # index -> (entry, term of the certifying leader)
    applied = {}
    acknowledgements = {}
    election_responses = {}
    response_ids = set()
    observations = 0
    last_event = 0
    advances = 0
    location = "start"

    def require(condition, rule, detail):
        if not condition:
            raise RaftViolation(rule, location, detail)

    def same_prefix(node, log, end):
        return node["durable"] >= end and node["log"][:end] == log[:end]

    for line_number, line in enumerate(text.splitlines(), 1):
        if not line.startswith("RAFT "):
            continue
        location = f"line {line_number}"
        try:
            record = json.loads(line[5:])
            event = record["event"]
            require(
                type(event) is int and event >= last_event,
                "trace",
                "event order decreased",
            )
            last_event = event
            location += f", event {event}"
            if record["kind"] == "response":
                require(previous, "trace", "response before initial state")
                rpc = record["rpc"]
                require(rpc not in response_ids, "trace", "duplicate response")
                response_ids.add(rpc)
                source, target = record["from"], record["to"]
                require(
                    source in previous and target in previous and source != target,
                    "trace",
                    "response outside voter set",
                )
                if record["type"] == 1 and record["accepted"]:
                    term = record["term"]
                    require(
                        term == record["request_term"]
                        and votes.get((source, term)) == target,
                        "persisted-vote",
                        f"node {source} granted an unpersisted vote",
                    )
                    election_responses.setdefault((target, term), set()).add(source)
                if record["type"] == 3 and record["accepted"]:
                    end = record["end"]
                    require(
                        record["next"] == end + 1,
                        "response-endpoint",
                        "acknowledgement exceeds request",
                    )
                    require(
                        previous[source]["durable"] >= end,
                        "durable-ack",
                        f"node {source} acknowledged undurable index {end}",
                    )
                    if record["term"] == record["request_term"]:
                        key = (target, record["term"], source)
                        acknowledgements[key] = max(acknowledgements.get(key, 0), end)
                continue

            require(
                record["kind"] == "state" and record["version"] == 1,
                "trace",
                "unsupported record/version",
            )
            observations += 1
            require(
                record["observation"] == observations,
                "trace",
                "missing or reordered observation",
            )
            nodes = {node["id"]: node for node in record["nodes"]}
            require(
                len(record["nodes"]) == 5 and set(nodes) == {1, 2, 3, 4, 5},
                "trace",
                "fixed five-voter membership required",
            )
            for node_id, node in nodes.items():
                require(
                    node["incarnation"] == 1, "trace", "restart outside version-1 scope"
                )
                for field in ("term", "commit", "applied", "durable"):
                    require(
                        type(node[field]) is int and node[field] >= 0,
                        "trace",
                        f"invalid {field}",
                    )
                require(
                    node["leader"] in (0, 1) and node["vote"] in (-1, 1, 2, 3, 4, 5),
                    "trace",
                    "invalid role/vote",
                )
                log = node["log"]
                for entry in log:
                    require(
                        len(entry) == 3
                        and type(entry[0]) is int
                        and 0 < entry[0] <= node["term"]
                        and entry[1] in (1, 2),
                        "trace",
                        "unsupported log entry",
                    )
                    bytes.fromhex(entry[2])
                require(
                    0
                    <= node["applied"]
                    <= node["commit"]
                    <= node["durable"]
                    <= len(log),
                    "index-bounds",
                    f"node {node_id}: applied/commit/durable/log order",
                )
                if not previous:
                    require(
                        node["term"] == 0 and not log and node["commit"] == 0,
                        "trace",
                        "history must start before the first election",
                    )
                else:
                    old = previous[node_id]
                    for field in ("term", "commit", "applied"):
                        require(
                            node[field] >= old[field],
                            f"{field}-monotonicity",
                            f"node {node_id}: {old[field]} -> {node[field]}",
                        )
                    require(
                        log[: old["commit"]] == old["log"][: old["commit"]],
                        "committed-prefix",
                        f"node {node_id} replaced a committed entry",
                    )
                    if old["leader"] and node["leader"] and old["term"] == node["term"]:
                        require(
                            log[: len(old["log"])] == old["log"],
                            "leader-append-only",
                            f"node {node_id} changed its own log",
                        )
                key = (node_id, node["term"])
                if key in votes:
                    require(
                        node["vote"] == votes[key],
                        "one-vote-per-term",
                        f"node {node_id} changed its vote",
                    )
                if node["vote"] != -1:
                    votes[key] = node["vote"]
                if node["leader"]:
                    term = node["term"]
                    require(
                        leaders.get(term, node_id) == node_id,
                        "election-safety",
                        f"two leaders in term {term}",
                    )
                    if term not in leaders:
                        granted = int(node["vote"] == node_id) + len(
                            election_responses.get((node_id, term), set())
                        )
                        require(
                            granted >= 3,
                            "election-quorum",
                            f"leader {node_id} has {granted} votes",
                        )
                        for index, (entry, commit_term) in committed.items():
                            if term > commit_term:
                                require(
                                    len(log) >= index and log[index - 1] == entry,
                                    "leader-completeness",
                                    f"leader {node_id} missing committed index {index}",
                                )
                    leaders[term] = node_id

            # Same index and term imply identical entries and identical prefixes,
            # including entries observed earlier that have since been overwritten.
            for node in nodes.values():
                for index, entry in enumerate(node["log"], 1):
                    key = (index, entry[0])
                    prefix = node["log"][:index]
                    require(
                        key not in log_prefixes or log_prefixes[key] == prefix,
                        "log-matching",
                        f"different prefixes at index {index}, term {entry[0]}",
                    )
                    log_prefixes[key] = prefix

            # Infer actual commitment independently of commitIndex: a current-term
            # entry on a durable majority commits its prefix, even before replies.
            for node in nodes.values():
                if not node["leader"]:
                    continue
                for end in range(1, node["durable"] + 1):
                    if node["log"][end - 1][0] != node["term"]:
                        continue
                    copies = sum(
                        same_prefix(peer, node["log"], end) for peer in nodes.values()
                    )
                    if copies >= 3:
                        for index in range(1, end + 1):
                            entry = node["log"][index - 1]
                            require(
                                index not in committed or committed[index][0] == entry,
                                "commit-agreement",
                                f"different committed entries at index {index}",
                            )
                            committed.setdefault(index, (entry, node["term"]))

            for node_id, node in nodes.items():
                end = node["commit"]
                old_commit = previous[node_id]["commit"] if previous else 0
                if end > old_commit:
                    advances += 1
                    if node["leader"]:
                        entry_term = node["log"][end - 1][0]
                        require(
                            entry_term == node["term"] or end in committed,
                            "current-term-commit",
                            f"leader {node_id} term {node['term']} advanced {old_commit}->{end} "
                            f"with endpoint term {entry_term}, no earlier current-term quorum certificate",
                        )
                        if entry_term == node["term"]:
                            acks = 1 + sum(
                                acknowledgements.get((node_id, node["term"], peer), 0)
                                >= end
                                for peer in nodes
                                if peer != node_id
                            )
                            require(
                                acks >= 3,
                                "commit-quorum",
                                f"leader {node_id} has only {acks} acknowledgements",
                            )
                    require(
                        end in committed and node["log"][end - 1] == committed[end][0],
                        "commit-certificate",
                        f"node {node_id} committed index {end} without a durable certificate",
                    )
                expected_values = []
                for index, entry in enumerate(node["log"][: node["applied"]], 1):
                    require(
                        index not in applied or applied[index] == entry,
                        "state-machine-safety",
                        f"different applied entries at index {index}",
                    )
                    applied[index] = entry
                    if entry[1] == 1:
                        data = bytes.fromhex(entry[2])
                        require(
                            len(data) == 4, "trace", "non-int32 command outside model"
                        )
                        expected_values.append(
                            int.from_bytes(data, "little", signed=True)
                        )
                require(
                    node["values"] == expected_values,
                    "applied-values",
                    f"node {node_id} state differs from applied log",
                )
            previous = nodes
        except (KeyError, TypeError, json.JSONDecodeError) as error:
            raise RaftViolation(
                "trace", location, f"malformed record: {error}"
            ) from error

    require(observations > 1, "trace", "missing protocol history")
    return {
        "observations": observations,
        "leaders": leaders,
        "commit_advances": advances,
        "certified_indices": sorted(committed),
        "responses": len(response_ids),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("history", type=Path)
    args = parser.parse_args()
    try:
        print(json.dumps(check_history(args.history.read_text()), sort_keys=True))
    except ValueError as error:
        parser.exit(1, f"FAIL {error}\n")


if __name__ == "__main__":
    main()
