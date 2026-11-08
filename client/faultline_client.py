#!/usr/bin/env python3
"""Client for faultline_server (protocol: docs/PROTOCOL.md). Standard library only.

Library:
    with FaultlineClient("127.0.0.1", 5555) as client:
        client.load("c17", Path("c17.bench").read_text())
        print(client.sim("c17", ["10101", "00000"]))
        job = client.faultsim("c17", random=1000, seed=1)
        print(client.wait_result(job))

Command line:
    faultline_client.py --port 5555 load c17 benchmarks/iscas85/c17.bench
    faultline_client.py --port 5555 sim c17 vectors.txt
    faultline_client.py --port 5555 faultsim c17 --random 1000   (waits; Ctrl-C cancels)
    faultline_client.py --port 5555 status [JOB]
    faultline_client.py --port 5555 result JOB [--wait]
    faultline_client.py --port 5555 cancel JOB

The server cancels a job when the connection that submitted it closes, so the
`faultsim` command keeps its connection open until the result arrives. Use
status/result/cancel from a second terminal while it waits.
"""

import argparse
import socket
import struct
import sys
import time
from pathlib import Path

HEADER = struct.Struct("!I")  # "!" = network byte order (big-endian), "I" = 32-bit unsigned
MAX_FRAME = 16 * 1024 * 1024  # must match kMaxFrameSize in protocol.hpp


class ServerError(Exception):
    """The server answered ERR <message>."""


class ServerBusy(Exception):
    """The job queue is full; try again later."""


def recv_exact(sock, size):
    """Read exactly `size` bytes. recv() may return fewer than asked, so loop.

    Returns fewer than `size` bytes only if the server closed the connection.
    """
    chunks = []
    remaining = size
    while remaining > 0:
        chunk = sock.recv(min(remaining, 1 << 20))
        if not chunk:  # b"" means the peer closed the connection
            break
        chunks.append(chunk)
        remaining -= len(chunk)
    return b"".join(chunks)


def send_frame(sock, payload):
    if len(payload) > MAX_FRAME:
        raise ValueError(f"payload of {len(payload)} bytes is over the {MAX_FRAME} byte limit")
    # sendall() loops over partial sends itself.
    sock.sendall(HEADER.pack(len(payload)) + payload)


def recv_frame(sock):
    """One frame's payload as bytes, or None if the server closed between frames."""
    header = recv_exact(sock, HEADER.size)
    if not header:
        return None
    if len(header) < HEADER.size:
        raise ConnectionError("connection closed in the middle of a frame header")
    (length,) = HEADER.unpack(header)
    if length > MAX_FRAME:
        raise ConnectionError(f"server sent a frame of {length} bytes, over the limit")
    payload = recv_exact(sock, length)
    if len(payload) < length:
        raise ConnectionError("connection closed in the middle of a frame")
    return payload


class FaultlineClient:
    def __init__(self, host="127.0.0.1", port=5555, timeout=30.0):
        # The timeout applies to every blocking socket call, so a stuck server
        # raises socket.timeout instead of hanging the caller forever.
        self.sock = socket.create_connection((host, port), timeout=timeout)

    def close(self):
        self.sock.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def request(self, payload):
        """Send one request (str) and return the raw reply text."""
        send_frame(self.sock, payload.encode())
        reply = recv_frame(self.sock)
        if reply is None:
            raise ConnectionError("server closed the connection")
        return reply.decode()

    def _checked(self, payload):
        """Like request(), but ERR and BUSY become exceptions."""
        reply = self.request(payload)
        if reply.startswith("ERR "):
            raise ServerError(reply[4:])
        if reply == "BUSY":
            raise ServerBusy()
        return reply

    def load(self, name, bench_text):
        return self._checked(f"LOAD {name}\n{bench_text}")

    def sim(self, name, vectors):
        """vectors: iterable of '0'/'1' strings. Returns one output string per vector."""
        reply = self._checked(f"SIM {name}\n" + "".join(v + "\n" for v in vectors))
        return reply.splitlines()[1:]  # first line is "OK <count>"

    def faultsim(self, name, random=None, seed=1, threads=1, vectors=None, details=False):
        """Submit a fault-simulation job; returns its id. Raises ServerBusy if the queue is full."""
        line = f"FAULTSIM {name} seed={seed} threads={threads} details={int(details)}"
        if random is not None:
            line += f" random={random}"
        body = "".join(v + "\n" for v in vectors) if vectors is not None else ""
        reply = self._checked(f"{line}\n{body}")
        return int(reply.split()[1])  # "JOB <id>"

    def status(self, job=None):
        """Server counters, or one job's fields, as a dict."""
        reply = self._checked("STATUS" if job is None else f"STATUS {job}")
        head, has_error, error = reply.partition(" error=")  # a failed job's message has spaces
        fields = dict(field.split("=", 1) for field in head.split()[1:])
        if has_error:
            fields["error"] = error
        return fields

    def result(self, job):
        """The result text once done, or None while the job is queued or running."""
        reply = self._checked(f"RESULT {job}")
        if reply.startswith("PENDING"):
            return None
        return reply[len("OK\n"):]

    def wait_result(self, job, timeout=300.0, poll_interval=0.05):
        deadline = time.monotonic() + timeout
        while True:
            text = self.result(job)
            if text is not None:
                return text
            if time.monotonic() > deadline:
                raise TimeoutError(f"job {job} not finished after {timeout} s")
            time.sleep(poll_interval)

    def cancel(self, job):
        return self._checked(f"CANCEL {job}")

    def quit(self):
        return self.request("QUIT")


def read_vector_file(path):
    """Vector lines without comments and blanks (the server would accept them too)."""
    lines = (line.split("#", 1)[0].strip() for line in Path(path).read_text().splitlines())
    return [line for line in lines if line]


def main(argv=None):
    parser = argparse.ArgumentParser(description="faultline_server client")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, required=True)
    parser.add_argument("--timeout", type=float, default=30.0)
    commands = parser.add_subparsers(dest="command", required=True)

    p = commands.add_parser("load", help="load a .bench file under a name")
    p.add_argument("name")
    p.add_argument("bench")
    p = commands.add_parser("sim", help="simulate a vector file")
    p.add_argument("name")
    p.add_argument("vectors")
    p = commands.add_parser("faultsim", help="run a fault-simulation job and wait for it")
    p.add_argument("name")
    source = p.add_mutually_exclusive_group(required=True)
    source.add_argument("--random", type=int)
    source.add_argument("--vectors")
    p.add_argument("--seed", type=int, default=1)
    p.add_argument("--threads", type=int, default=1)
    p.add_argument("--details", action="store_true", help="per-fault D/U lines")
    p = commands.add_parser("status", help="server counters or one job")
    p.add_argument("job", nargs="?", type=int)
    p = commands.add_parser("result", help="result of a job")
    p.add_argument("job", type=int)
    p.add_argument("--wait", action="store_true")
    p = commands.add_parser("cancel", help="cancel a job")
    p.add_argument("job", type=int)
    args = parser.parse_args(argv)

    try:
        with FaultlineClient(args.host, args.port, args.timeout) as client:
            if args.command == "load":
                print(client.load(args.name, Path(args.bench).read_text()))
            elif args.command == "sim":
                print("\n".join(client.sim(args.name, read_vector_file(args.vectors))))
            elif args.command == "faultsim":
                vectors = read_vector_file(args.vectors) if args.vectors else None
                job = client.faultsim(args.name, args.random, args.seed, args.threads,
                                      vectors, args.details)
                print(f"JOB {job}", flush=True)  # visible at once, for status/cancel elsewhere
                print(client.wait_result(job), end="")
            elif args.command == "status":
                status = client.status(args.job)
                print(" ".join(f"{key}={value}" for key, value in status.items()))
            elif args.command == "result":
                text = client.wait_result(args.job) if args.wait else client.result(args.job)
                if text is None:
                    print("PENDING")
                else:
                    print(text, end="")
            elif args.command == "cancel":
                print(client.cancel(args.job))
    except ServerBusy:
        print("BUSY: job queue full, try again later", file=sys.stderr)
        return 2
    except (ServerError, OSError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
