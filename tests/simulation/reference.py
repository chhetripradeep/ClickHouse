#!/usr/bin/env python3
"""Real-process reference experiment. This is NOT a deterministic simulator."""

import argparse
import hashlib
import json
import shutil
import socket
import subprocess
import threading
import time
import uuid
from contextlib import ExitStack, contextmanager
from dataclasses import asdict
from pathlib import Path
from xml.sax.saxutils import escape

from clickhouse_driver import Client
from clickhouse_driver.errors import NetworkError, SocketTimeoutError
from clickhouse_driver.protocol import ServerPacketTypes
from contract import (
    BATCHES,
    INSERT_SETTINGS,
    ROWS_PER_BATCH,
    TABLE_SETTINGS,
    Attempt,
    batch_rows,
    check_rows,
)


class LostAcknowledgement(EOFError):
    """The transport adapter withheld success from the workload."""


@contextmanager
def observe_insert(client, drop_success):
    """Count native input blocks and optionally withhold the terminal success packet.

    This is an application-boundary acknowledgement-loss fault, not a TCP proxy.
    No cancellation, server exception, or arbitrary timer is used as a success trigger.
    """
    connection = client.connection
    receive = connection.receive_packet
    send = connection.send_data
    evidence = {"blocks": [], "success_withheld": False}

    def receive_packet():
        packet = receive()
        if drop_success and packet.type == ServerPacketTypes.END_OF_STREAM:
            evidence["success_withheld"] = True
            connection.disconnect()
            raise LostAcknowledgement("Terminal insert success withheld")
        return packet

    def send_data(block, table_name=""):
        if block.num_rows:
            evidence["blocks"].append(block.num_rows)
        return send(block, table_name)

    connection.receive_packet = receive_packet
    connection.send_data = send_data
    try:
        yield evidence
    finally:
        connection.receive_packet = receive
        connection.send_data = send


def wait_until(description, predicate, processes, timeout=120):
    deadline = time.monotonic() + timeout
    pause = threading.Event()
    while True:
        for name, process in processes.items():
            if process.poll() is not None:
                raise RuntimeError(
                    f"{name} exited with {process.returncode}; inspect its console.log"
                )
        if predicate():
            return
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            raise TimeoutError(f"Timed out waiting for {description}")
        pause.wait(min(0.1, remaining))


class Cluster:
    def __init__(self, binary, root):
        self.binary = binary
        self.root = root
        self.processes = {}
        self.logs = []
        # Reserve all ports together. There is still a bind race after release;
        # startup failure is fatal rather than silently choosing a different setup.
        with ExitStack() as stack:
            ports = []
            for _ in range(12):
                reservation = stack.enter_context(socket.socket())
                reservation.bind(("127.0.0.1", 0))
                ports.append(reservation.getsockname()[1])
            self.keeper_ports = ports[:3]
            raft_ports = ports[3:6]
            self.replica_ports = {"r1": ports[6], "r2": ports[7]}
            self.write_configs(raft_ports, ports[8:10], ports[10:12])

    def write_configs(self, raft_ports, http_ports, interserver_ports):
        raft = "".join(
            f"<server><id>{i + 1}</id><hostname>127.0.0.1</hostname><port>{port}</port></server>"
            for i, port in enumerate(raft_ports)
        )
        for i, port in enumerate(self.keeper_ports):
            name = f"k{i + 1}"
            path = self.root / name
            path.mkdir()
            data = escape(str(path))
            (path / "config.xml").write_text(f"""<clickhouse>
<listen_host>127.0.0.1</listen_host>
<logger><level>information</level><console>1</console></logger>
<keeper_server><tcp_port>{port}</tcp_port><server_id>{i + 1}</server_id>
<log_storage_path>{data}/logs</log_storage_path>
<snapshot_storage_path>{data}/snapshots</snapshot_storage_path>
<coordination_settings><force_sync>true</force_sync>
<heart_beat_interval_ms>500</heart_beat_interval_ms>
<election_timeout_lower_bound_ms>1000</election_timeout_lower_bound_ms>
<election_timeout_upper_bound_ms>2000</election_timeout_upper_bound_ms>
</coordination_settings><raft_configuration>{raft}</raft_configuration>
</keeper_server></clickhouse>""")

        keepers = "".join(
            f"<node><host>127.0.0.1</host><port>{port}</port></node>"
            for port in self.keeper_ports
        )
        for i, (name, port) in enumerate(self.replica_ports.items()):
            path = self.root / name
            path.mkdir()
            data = escape(str(path))
            (path / "users.xml").write_text("""<clickhouse>
<profiles><default><max_threads>2</max_threads></default></profiles>
<users><default><password></password><networks><ip>127.0.0.1</ip></networks>
<profile>default</profile><quota>default</quota></default></users>
<quotas><default><interval><duration>3600</duration><queries>0</queries>
<errors>0</errors><result_rows>0</result_rows><read_rows>0</read_rows>
<execution_time>0</execution_time></interval></default></quotas></clickhouse>""")
            (path / "config.xml").write_text(f"""<clickhouse>
<listen_host>127.0.0.1</listen_host><tcp_port>{port}</tcp_port>
<http_port>{http_ports[i]}</http_port>
<interserver_http_port>{interserver_ports[i]}</interserver_http_port>
<interserver_http_host>127.0.0.1</interserver_http_host>
<logger><level>information</level><console>1</console></logger>
<path>{data}/data/</path><tmp_path>{data}/tmp/</tmp_path>
<user_files_path>{data}/user_files/</user_files_path>
<users_config>{data}/users.xml</users_config>
<max_server_memory_usage>1073741824</max_server_memory_usage>
<max_thread_pool_size>256</max_thread_pool_size>
<background_pool_size>16</background_pool_size>
<background_schedule_pool_size>4</background_schedule_pool_size>
<background_fetches_pool_size>2</background_fetches_pool_size>
<background_move_pool_size>2</background_move_pool_size>
<background_common_pool_size>2</background_common_pool_size>
<zookeeper>{keepers}</zookeeper></clickhouse>""")

    def start(self, name):
        path = self.root / name
        log = (path / "console.log").open("a")
        self.logs.append(log)
        mode = "keeper" if name.startswith("k") else "server"
        self.processes[name] = subprocess.Popen(
            [self.binary, mode, f"--config-file={path / 'config.xml'}"],
            cwd=path,
            stdout=log,
            stderr=subprocess.STDOUT,
        )

    def client(self, replica, timeout=15):
        return Client(
            "127.0.0.1",
            port=self.replica_ports[replica],
            connect_timeout=1,
            send_receive_timeout=timeout,
            settings={"insert_block_size": ROWS_PER_BATCH},
        )

    def ready(self, replica):
        try:
            with self.client(replica) as client:
                return client.execute("SELECT 1") == [(1,)]
        except (NetworkError, SocketTimeoutError, EOFError):
            return False

    def start_all(self):
        for name in ("k1", "k2", "k3", "r1", "r2"):
            self.start(name)

        def keeper_ready(port):
            try:
                with socket.create_connection(
                    ("127.0.0.1", port), timeout=1
                ) as connection:
                    connection.sendall(b"ruok")
                    return connection.recv(4) == b"imok"
            except (ConnectionError, TimeoutError):
                return False

        for port in self.keeper_ports:
            wait_until(
                "Keeper client listener", lambda: keeper_ready(port), self.processes
            )
        for name in self.replica_ports:
            wait_until(f"{name} startup", lambda: self.ready(name), self.processes)

    def crash_restart(self, name):
        process = self.processes.pop(name)
        process.kill()
        process.wait(timeout=30)
        self.start(name)
        wait_until(f"{name} restart", lambda: self.ready(name), self.processes)

    def close(self):
        for process in self.processes.values():
            if process.poll() is None:
                process.terminate()
        try:
            for process in self.processes.values():
                try:
                    process.wait(timeout=30)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait(timeout=30)
        finally:
            for log in self.logs:
                log.close()


def run(binary, root, scenario):
    run_id = uuid.uuid4().hex
    attempts = []
    started = time.monotonic()
    history = (root / "history.jsonl").open("w", buffering=1)

    def event(kind, **details):
        history.write(
            json.dumps(
                {
                    "event": kind,
                    "elapsed_seconds": time.monotonic() - started,
                    **details,
                }
            )
            + "\n"
        )

    with open(binary, "rb") as executable:
        binary_hash = hashlib.file_digest(executable, "sha256").hexdigest()
    manifest = {
        "backend": "real-process",
        "deterministic": False,
        "scenario": scenario,
        "run_id": run_id,
        "binary": binary,
        "binary_sha256": binary_hash,
        "source_revision": subprocess.check_output(
            ["git", "rev-parse", "HEAD"],
            cwd=Path(__file__).resolve().parents[2],
            text=True,
        ).strip(),
        "binary_built_from_source_revision": "unverified",
        "insert_settings": INSERT_SETTINGS,
        "table_settings": TABLE_SETTINGS,
        "batches": BATCHES,
        "rows_per_batch": ROWS_PER_BATCH,
        "feasibility_gate": {
            "status": "blocked",
            "node_isolation": "processes only; ContextSharedPart and DatabaseCatalog prohibit independent in-process nodes",
            "controlled_blocking": "not implemented",
            "deterministic_replay": "not implemented",
            "runtime_feasibility_probe_executed": False,
        },
    }
    (root / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    (root / "expected.json").write_text(
        json.dumps(
            [row for batch_id in range(BATCHES) for row in batch_rows(run_id, batch_id)]
        )
        + "\n"
    )
    cluster = Cluster(binary, root)

    def insert(batch_id, replica, *, drop=False):
        query_id = f"{run_id}-{len(attempts)}"
        settings = {
            **INSERT_SETTINGS,
            "insert_deduplication_token": f"{run_id}-{batch_id}",
        }
        event(
            "submit",
            batch_id=batch_id,
            replica=replica,
            query_id=query_id,
            token=settings["insert_deduplication_token"],
        )
        with cluster.client(replica) as client, observe_insert(
            client, drop
        ) as evidence:
            try:
                client.execute(
                    "INSERT INTO experiment VALUES",
                    batch_rows(run_id, batch_id),
                    query_id=query_id,
                    settings=settings,
                    types_check=True,
                )
            except LostAcknowledgement:
                attempt = Attempt(batch_id, replica, query_id, "unknown")
            except Exception as error:
                attempt = Attempt(batch_id, replica, query_id, "unknown")
                attempts.append(attempt)
                event("attempt", **asdict(attempt), error=str(error), **evidence)
                raise
            else:
                attempt = Attempt(batch_id, replica, query_id, "success")
        attempts.append(attempt)
        event("attempt", **asdict(attempt), **evidence)
        if evidence["blocks"] != [ROWS_PER_BATCH]:
            raise AssertionError(f"Expected one native data block, got {evidence}")
        if drop and not evidence["success_withheld"]:
            raise AssertionError("Lost acknowledgement scenario was not exercised")

    def validate(label):
        snapshots = {}
        for replica in cluster.replica_ports:
            with cluster.client(replica, timeout=120) as client:
                client.execute(
                    "SYSTEM SYNC REPLICA experiment", settings={"receive_timeout": 120}
                )
                rows = client.execute(
                    "SELECT run_id, batch_id, row_id, payload FROM experiment ORDER BY run_id, batch_id, row_id"
                )
            (root / f"{label}-{replica}.json").write_text(json.dumps(rows) + "\n")
            violations = check_rows(run_id, attempts, rows, require_all=True)
            event(
                "validation",
                phase=label,
                replica=replica,
                rows=len(rows),
                violations=violations,
            )
            if violations:
                raise AssertionError(f"{replica}: {violations}")
            snapshots[replica] = rows
        if snapshots["r1"] != snapshots["r2"]:
            raise AssertionError("Replicas diverged")

    try:
        cluster.start_all()
        for replica in cluster.replica_ports:
            with cluster.client(replica) as client:
                version = client.execute("SELECT version()")
                effective = client.execute(
                    "SELECT name, value FROM system.settings WHERE name IN %(names)s ORDER BY name",
                    {"names": tuple(INSERT_SETTINGS)},
                    settings=INSERT_SETTINGS,
                )
                event(
                    "configuration",
                    replica=replica,
                    version=version,
                    effective_settings=effective,
                )
                if dict(effective) != {
                    name: str(value) for name, value in INSERT_SETTINGS.items()
                }:
                    raise AssertionError(
                        f"Insert settings differ from the contract: {effective}"
                    )
                table_settings = ", ".join(
                    f"{name}={value}" for name, value in TABLE_SETTINGS.items()
                )
                client.execute(
                    "CREATE TABLE experiment (run_id String, batch_id UInt32, row_id UInt32, payload String) "
                    f"ENGINE=ReplicatedMergeTree('/simulation/{run_id}/table', '{replica}') "
                    f"PARTITION BY tuple() ORDER BY (run_id, batch_id, row_id) SETTINGS {table_settings}"
                )
                event(
                    "table_configuration",
                    replica=replica,
                    definition=client.execute("SHOW CREATE TABLE experiment"),
                )
                if client.execute("SELECT count() FROM experiment") != [(0,)]:
                    raise AssertionError("Initial table is not empty")

        def replicas_active():
            with cluster.client("r1") as client:
                return client.execute(
                    "SELECT active_replicas=2 AND total_replicas=2 AND NOT is_readonly "
                    "FROM system.replicas WHERE table='experiment'"
                ) == [(1,)]

        wait_until("both replicas active", replicas_active, cluster.processes)
        for batch_id in range(BATCHES):
            insert(batch_id, "r1", drop=scenario == "lost-response" and batch_id == 8)
            if scenario == "lost-response" and batch_id == 8:
                # The workload only knows 'unknown'; only the fault adapter knows
                # that the server produced a terminal success packet.
                insert(batch_id, "r1")
                insert(batch_id, "r2")
        validate("reconciled")
        for replica in cluster.replica_ports:
            event(
                "process_crash",
                replica=replica,
                storage="preserved; no power-loss model",
            )
            cluster.crash_restart(replica)
            wait_until("replicas reactivated", replicas_active, cluster.processes)
            validate(f"restarted-{replica}")
        event("result", verdict="pass", deterministic=False)
    except BaseException as error:
        event("result", verdict="fail", error=repr(error), deterministic=False)
        raise
    finally:
        cluster.close()
        history.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--binary",
        required=True,
        help="Explicit ClickHouse executable; its hash is recorded",
    )
    parser.add_argument(
        "--output",
        required=True,
        type=Path,
        help="New directory for configuration, state, and evidence",
    )
    parser.add_argument(
        "--scenario", choices=("baseline", "lost-response"), default="lost-response"
    )
    args = parser.parse_args()
    binary = shutil.which(args.binary)
    if binary is None:
        parser.error(f"Executable not found: {args.binary}")
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=False)
    run(str(Path(binary).resolve()), root, args.scenario)
    print(f"PASS ({args.scenario}, real-process, NOT deterministic). Evidence: {root}")


if __name__ == "__main__":
    main()
