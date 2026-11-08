# faultline server protocol

`faultline_server` speaks a small request/reply protocol over TCP. The client
sends one request frame, the server sends exactly one reply frame, and so on.
Requests on one connection are handled in order.

```
faultline_server --port P [--host 127.0.0.1] [--max-connections N] [--job-workers W] [--queue Q]
```

Defaults: host `127.0.0.1` (loopback only), 16 connections, 2 job workers,
16 queued jobs. `--port 0` lets the kernel choose a free port. The first line
on stdout is `listening on <host>:<port>`. SIGINT or SIGTERM shuts the server
down cleanly (exit status 0).

## Framing

Every message in both directions is one frame:

```
+--------------------------------+---------------------------+
| length: 4 bytes, big-endian    | payload: `length` bytes   |
+--------------------------------+---------------------------+
```

- TCP is a byte stream; it keeps no message boundaries. The length prefix is
  how the reader knows where a message ends, however the bytes were split
  into `recv()` results (a frame sent one byte at a time works).
- Big-endian ("network byte order") is the usual choice for wire formats;
  the C++ side uses `htonl`/`ntohl`, the Python client `struct.Struct("!I")`.
- **Maximum payload: 16 MiB (16,777,216 bytes).** A larger length is a
  protocol violation: the server replies `ERR frame of N bytes is over the
  limit of 16777216` and closes the connection without reading the payload.
  The limit stops a client from making the server allocate gigabytes with one
  header. A connection that closes in the middle of a frame is also closed by
  the server. In both cases the stream cannot be resynchronized, so the
  connection is not kept.
- A reply that would be over the limit (for example SIM on millions of
  vectors) is replaced by an `ERR` reply and the connection is closed.
- A zero-length frame is allowed by the framing and answered `ERR empty request`.

## Payload

The payload is text. The first line (up to the first `\n`; a trailing `\r`
is ignored) is the command and its space-separated arguments. Everything
after the first `\n` is the body, used by `LOAD`, `SIM` and `FAULTSIM`.
Commands are upper-case and case-sensitive.

Every reply starts with a status word: `OK`, `ERR`, `JOB`, `BUSY` or
`PENDING`. An `ERR` reply to a bad request (unknown command, wrong
arguments, parse error, unknown circuit or job) keeps the connection open.

## Commands

### LOAD `<name>`

Body: a `.bench` netlist. Parses it and stores it under `<name>` for all
connections (a later LOAD with the same name replaces it; jobs already
running keep the old netlist). At most 64 names.

```
> LOAD c17\n# c17\nINPUT(1)\nINPUT(2)\n...
< OK c17 gates=6 inputs=5 outputs=2 depth=3

> LOAD bad\nINPUT(a)\nOUTPUT(y)\ny = AND(a, ghost)\n
< ERR bad:3: undefined net 'ghost' used by gate 'y'
```

Parse errors have the form `<name>:<line>: <message>`, the line counted from
the first body line.

### SIM `<name>`

Body: input vectors, one per line, `0`/`1` in primary-input order (same
format as the CLI's vector files: `#` comments and blank lines allowed).
Runs the bit-parallel simulator on the connection's own thread and replies
with the number of vectors, then one line of output values per vector, in
primary-output order.

```
> SIM c17\n10101\n00000\n
< OK 2\n11\n00\n

> SIM c17\n10101\n101\n
< ERR vectors:2: vector has 3 bits, circuit has 5 primary inputs
```

### FAULTSIM `<name>` `[random=N]` `[seed=S]` `[threads=T]` `[details=0|1]`

Submits a stuck-at fault simulation job and replies at once, without waiting
for the job:

- `JOB <id>`: queued; use `STATUS <id>` / `RESULT <id>`.
- `BUSY`: the job queue is full (`--queue`). Nothing was queued; try later.

Vectors come from exactly one of: `random=N` (1 to 1,000,000 vectors from
`std::mt19937_64` seeded with `S`, default 1, same vectors as
`faultline faultsim --random N --seed S`), or the body (vector lines as in
SIM, checked before the job is queued). `threads` (1 to 64, default 1) is the
number of threads that job's fault simulation uses. `details=1` adds one line
per fault to the result.

```
> FAULTSIM c432 random=1000 seed=7 threads=4 details=1
< JOB 1
> FAULTSIM c17\n10101\n01000\n
< JOB 2
> FAULTSIM c7552 random=50000
< BUSY
> FAULTSIM c17 random=10 threads=0
< ERR threads must be between 1 and 64
```

### RESULT `<id>`

Never blocks: a connection thread never waits on a job, so it keeps reading
its socket and notices at once if the client goes away. Clients poll
(the Python client's `wait_result()` polls every 50 ms).

```
> RESULT 1
< PENDING running                  (or PENDING queued)
> RESULT 1
< OK
  vectors:    1000
  faults:     392
  detected:   389
  undetected: 3
  coverage:   99.23%
  time_ms:    1.731 (4 threads)
  D 1 SA0 2                        (details=1 only: D <net> SA<v> <first detecting vector>)
  U 223 SA1                        (details=1 only: U <net> SA<v>, undetected)
  ...
> RESULT 3
< ERR job 3 was cancelled          (or: ERR job 3 failed: <message>)
```

The summary lines and the `D`/`U` lines have the same format and order as
`faultline faultsim --detections --undetected` (the CLI also prints the
circuit path and a note on the fault model).

### STATUS `[id]`

Without an id: server counters. `connections` counts open connections
(including the asking one); the job counts are over the jobs the server
still remembers.

```
> STATUS
< OK connections=2 circuits=1 queued=1 running=1 done=4 cancelled=0 failed=0
```

With an id: the job's state, one of `queued`, `running`, `done`,
`cancelled`, `failed`; a summary when done, the message when failed.

```
> STATUS 1
< OK job=1 state=done vectors=1000 faults=392 detected=389 coverage=99.23% time_ms=1.731
> STATUS 5
< OK job=5 state=running
> STATUS 9
< ERR unknown job 9
```

Finished jobs are forgotten after 256 more jobs have finished, so a server
that runs for a long time does not grow without limit.

### CANCEL `<id>`

Requests a stop. A queued job becomes `cancelled` at once and never runs. A
running job stops at its next block of 64 vectors and then becomes
`cancelled`. Measured by `test_cancel_long_job` (2026-11-08, c7552, 1
thread): 61 ms in the debug build, 590 ms under TSan. Any connection may
cancel any job.

```
> CANCEL 5
< OK
> CANCEL 5                         (after it stopped)
< ERR job 5 is already cancelled
> CANCEL 77
< ERR unknown job 77
```

### QUIT

```
> QUIT
< OK bye                           (then the server closes the connection)
```

Closing the socket without QUIT is fine too.

## Connections

- **Cap.** Over `--max-connections` open connections, a new connection gets
  the single frame `ERR server full` and is closed. (A client that already
  sent a request before reading may see a connection reset instead, because
  the server closes with unread data.)
- **Disconnect cancels jobs.** When a connection ends for any reason (QUIT,
  close, reset, protocol violation), every queued or running job it
  submitted is cancelled.
- **Shutdown.** On SIGINT/SIGTERM the server stops accepting, cancels all
  jobs, closes every connection (a client sees end of stream) and exits.

## Example session (Python client)

```
$ build/release/faultline_server --port 5555 &
listening on 127.0.0.1:5555
$ client/faultline_client.py --port 5555 load c17 benchmarks/iscas85/c17.bench
OK c17 gates=6 inputs=5 outputs=2 depth=3
$ printf '10101\n00000\n' > v.txt
$ client/faultline_client.py --port 5555 sim c17 v.txt
11
00
$ client/faultline_client.py --port 5555 faultsim c17 --random 100
JOB 1
vectors:    100
faults:     22
...
$ client/faultline_client.py --port 5555 status
connections=1 circuits=1 queued=0 running=0 done=1 cancelled=0 failed=0
```

The `faultsim` command waits for its result on the same connection, because
closing that connection would cancel the job. To watch or cancel it, use a
second terminal while it waits:

```
terminal 1$ client/faultline_client.py --port 5555 load c7552 benchmarks/downloaded/c7552.bench
terminal 1$ client/faultline_client.py --port 5555 faultsim c7552 --random 200000
JOB 2
terminal 2$ client/faultline_client.py --port 5555 status 2
job=2 state=running
terminal 2$ client/faultline_client.py --port 5555 cancel 2
OK
terminal 1 prints: error: job 2 was cancelled
```

Pressing Ctrl-C in terminal 1 instead closes its connection, which cancels
the job too.

The library side (`FaultlineClient` in `client/faultline_client.py`) has one
method per command; `request(text)` sends any raw payload and returns the
raw reply. `ERR` replies raise `ServerError`, `BUSY` raises `ServerBusy`.
