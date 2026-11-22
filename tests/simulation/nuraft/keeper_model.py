"""Independent sequential Keeper model; consumes inputs, never storage-derived expectations.

Only the generated workload's single session, open ACLs, non-sequential create,
versioned set/remove/check, atomic multi, get/list, and close are supported. This is not a concurrent
linearizability checker or a model of Keeper's internal transaction machinery.
"""


def check_history(text):
    nodes = {}
    paths = set()
    results = set()
    observations = set()
    phases = set()
    errors = set()
    submitted = 0
    generated = None
    closed = False
    phase = None
    current_xid = None
    expected_error = None
    is_multi = False
    transactions = 0
    expected_parts = 0
    expected_suberrors = []
    multi_results = set()
    transaction_state = None
    transaction_failed = False
    transaction_coverage = set()
    pipelines = 0
    pipeline = None
    pipeline_start_nodes = None
    pipeline_results = {}
    pipeline_fault = None
    pipeline_faults = set()
    fault_batch = 0
    aborted_pipelines = set()
    prefix_pipelines = set()
    discarded_requests = set()
    committed_clients = set()
    read_nodes = nodes

    def require(condition, message):
        if not condition:
            raise ValueError(f"Keeper model: {message}")

    def children(path, state):
        return sorted(
            key.rsplit("/", 1)[1] for key in state if key.rsplit("/", 1)[0] == path
        )

    def operation(args):
        kind, path, data, version, ephemeral = args
        require(
            path == "/generated" or path.startswith("/generated/"),
            "path outside modeled namespace",
        )
        paths.add(path)
        parent = path.rsplit("/", 1)[0]
        if parent:
            paths.add(parent)
        return kind, path, data, int(version), int(ephemeral)

    def apply(state, kind, path, data, version, ephemeral):
        parent = path.rsplit("/", 1)[0]
        if kind == "create":
            if path in state:
                return -110  # ZNODEEXISTS
            if parent and parent not in state:
                return -101  # ZNONODE
            if parent and state[parent][2]:
                return -108  # ZNOCHILDRENFOREPHEMERALS
            state[path] = (data, 0, ephemeral)
        elif kind in ("set", "remove", "check"):
            if path not in state:
                return -101
            if version != -1 and version != state[path][1]:
                return -103  # ZBADVERSION
            if kind == "remove":
                if children(path, state):
                    return -111  # ZNOTEMPTY
                del state[path]
            elif kind == "set":
                state[path] = (data, state[path][1] + 1, state[path][2])
        else:
            raise ValueError(f"Keeper model: unsupported operation {kind}")
        return 0

    for line in text.splitlines():
        if not line.startswith("TRACE "):
            continue
        fields = line.split()[2:]
        if not fields or not fields[0].startswith("workload-"):
            continue
        event, *args = fields
        if event in ("workload-submit", "workload-multi"):
            require(
                not closed and phase is None, "submission during check or after close"
            )
            if submitted and pipeline is None:
                require(results == {1, 2, 3}, "missing operation results")
                require(f"step-{current_xid}" in phases, "missing operation check")
                require(
                    not is_multi or multi_results == {1, 2, 3}, "missing multi results"
                )
            if submitted and pipeline is not None:
                require(
                    not is_multi or len(expected_suberrors) == expected_parts,
                    "missing pipelined transaction parts",
                )
            xid, *body = args
            require(int(xid) == 100 + submitted, "duplicate or missing submission")
            current_xid = xid
            submitted += 1
            results = set()
            is_multi = event == "workload-multi"
            multi_results = set()
            if is_multi:
                require(submitted > 76, "multi preceded single-operation coverage")
                (count,) = body
                expected_parts = int(count)
                require(0 <= expected_parts <= 3, "unsupported transaction size")
                transactions += 1
                expected_suberrors = []
                transaction_state = nodes.copy()
                transaction_failed = False
                # Raw state-machine responses carry operation failure in subresponses.
                expected_error = 0
                if not expected_parts:
                    transaction_coverage.add("empty")
                if pipeline is not None:
                    pipeline_results[xid] = {
                        "error": expected_error,
                        "suberrors": expected_suberrors,
                        "results": results,
                        "multi_results": multi_results,
                        "count": expected_parts,
                    }
            else:
                require(submitted <= 76, "single operation followed transaction phase")
                expected_error = apply(nodes, *operation(body))
            errors.add(expected_error)
        elif event == "workload-part":
            xid, index, *body = args
            require(
                is_multi and xid == current_xid and not results,
                "part outside current transaction",
            )
            require(
                int(index) == len(expected_suberrors) < expected_parts,
                "missing or duplicate transaction part",
            )
            op = operation(body)
            error = -2 if transaction_failed else apply(transaction_state, *op)
            if error != -2:
                errors.add(error)
            transaction_failed |= error != 0
            expected_suberrors.append(error)
            if len(expected_suberrors) == expected_parts:
                if not transaction_failed:
                    nodes = transaction_state
                    transaction_coverage.add("success")
                else:
                    failed = next(
                        i for i, code in enumerate(expected_suberrors) if code
                    )
                    transaction_coverage.add(
                        "failed-first"
                        if failed == 0
                        else (
                            "failed-last"
                            if failed == expected_parts - 1
                            else "failed-middle"
                        )
                    )
                if pipeline is not None:
                    pipeline_results[xid]["state"] = nodes.copy()
        elif event == "workload-result":
            node, xid, error = map(int, args)
            require(phase is None and not closed, "result outside operation")
            require(
                str(xid) not in discarded_requests,
                "abandoned transaction returned a result",
            )
            if pipeline is not None:
                require(str(xid) in pipeline_results, "unknown pipelined result")
                pending = pipeline_results[str(xid)]
                results = pending["results"]
                expected = pending["error"]
            else:
                require(str(xid) == current_xid, "result identity differs")
                expected = expected_error
            require(
                node in (1, 2, 3) and node not in results,
                "duplicate or unknown result node",
            )
            require(
                not is_multi or len(expected_suberrors) == expected_parts,
                "missing transaction parts",
            )
            require(
                error == expected,
                f"operation {xid} response differs: {error} != {expected}",
            )
            results.add(node)
        elif event == "workload-multi-result":
            node, xid, count, *suberrors = map(int, args)
            require(
                is_multi and phase is None and not closed,
                "multi result outside transaction",
            )
            require(
                str(xid) not in discarded_requests,
                "abandoned transaction returned subresults",
            )
            if pipeline is not None:
                require(str(xid) in pipeline_results, "unknown pipelined multi result")
                pending = pipeline_results[str(xid)]
                results, multi_results = pending["results"], pending["multi_results"]
                expected, parts = pending["suberrors"], pending["count"]
            else:
                require(str(xid) == current_xid, "multi result identity differs")
                expected, parts = expected_suberrors, expected_parts
            require(
                node in results and node not in multi_results,
                "duplicate or unknown multi result node",
            )
            require(
                count == len(suberrors) == parts and suberrors == expected,
                f"transaction {xid} subresponses differ: {suberrors} != {expected}",
            )
            multi_results.add(node)
        elif event == "workload-check":
            require(phase is None, "overlapping check phases")
            (next_phase,) = args
            if pipeline is not None:
                require(len(pipeline_results) == 4, "incomplete pipeline submissions")
                require(
                    next_phase
                    in (
                        f"pipeline-before-{pipeline}",
                        f"pipeline-after-{pipeline}",
                        f"pipeline-recovered-{pipeline}",
                    ),
                    "unexpected pipeline check",
                )
                recovered = next_phase == f"pipeline-recovered-{pipeline}"
                before = next_phase != f"pipeline-after-{pipeline}"
                if recovered:
                    require(
                        pipeline_fault == "restart"
                        and pipeline in pipeline_faults
                        and f"pipeline-before-{pipeline}" in phases,
                        "recovery check without outstanding restart",
                    )
                if before:
                    require(
                        all(
                            not item["results"] and not item["multi_results"]
                            for item in pipeline_results.values()
                        ),
                        "pipeline returned before durable quorum",
                    )
                else:
                    require(
                        f"pipeline-before-{pipeline}" in phases,
                        "missing uncommitted visibility check",
                    )
                    require(
                        all(
                            item["results"]
                            == item["multi_results"]
                            == (set() if xid in discarded_requests else {1, 2, 3})
                            for xid, item in pipeline_results.items()
                        ),
                        "missing pipelined results",
                    )
                read_nodes = pipeline_start_nodes if before else nodes
            else:
                require(
                    (
                        results == {1, 2, 3}
                        and (not is_multi or multi_results == {1, 2, 3})
                    )
                    or (
                        pipelines == 8
                        and generated == 142
                        and current_xid in discarded_requests
                        and not results
                        and not multi_results
                    ),
                    "missing results before check",
                )
                read_nodes = nodes
            phase = next_phase
            require(phase not in phases, "duplicate check phase")
            observations = {
                (kind, str(node), path)
                for kind in ("get", "list")
                for node in (1, 2, 3)
                for path in paths
            }
        elif event in ("workload-get", "workload-list"):
            kind = event.removeprefix("workload-")
            node, path, error, *values = args
            key = (kind, node, path)
            require(
                phase is not None and key in observations,
                "duplicate or unexpected observation",
            )
            observations.remove(key)
            require(
                int(error) == (0 if path in read_nodes else -101),
                f"{phase} {node} {path} existence differs",
            )
            if path not in read_nodes:
                continue
            if kind == "get":
                data, version, owner = values
                require(
                    (data, int(version), int(owner)) == read_nodes[path],
                    f"{phase} {node} {path} value/version/owner differs",
                )
            else:
                count, *actual = values
                require(
                    int(count) == len(actual) and actual == children(path, read_nodes),
                    f"{phase} {node} {path} children differ",
                )
        elif event == "workload-checked":
            require(args == [phase] and not observations, "missing read observations")
            phases.add(phase)
            phase = None
        elif event == "workload-pipeline-faults":
            require(
                pipeline_fault is None
                and pipeline is None
                and submitted == 110
                and phase is None,
                "pipeline fault declaration outside setup",
            )
            (pipeline_fault,) = args
            require(
                pipeline_fault
                in (
                    "reply-loss",
                    "restart",
                    "leader-failover",
                    "leader-partition",
                    "leader-prefix",
                    "leader-prefix-partition",
                ),
                "unknown pipeline fault",
            )
        elif event == "workload-pipeline-fault-batch":
            require(
                pipeline_fault
                in (
                    "leader-partition",
                    "leader-failover",
                    "leader-prefix",
                    "leader-prefix-partition",
                )
                and pipeline is None
                and submitted == 110
                and phase is None
                and fault_batch == 0,
                "fault batch declaration outside setup",
            )
            (fault_batch,) = map(int, args)
            require(1 <= fault_batch <= 7, "invalid leader fault batch")
        elif event == "workload-pipeline-fault":
            batch, mode = args
            require(
                pipeline is not None
                and int(batch) == pipeline
                and mode == pipeline_fault
                and pipeline not in pipeline_faults
                and phase is None
                and f"pipeline-before-{pipeline}" in phases,
                "unexpected or duplicate pipeline fault",
            )
            if mode == "restart":
                require(
                    pipeline < 2
                    and all(not item["results"] for item in pipeline_results.values()),
                    "restart outside outstanding batch",
                )
            if mode in (
                "leader-failover",
                "leader-partition",
                "leader-prefix",
                "leader-prefix-partition",
            ):
                require(
                    pipeline == fault_batch
                    and all(not item["results"] for item in pipeline_results.values()),
                    "leader failure outside outstanding batch",
                )
            pipeline_faults.add(pipeline)
        elif event == "workload-pipeline-aborted":
            require(
                args == [str(fault_batch)]
                and pipeline == fault_batch
                and pipeline_fault == "leader-partition"
                and pipeline in pipeline_faults
                and pipeline not in aborted_pipelines
                and phase is None
                and len(pipeline_results) == 4
                and all(
                    not item["results"] and not item["multi_results"]
                    for item in pipeline_results.values()
                ),
                "unexpected or acknowledged abandoned batch",
            )
            nodes = pipeline_start_nodes.copy()
            aborted_pipelines.add(pipeline)
            discarded_requests.update(pipeline_results)
        elif event == "workload-pipeline-prefix":
            require(
                args == [str(fault_batch), "2"]
                and pipeline == fault_batch
                and pipeline_fault in ("leader-prefix", "leader-prefix-partition")
                and pipeline in pipeline_faults
                and pipeline not in prefix_pipelines
                and phase is None
                and len(pipeline_results) == 4
                and all(
                    not item["results"] and not item["multi_results"]
                    for item in pipeline_results.values()
                ),
                "unexpected or acknowledged partial survivor",
            )
            pending = list(pipeline_results.items())
            require(
                all("state" in item for _, item in pending),
                "incomplete survivor transactions",
            )
            nodes = pending[1][1]["state"].copy()
            discarded_requests.update(xid for xid, _ in pending[2:])
            prefix_pipelines.add(pipeline)
        elif event == "workload-pipeline-client-committed":
            (xid,) = args
            require(
                phase is None
                and pipeline == fault_batch
                and pipeline_fault == "leader-prefix-partition"
                and pipeline in pipeline_faults
                and xid in list(pipeline_results)[:2]
                and xid not in committed_clients,
                "unexpected or discarded client completion",
            )
            committed_clients.add(xid)
        elif event == "workload-pipeline":
            batch, count = map(int, args)
            require(
                pipeline is None
                and phase is None
                and not closed
                and submitted == 110 + 4 * pipelines,
                "pipeline began outside transaction phase",
            )
            require(batch == pipelines and count == 4, "pipeline identity/size differs")
            require(
                (current_xid in discarded_requests)
                or (results == {1, 2, 3} and multi_results == {1, 2, 3}),
                "missing prior results before pipeline",
            )
            pipeline = batch
            pipeline_start_nodes = nodes.copy()
            pipeline_results = {}
        elif event == "workload-pipeline-finished":
            require(pipeline is not None, "pipeline finish outside batch")
            if (
                pipeline_fault == "reply-loss"
                or (pipeline_fault == "restart" and pipeline < 2)
                or (
                    pipeline_fault
                    in (
                        "leader-failover",
                        "leader-partition",
                        "leader-prefix",
                        "leader-prefix-partition",
                    )
                    and pipeline == fault_batch
                )
            ):
                require(
                    pipeline in pipeline_faults, "missing outstanding transaction fault"
                )
            if pipeline_fault == "restart" and pipeline < 2:
                require(
                    f"pipeline-recovered-{pipeline}" in phases,
                    "missing precommit recovery check",
                )
            if pipeline_fault == "leader-partition" and pipeline == fault_batch:
                require(
                    pipeline in aborted_pipelines, "missing abandoned minority batch"
                )
            if (
                pipeline_fault in ("leader-prefix", "leader-prefix-partition")
                and pipeline == fault_batch
            ):
                require(
                    pipeline in prefix_pipelines, "missing partial survivor boundary"
                )
            require(
                args == [str(pipeline)]
                and pipeline is not None
                and phase is None
                and f"pipeline-after-{pipeline}" in phases,
                "pipeline ended before verification",
            )
            results = pipeline_results[current_xid]["results"]
            multi_results = pipeline_results[current_xid]["multi_results"]
            pipelines += 1
            pipeline = None
        elif event == "workload-generated":
            require(
                generated is None and phase is None,
                "duplicate or premature generation marker",
            )
            generated = int(args[0])
            require(
                generated
                == submitted
                == (142 if pipelines else (110 if transactions else 76))
                and pipeline is None,
                "generated operation count differs",
            )
        elif event == "workload-close":
            require(
                not closed and generated == submitted and phase is None,
                "premature or duplicate close",
            )
            closed = True
            nodes = {path: state for path, state in nodes.items() if not state[2]}
        else:
            raise ValueError(f"Keeper model: unsupported history event {event}")
    require(
        generated == submitted == (142 if pipelines else (110 if transactions else 76))
        and closed
        and phase is None,
        "incomplete generated history",
    )
    require(
        (
            results == {1, 2, 3}
            or (current_xid in discarded_requests and not results and not multi_results)
        )
        and not observations,
        "incomplete results or observations",
    )
    require({"restored-1", "closed"} <= phases, "missing recovery or close check")
    require(
        all(
            f"step-{xid}" in phases
            for xid in range(100, 210 if pipelines else 100 + submitted)
        ),
        "missing operation check",
    )
    require({0, -101, -103, -108, -110, -111} <= errors, "missing semantic coverage")
    if transactions:
        require(
            transactions == (66 if pipelines else 34)
            and transaction_coverage
            == {"empty", "success", "failed-first", "failed-middle", "failed-last"},
            "missing transaction coverage",
        )
    if pipelines:
        require(
            pipelines == 8
            and pipeline is None
            and all(
                f"pipeline-{stage}-{batch}" in phases
                for stage in ("before", "after")
                for batch in range(8)
            ),
            "missing pipeline coverage",
        )
    require(
        pipeline_faults
        == (
            set(range(8))
            if pipeline_fault == "reply-loss"
            else (
                {0, 1}
                if pipeline_fault == "restart"
                else (
                    {fault_batch}
                    if pipeline_fault
                    in (
                        "leader-failover",
                        "leader-partition",
                        "leader-prefix",
                        "leader-prefix-partition",
                    )
                    else set()
                )
            )
        ),
        "missing pipeline fault coverage",
    )
    return {
        "operations": submitted,
        "transactions": transactions,
        "pipelines": pipelines,
        "pipeline_faults": len(pipeline_faults),
        "checkpoints": len(phases),
        "expected_errors": sorted(errors),
        **(
            {"aborted_pipelines": len(aborted_pipelines)}
            if pipeline_fault == "leader-partition"
            else {}
        ),
        **(
            {
                "partial_pipelines": len(prefix_pipelines),
                "discarded_requests": len(discarded_requests),
            }
            if pipeline_fault in ("leader-prefix", "leader-prefix-partition")
            else {}
        ),
        **(
            {"committed_clients": len(committed_clients)}
            if pipeline_fault == "leader-prefix-partition"
            else {}
        ),
    }
