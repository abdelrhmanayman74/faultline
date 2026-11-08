"""Integration tests: the real faultline_server binary, driven by the Python client.

ctest runs this file as `server_integration` and sets FAULTLINE_SERVER and
FAULTLINE_CLI (also in the asan and tsan builds). By hand:
    FAULTLINE_SERVER=build/debug/faultline_server FAULTLINE_CLI=build/debug/faultline \
        python3 -m unittest -v tests/python/test_server.py
Every server is started on port 0 and stopped with SIGTERM; a test fails if
the server does not exit with status 0 (sanitizer reports change the status).
Every blocking call has a timeout, so a broken server fails a test instead of
hanging it.
"""

import os
import select
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "client"))
from faultline_client import (  # noqa: E402
    HEADER, MAX_FRAME, FaultlineClient, ServerBusy, ServerError, recv_frame, send_frame)

SERVER = os.environ.get("FAULTLINE_SERVER")
CLI = os.environ.get("FAULTLINE_CLI", str(ROOT / "build" / "debug" / "faultline"))
C17 = ROOT / "benchmarks" / "iscas85" / "c17.bench"
C432 = ROOT / "benchmarks" / "downloaded" / "c432.bench"
C7552 = ROOT / "benchmarks" / "downloaded" / "c7552.bench"

TIMEOUT = 120.0  # generous: sanitizer builds are 5-20x slower
LONG_JOB = 30000  # random vectors on c7552: tens of seconds in a debug build


def wait_until(predicate, timeout=TIMEOUT, interval=0.02):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(interval)
    raise AssertionError(f"condition not met within {timeout} s")


def cli(*args):
    result = subprocess.run([CLI, *map(str, args)], capture_output=True, text=True,
                            timeout=TIMEOUT, check=False)
    if result.returncode != 0:
        raise AssertionError(result.stderr)
    return result.stdout


def report_fields(text):
    """{'vectors': '1000', ...} plus the D/U lines, from CLI or server output."""
    fields, lines = {}, []
    for line in text.splitlines():
        if line.startswith(("D ", "U ")):
            lines.append(line)
        elif ":" in line:
            key, value = line.split(":", 1)
            fields[key] = value.split()[0]
    return fields, lines


class ServerProcess:
    def __init__(self, *options):
        self.stderr = tempfile.TemporaryFile()  # a file, not a pipe: it can never fill up
        self.proc = subprocess.Popen([SERVER, "--port", "0", *map(str, options)],
                                     stdout=subprocess.PIPE, stderr=self.stderr, text=True)
        ready, _, _ = select.select([self.proc.stdout], [], [], TIMEOUT)
        line = self.proc.stdout.readline() if ready else ""
        if not line.startswith("listening on "):
            self.proc.kill()
            raise AssertionError(f"server did not start: {line!r} {self.read_stderr()}")
        self.port = int(line.rsplit(":", 1)[1])

    def client(self, timeout=TIMEOUT):
        return FaultlineClient("127.0.0.1", self.port, timeout)

    def raw(self):
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=TIMEOUT)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)  # really send tiny pieces
        return sock

    def read_stderr(self):
        self.stderr.seek(0)
        return self.stderr.read().decode(errors="replace")

    def stop(self):
        """SIGTERM, then the exit status and how long the shutdown took."""
        start = time.monotonic()
        if self.proc.poll() is None:
            self.proc.send_signal(signal.SIGTERM)
        try:
            code = self.proc.wait(timeout=TIMEOUT)
        except subprocess.TimeoutExpired:
            self.proc.kill()
            self.proc.wait()
            code = "killed after timeout"
        self.proc.stdout.close()
        return code, time.monotonic() - start


@unittest.skipUnless(SERVER, "FAULTLINE_SERVER not set (ctest runs this as server_integration)")
class ServerTest(unittest.TestCase):
    options = ()

    def setUp(self):
        self.server = ServerProcess(*self.options)
        self.addCleanup(self.check_clean_exit)

    def check_clean_exit(self):
        code, _ = self.server.stop()
        stderr = self.server.read_stderr()
        self.server.stderr.close()
        self.assertEqual(code, 0, stderr)
        self.assertNotIn("Sanitizer", stderr)

    def connect(self):
        client = self.server.client()
        self.addCleanup(client.close)
        return client

    def require(self, path):
        if not path.exists():
            self.skipTest(f"{path.name} not downloaded (tools/fetch_benchmarks.sh)")

    def start_long_job(self, client):
        self.require(C7552)
        client.load("c7552", C7552.read_text())
        job = client.faultsim("c7552", random=LONG_JOB, threads=1)
        wait_until(lambda: client.status(job)["state"] == "running")
        return job


class Commands(ServerTest):
    def test_load_and_sim_match_cli(self):
        client = self.connect()
        reply = client.load("c17", C17.read_text())
        self.assertEqual(reply, "OK c17 gates=6 inputs=5 outputs=2 depth=3")
        vectors = [format(v, "05b") for v in range(32)]
        with tempfile.NamedTemporaryFile("w", suffix=".txt") as file:
            file.write("\n".join(vectors) + "\n")
            file.flush()
            expected = cli("sim", C17, file.name).split()
        self.assertEqual(client.sim("c17", vectors), expected)

    def test_parse_error_reports_line(self):
        client = self.connect()
        with self.assertRaises(ServerError) as error:
            client.load("bad", "INPUT(a)\nOUTPUT(y)\ny = AND(a, ghost)\n")
        self.assertIn("bad:3: undefined net 'ghost'", str(error.exception))
        with self.assertRaises(ServerError) as error:
            client.sim("bad", ["1"])
        self.assertIn("unknown circuit 'bad'", str(error.exception))

    def test_bad_requests_keep_the_connection_usable(self):
        client = self.connect()
        self.assertEqual(client.request("HELLO"), "ERR unknown command 'HELLO'")
        self.assertEqual(client.request("FAULTSIM c17 threads=0 random=5"),
                         "ERR threads must be between 1 and 64")
        self.assertEqual(client.request("RESULT 77"), "ERR unknown job 77")
        self.assertEqual(client.request("CANCEL 77"), "ERR unknown job 77")
        client.load("c17", C17.read_text())
        reply = client.request("SIM c17\n10101\n101\n")  # body line 2 is too short
        self.assertTrue(reply.startswith("ERR vectors:2:"), reply)
        self.assertTrue(client.request("STATUS").startswith("OK connections=1 circuits=1"))
        self.assertEqual(client.quit(), "OK bye")
        self.assertIsNone(recv_frame(client.sock))  # server closed the connection

    def test_faultsim_c432_matches_cli(self):
        self.require(C432)
        client = self.connect()
        client.load("c432", C432.read_text())
        job = client.faultsim("c432", random=1000, seed=7, threads=4, details=True)
        served = report_fields(client.wait_result(job))
        local = report_fields(cli("faultsim", C432, "--random", 1000, "--seed", 7,
                                  "--detections", "--undetected"))
        for key in ("vectors", "faults", "detected", "undetected", "coverage"):
            self.assertEqual(served[0][key], local[0][key], key)
        self.assertEqual(len(served[1]), int(local[0]["faults"]))
        self.assertEqual(served[1], local[1])  # every fault, same first detecting vector
        self.assertEqual(client.status(job)["state"], "done")
        self.assertEqual(client.status(job)["detected"], local[0]["detected"])

    def test_faultsim_with_vector_body_matches_cli(self):
        client = self.connect()
        client.load("c17", C17.read_text())
        vectors = ["10101", "01000", "11111", "00110"]
        job = client.faultsim("c17", vectors=vectors, details=True)
        with tempfile.NamedTemporaryFile("w", suffix=".txt") as file:
            file.write("\n".join(vectors) + "\n")
            file.flush()
            local = report_fields(cli("faultsim", C17, "--vectors", file.name,
                                      "--detections", "--undetected"))
        self.assertEqual(report_fields(client.wait_result(job))[1], local[1])


class Framing(ServerTest):
    def test_oversize_frame_is_rejected_and_closed(self):
        sock = self.server.raw()
        sock.sendall(HEADER.pack(MAX_FRAME + 1))
        reply = recv_frame(sock).decode()
        self.assertTrue(reply.startswith("ERR frame of 16777217 bytes is over the limit"), reply)
        self.assertIsNone(recv_frame(sock))
        sock.close()
        self.assertTrue(self.connect().request("STATUS").startswith("OK"))  # server still fine

    def test_truncated_frame_then_disconnect(self):
        sock = self.server.raw()
        sock.sendall(HEADER.pack(100) + b"STATUS")  # 6 of 100 bytes, then gone
        sock.close()
        self.assertTrue(self.connect().request("STATUS").startswith("OK"))

    def test_byte_by_byte_frames(self):
        sock = self.server.raw()
        self.addCleanup(sock.close)
        for payload in (f"LOAD c17\n{C17.read_text()}", "SIM c17\n10101\n00000\n"):
            frame = HEADER.pack(len(payload)) + payload.encode()
            for i in range(len(frame)):
                sock.send(frame[i:i + 1])
                time.sleep(0.001)  # each byte travels on its own
            reply = recv_frame(sock).decode()
            self.assertTrue(reply.startswith("OK"), reply)
        self.assertEqual(reply, "OK 2\n11\n00\n")


class Jobs(ServerTest):
    options = ("--job-workers", 1, "--queue", 1)

    def test_busy_when_queue_full(self):
        client = self.connect()
        running = self.start_long_job(client)  # occupies the only worker
        queued = client.faultsim("c7552", random=LONG_JOB)  # takes the only queue slot
        with self.assertRaises(ServerBusy):
            client.faultsim("c7552", random=LONG_JOB)
        self.assertEqual(client.status(queued)["state"], "queued")
        status = client.status()
        self.assertEqual((status["running"], status["queued"]), ("1", "1"))
        self.assertEqual(client.cancel(queued), "OK")
        self.assertEqual(client.status(queued)["state"], "cancelled")  # at once, never ran
        self.assertEqual(client.cancel(running), "OK")
        wait_until(lambda: client.status(running)["state"] == "cancelled")

    def test_cancel_long_job(self):
        client = self.connect()
        job = self.start_long_job(client)
        self.assertIsNone(client.result(job))  # PENDING
        start = time.monotonic()
        self.assertEqual(client.cancel(job), "OK")
        wait_until(lambda: client.status(job)["state"] == "cancelled")
        latency = time.monotonic() - start
        print(f"\n  cancel latency: {latency * 1000:.0f} ms", file=sys.stderr)
        self.assertLess(latency, 30.0)
        with self.assertRaises(ServerError) as error:
            client.result(job)
        self.assertIn("was cancelled", str(error.exception))
        self.assertIn("already cancelled", client.request(f"CANCEL {job}"))

    def test_disconnect_cancels_jobs(self):
        owner = self.server.client()
        job = self.start_long_job(owner)
        owner.close()  # leaves without cancelling
        observer = self.connect()
        wait_until(lambda: observer.status(job)["state"] == "cancelled")

    def test_sigterm_stops_running_job_quickly(self):
        client = self.connect()
        self.start_long_job(client)
        code, seconds = self.server.stop()
        self.assertEqual(code, 0)
        self.assertLess(seconds, 30.0)


class Connections(ServerTest):
    options = ("--max-connections", 2)

    def try_status(self):
        """STATUS on a fresh connection; None if the server turned us away."""
        try:
            sock = self.server.raw()
            with sock:
                send_frame(sock, b"STATUS")
                reply = recv_frame(sock)
        except (ConnectionResetError, BrokenPipeError):
            return None  # refused with data still unread: the kernel sent a reset
        if reply is None or reply == b"ERR server full":
            return None
        return reply.decode()

    def test_connection_cap(self):
        first, second = self.server.client(), self.connect()
        first.status()
        second.status()  # both are being served now
        extra = self.server.raw()
        self.assertEqual(recv_frame(extra), b"ERR server full")
        self.assertIsNone(recv_frame(extra))
        extra.close()
        first.close()
        reply = wait_until(self.try_status)  # a slot frees up once `first` is gone
        self.assertTrue(reply.startswith("OK connections=2"), reply)


class Concurrency(ServerTest):
    def test_eight_concurrent_clients(self):
        vectors = [format(v, "05b") for v in range(32)]
        with self.connect() as reference:
            reference.load("c17", C17.read_text())
            expected_sim = reference.sim("c17", vectors)
            expected_fault = reference.wait_result(reference.faultsim("c17", random=300, seed=3))
        start = threading.Barrier(8, timeout=TIMEOUT)  # all clients send at about the same time

        def session(index):
            with self.server.client() as client:
                start.wait()
                name = f"c17_{index}"
                client.load(name, C17.read_text())
                for _ in range(5):
                    self.assertEqual(client.sim(name, vectors), expected_sim)
                job = client.faultsim(name, random=300, seed=3, threads=2)
                result = client.wait_result(job)
                self.assertEqual(result.split("time_ms")[0], expected_fault.split("time_ms")[0])
            return index

        with ThreadPoolExecutor(max_workers=8) as pool:
            futures = [pool.submit(session, i) for i in range(8)]
            self.assertEqual(sorted(f.result(timeout=TIMEOUT) for f in futures), list(range(8)))
        self.assertEqual(self.connect().status()["circuits"], "9")


if __name__ == "__main__":
    unittest.main()
