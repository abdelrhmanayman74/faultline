# faultline design notes

Part 1 covers parsing, the netlist graph, levelization and scalar simulation.
Part 2 adds bit-parallel simulation and single-threaded stuck-at fault simulation.
Part 3 adds a bounded queue, a thread pool and parallel fault simulation.
Part 4 adds a TCP job server, its protocol and a Python client.

## Netlist (`include/faultline/netlist.hpp`)

- **One index for a net and its driver.** In `.bench` every net is driven by
  exactly one gate, and a primary input is modelled as an `INPUT` pseudo-gate.
  So `NetId` (a `std::size_t`) indexes both. No separate net table is needed.
- **Flat `std::vector<Gate>`.** Each `Gate` stores its type, its fan-in as
  `NetId`s (file order) and its fan-out (computed by `finalize()`). Indices
  instead of pointers: the vector can grow while parsing without invalidating
  anything, and copies of a netlist stay valid.
- **Name map only for parsing.** `std::unordered_map<std::string, NetId>`
  turns names into indices. Simulation never touches strings.
- **Evaluation order is stored.** `finalize()` runs levelization once and
  keeps `eval_order()` (topological order) and each gate's `level`. Any
  simulator just walks that vector with its own per-net value array of size
  `netlist.size()`. The bit-parallel and fault simulators of Part 2 can reuse
  the same order with `std::uint64_t` per net instead of one byte.

## Parser (`src/bench_parser.cpp`)

- Two passes: pass 1 turns lines into statements (syntax only); pass 2
  resolves names. Two passes are needed because `.bench` may use a net before
  the line that defines it.
- Accepted: `#` comments, blank lines, any spaces, CRLF line ends (the
  downloaded ISCAS files use CRLF), case-insensitive keywords, `BUF` and `BUFF`,
  gates with any number of inputs (at least 1; `NOT`/`BUF` exactly 1).
- Rejected with `file:line: message`: undefined net, duplicate definition,
  unknown gate, wrong arity for `NOT`/`BUF`, output never driven, output
  declared twice, malformed line, `DFF` ("sequential circuits not supported
  yet"), combinational cycle.
- Errors are exceptions (`ParseError`). Why: a bad file stops the whole job,
  and an exception lets the small helper functions stop without passing
  error codes up through every return. The reasoning is in `errors.hpp`.

## Levelization (`src/levelize.cpp`)

- Kahn's algorithm: start with nets that have no fan-in (the inputs), place a
  gate when all of its fan-in is placed. The output vector doubles as the FIFO
  queue. O(nets + connections).
- `level = 1 + max(level of fan-in)`, inputs are level 0. `depth()` is the
  largest level (c17: 3, c6288: 124).
- Cycle detection: if some nets are never placed there is a loop. Every
  unplaced net has an unplaced fan-in, so walking backwards through unplaced
  fan-ins must revisit a net; the revisited part is the loop. `CycleError`
  names those nets in signal-flow order, and the parser adds the line number.

## Scalar simulator (`src/scalar_simulator.cpp`)

- One value per net in a `std::vector<std::uint8_t>` (0 or 1). Not
  `std::vector<bool>`: that specialization packs bits and returns proxy
  objects, so every access is a shift and mask and it cannot be viewed as a
  `std::span`. One byte per net is simpler and the layout matches the later
  word-per-net bit-parallel simulator.
- One pass over `eval_order()` gives the settled zero-delay outputs.
- Gate pairs share a base function (AND/NAND, OR/NOR, XOR/XNOR), and the
  inverting gate flips it. Multi-input XOR is parity, as in Verilog.

## Verification (Part 1)

- GoogleTest: parser (accepted syntax and every error case), levelization
  (order valid, levels, cycles), simulator (c17 full truth table checked against
  equations simplified by hand, every gate type, vector file reader).
- `tools/ref_eval.py`: an independent Python evaluator. It shares no design
  with the C++ code: regular-expression parser, demand-driven memoized
  recursion from each output backwards.
- `tools/iverilog_check.py`: converts the netlist to structural Verilog
  (`tools/bench_to_verilog.py`), simulates seeded random vectors in Icarus
  Verilog, and requires `faultline sim`, `ref_eval.py` and iverilog to agree on
  the settled outputs of every vector. Result: `docs/results/part1_iverilog_check.txt`.

## Bit-parallel simulator (`src/bit_parallel_simulator.cpp`)

- **64 vectors per word.** Each net gets one `std::uint64_t`; bit k is the
  net's value under vector k of the block ("lane" k). One `&` on two words
  evaluates an AND gate for 64 vectors, so a block costs about as much as one
  scalar vector. Same `eval_order()` walk as the scalar simulator.
- **`PatternBlock`.** `pack_vectors()` transposes the vector list into blocks:
  one word per primary input, plus `first_vector` (index of lane 0) and
  `count` (valid lanes). Only the last block can be partial; its unused lanes
  are padded with 0, and `valid_mask()` (lanes `[0, count)`) is ANDed into
  every result so padding can never show up as an output or a detection.
- **Inversion uses `~`.** The scalar code inverts with `^ 1`, which on a word
  would flip lane 0 only. The word evaluator is a separate function, not a
  template shared with the scalar one, so the scalar simulator stays an
  independent reference.
- **The scalar simulator stays.** `faultline sim` uses it by default;
  `faultline sim ... --bitparallel` uses the new one and prints the same text.

## Fault model (`include/faultline/fault_simulator.hpp`)

- **Single stuck-at, stem faults only.** The list has stuck-at-0 and
  stuck-at-1 on every net, primary inputs included: `2 * nets` faults, ordered
  by `NetId`. A stem fault sits on the driving gate's output, so every reader
  of the net sees the stuck value.
- **Fan-out branch faults are NOT modelled.** When a net feeds several gates,
  each connection is a separate physical site, and a fault there affects only
  that one reader. Such a branch fault can need a different test than its
  stem, or be redundant when the stem is not (and the reverse), mostly where
  fan-out reconverges. Consequences for the numbers: the fault totals are
  smaller than a full stem+branch list (c17 has 11 stems and 6 branch sites,
  so 22 stem faults versus 34 with branches), and the coverage percentages
  are coverage of the stem list only. They are not comparable with published
  coverage figures, which normally use a collapsed list that includes branch
  faults. A vector set with 100% stem coverage is not guaranteed to detect
  every branch fault.
- **No fault collapsing.** Equivalence collapsing (for example, an AND input
  stuck-at-0 equals its output stuck-at-0) is defined on gate inputs, which
  are branch sites; with a stem-only list it only applies where a net has a
  single reader. Rather than a half-correct rule, every stem fault is kept and
  simulated. Collapsing would cut run time and shrink the denominator of the
  coverage percentage; it would not change which faults are detected.

## Fault simulator: PPSFP (`src/fault_simulator.cpp`)

Parallel-pattern single-fault propagation: one fault at a time, 64 vectors at
a time.

1. For each block, `BitParallelSimulator` computes the good-circuit words once.
2. `FaultSimulator::simulate_block()` copies them into its own `faulty_`
   array, then for each fault that is not yet detected:
   - **Activation check.** If the good word already equals the stuck value in
     every valid lane, the faulty circuit equals the good one: skip.
   - **Force and re-evaluate.** Set the faulty net's word to all-0 or all-1,
     then re-evaluate every gate that comes after that net in `eval_order()`.
     The faulty net itself is never re-evaluated, so the stuck value holds.
   - **Compare.** OR together `faulty ^ good` over the primary outputs, AND
     with `valid_mask()`. Any set bit means detected; the lowest set bit
     (`std::countr_zero`) gives the first detecting vector of the block.
   - **Restore.** Copy the good words back over the positions just written,
     so `faulty_` equals the good values again for the next fault.
3. **Fault dropping.** A detected fault is skipped in all later blocks. Blocks
   run in vector order, so the first detection found is the earliest vector.
   `FaultSimReport::fault_simulations` counts (fault, block) pairs actually
   simulated; a test checks it to prove dropping happens. The run stops early
   once every fault is detected.

**Why the suffix of `eval_order()` and not the exact fan-out cone?** Every
gate a fault can reach comes after the faulty net in any topological order,
so re-evaluating the suffix is correct by construction and needs no extra
data. Gates in the suffix but outside the cone just recompute their good
value: wasted work, but not wrong. An event-driven cone walk would skip them
but needs a level-ordered queue and per-gate "changed" flags. That is a later
optimization if profiling shows it matters (Part 2 times are in
`docs/results/part2_coverage.txt`).

**Ready for threads (Part 3).** `simulate_block()` reads the netlist, the
block and the good words (all read-only during a block) and writes only the
`FaultStatus` entries it was handed plus its private `faulty_` array. So
workers can each own a `FaultSimulator` and take disjoint fault ranges
(`std::span` slices of the fault and status arrays) with no locking. The
shared state is the good-value array per block (written once before the
workers start, then read-only) and the status array (each element written by
exactly one worker).

## Concurrency (Part 3)

### BoundedQueue (`include/faultline/bounded_queue.hpp`)

A fixed-capacity FIFO: `std::deque<T>`, one `std::mutex`, two
`std::condition_variable`s (`not_full_`, `not_empty_`) and a `closed_` flag.

- **Blocking and non-blocking.** `push()` waits while full, `pop()` waits
  while empty. `try_push()` never waits and returns `PushResult::Ok`, `Full` or
  `Closed`, so a server can answer "busy" (backpressure) instead of letting a
  client hang.
- **Predicate waits.** Every wait is `cv.wait(lock, predicate)`, i.e. a loop
  that re-checks the condition under the lock. Needed because of spurious
  wakeups and because another thread can take the slot or item between the
  notify and the moment the woken thread gets the mutex.
- **Two condition variables.** Producers and consumers wait for different
  things. With one shared variable, `notify_one()` can wake a thread of the
  wrong kind, which goes back to sleep while the right kind is never told
  (a stall), so one would need `notify_all()` and wake everyone each time.
- **Notify after unlocking.** The state change is made under the lock and the
  waiter re-checks under the lock, so no wakeup can be lost. Notifying after
  the unlock only avoids waking a thread straight into a mutex we still hold.
- **`close()`** sets the flag and `notify_all()`s both variables. Producers
  then fail; consumers drain what is left, then `pop()` returns
  `std::nullopt`, which is the workers' exit signal. No sentinel items needed.

### ThreadPool (`include/faultline/thread_pool.hpp`, `src/thread_pool.cpp`)

- `N` `std::jthread` workers loop on `queue_.pop()` until it returns
  `nullopt`. `std::jthread` rather than `std::thread`: it joins in its
  destructor, so no error path can leave a joinable thread (which would call
  `std::terminate`). Its built-in stop token is not used; see cancellation.
- **Jobs are `std::packaged_task<void()>`** built from a
  `std::function<void()>`. `submit()` returns the `std::future<void>`. If a
  job throws, the packaged task stores the exception in that future, so the
  worker keeps running and `future.get()` rethrows it in the caller. This
  needs no extra code in the worker loop. Results are written through what
  the job captured. The queue holds the packaged task by move (it is
  move-only), which is why `BoundedQueue` only ever moves items.
- `submit()` blocks while the queue is full and throws after shutdown;
  `try_submit()` returns `std::nullopt` when the queue is full or closed.
- **`shutdown()`** closes the queue, lets the workers finish every queued job
  and joins them. `std::call_once` makes it idempotent and safe to call from
  several threads; the destructor calls it. It must not be called from inside
  a job (a worker cannot join itself).
- **Cancellation is cooperative and owned by the caller.** A job captures a
  `std::stop_token` from a `std::stop_source` the caller keeps; the job checks
  `stop_requested()` at safe points and returns or throws. A thread cannot be
  stopped safely from outside (it may hold a lock or be half way through a
  write). A per-job source, not a pool-wide one, is what a server needs to
  cancel one client's job.
- **Do not wait on the pool from inside the pool.** If every worker blocks
  on futures of jobs still in the queue, nothing runs them (deadlock). That
  is why `run_fault_simulation_parallel()` creates its own pool for the call.

### Parallel fault simulation (`src/parallel_fault_simulator.cpp`)

Faults are independent of each other: whether fault *f* is detected by
vector *v* depends only on the netlist and *v*. So the fault list is split
into chunks of consecutive faults, and each chunk is one pool job that runs
the whole single-thread algorithm on its range: its own
`BitParallelSimulator` (good values), its own `FaultSimulator` (`faulty_`
scratch array), all blocks in vector order, fault dropping, and its own early
stop when all faults of the chunk are detected.

**Two designs were considered.**

(a) *Per-block barrier.* One thread computes the good values of a block, all
workers simulate their fault ranges against those shared values, everybody
waits at a barrier, next block. Good simulation is done once per block.

(b) *Independent chunks (implemented).* Each job walks every block with its
own good simulator. Good simulation is repeated once per chunk, but there is
no barrier: the only synchronization is the final wait for the futures.

Why (b): the repeated work is small and (a) pays at every block. The
benchmark measures good-circuit simulation of all blocks on its own: with
1024 vectors it is 0.2% of the serial fault simulation time for c7552 and
under 1% for every circuit from c1908 up (`docs/results/part3_benchmarks.md`,
column "good sim only"). At 12 threads (48 chunks) that is about 2 ms of a
39 ms c7552 run. With (a), each of the 16 blocks ends with every thread
waiting for the slowest range of that block, and fault dropping makes the
per-block work shrink and become uneven, so idle time is paid 16 times
instead of once. (a) also adds a serial step (the good simulation) before
every block. Not built and measured; this is the reasoning. (b) also has a simpler early stop: a chunk stops when *its* faults
are all detected, without summing a counter across workers. (a) would win
when good simulation is a large share of the work, i.e. many vectors but
very few undetected faults per chunk; then shared good values (or fewer,
larger chunks) are the better choice.

**Chunking.** `min(faults, 4 * threads)` chunks (`kChunksPerThread`), sizes
differ by at most one fault. 4 was measured against 1 and 16 (table in
`docs/results/part3_benchmarks.md`): 1 balances badly, 16 is slightly slower. Cost per fault varies a lot: a fault near the
inputs re-evaluates a long suffix of `eval_order()`, one near the outputs a
short one, and hard-to-detect faults survive every block. More chunks than
threads lets a worker that finished cheap chunks take another one from the
queue (dynamic load balancing); not many more, because each chunk repeats the
good simulation.

**What is shared and what is owned.**

| data | owner / sharing | written by | synchronization |
|---|---|---|---|
| `Netlist`, `blocks` (packed vectors) | shared | nobody during the run | none needed: read-only after the jobs start |
| `report.faults` | shared, each job sees a `std::span` slice | nobody during the run | none |
| `report.status` | each job gets a disjoint `std::span` slice | exactly one job per element | none during the run; `future.get()` at the end |
| `simulations[c]` | element `c` belongs to job `c` | job `c` once | `future.get()` |
| good values, `faulty_`, `position_` | owned by the job's simulators | that job | none |
| queue of jobs | the pool | `submit()` / workers | the queue's mutex |
| `std::stop_token` | copied into every job | `request_stop()` by the caller | atomic inside `std::stop_source` |

**Why it is race-free.** A data race needs two threads touching the same
memory location with at least one write and no ordering between them. During
the run the only shared memory that is written is `report.status`, and every
element belongs to one slice and therefore one job. Neighbouring elements
written by two jobs are different memory locations (`FaultStatus` is a
struct with a real `bool`; a `std::vector<bool>` would pack several flags
into one byte and *would* race). The thread that collects the results reads
them only after `future.get()`, and a packaged task finishing
"happens-before" `get()` returning. The pool's queue is protected by its
mutex. TSan reports nothing on the tests below.

**Why the result is identical to the single-thread run.** Each fault's
status depends only on its own simulation, never on which thread or which
chunk ran it. Within a chunk, blocks run in vector order, so the first block
that detects a fault is the earliest one, as in the serial loop; the lowest
set lane in that block is the earliest vector. `fault_simulations` (sum of
"faults still undetected" over blocks) is a sum over faults, so splitting it
by chunk does not change the total. Tests compare the full status array, the
fault list and `fault_simulations` with `run_fault_simulation()` for 1, 2, 3,
4 and 8 threads on c17 and on every downloaded ISCAS-85 circuit.

**Cancellation.** `run_fault_simulation_parallel(..., std::stop_token)`.
Each job checks the token before every block; once it is set the job throws
`Cancelled` (stored in its future). The coordinator first `wait()`s for
*every* future and only then calls `get()`, which rethrows `Cancelled`, so no
job can still be using the local data when the function exits. All data the
jobs touch is declared before the pool for the same reason: locals are
destroyed in reverse order, so the pool (which joins) goes first. Latency of
a cancel: one block (64 vectors) of one chunk. A partial report is never
returned. A stop requested after the last job has finished is ignored, since
the result is already complete.

**Threads in the CLI.** `faultline faultsim ... --threads N`. `1` (default)
runs the single-thread `run_fault_simulation()`; `0` means
`std::thread::hardware_concurrency()`.

## CLI (Part 2)

```
faultline sim <file.bench> <vectors.txt> [--bitparallel]
faultline faultsim <file.bench> (--vectors <file> | --random N [--seed S]) [--threads N]
                   [--detections] [--undetected]
faultline_bench [--runs N] [--vectors N] [--seed S] [--scalar-max-gates G] <file.bench>...
```

`faultsim` prints vectors, fault count, detected, undetected, coverage and the
fault-simulation time (parsing and vector generation excluded).
`--detections` adds `D <net> SA<v> <first vector>` lines, `--undetected` adds
`U <net> SA<v>` lines. `--random` uses `std::mt19937_64`, which the C++
standard fully specifies, so a seed gives the same vectors everywhere (default
seed 1).

## Verification (Part 2)

- GoogleTest: bit-parallel equals scalar on every net for c17 (all 32
  vectors), every gate type, and 200 random vectors (3 full blocks + 1 partial)
  on each downloaded circuit (skipped if the file is missing); block packing
  and valid masks; c17 faults worked by hand (`10 SA1` first detected by
  vector 20 = `10100`, `22 SA0` by vector 8 = `01000`); fault dropping (first
  vector is kept, `fault_simulations` count); a detection that exists only in
  a padding lane is ignored, and found once that lane becomes valid.
- `tools/ref_faultsim.py`: naive oracle. For every fault and every vector it
  evaluates the whole faulty circuit (demand-driven recursion, faulty net
  pre-set in the memo table) and compares with the good outputs. No
  bit-parallelism, no levelization, no partial re-evaluation.
- `tools/faultsim_check.py`: runs the CLI and the oracle on the same vectors
  and requires identical fault lists, detected sets and first-detecting
  vector indices. ctest runs it on c17; the full run is in
  `docs/results/part2_faultsim_check.txt`.

## Verification (Part 3)

- GoogleTest, `BoundedQueue`: FIFO order; `try_push` returns `Full`; a
  blocked `push` stays blocked until a `pop`; `close()` wakes a blocked `pop`
  (gets `nullopt`) and a blocked `push` (gets `false`) and keeps queued items;
  4 producers x 4 consumers x 100,000 items through 16 slots, every item
  received exactly once and in per-producer order. Blocking is checked with a
  helper thread and `future::wait_for`: a short wait that must time out ("still
  blocked") and a long one that must succeed ("woken"), so a slow machine
  cannot make the tests fail wrongly.
- `ThreadPool`: 1000 jobs all run; a throwing job reaches its future and the
  single worker keeps running; `try_submit` fails when the queue is full;
  `shutdown()` drains queued jobs, is idempotent, and is safe from two threads
  at once; a stop token ends a long job.
- Parallel fault simulation: identical fault list, status array and
  `fault_simulations` to the serial run for 1, 2, 3, 4 and 8 threads, on c17
  (3 and 200 vectors) and on all ten downloaded circuits (130 vectors: two
  full blocks and a partial one); stop before the start and during a long
  c7552 run both throw `Cancelled`.
- All tests pass under the `debug`, `asan` (address + undefined) and `tsan`
  presets; the TSan suite was run repeatedly with no report. A deliberate
  data race built with the same compiler and launcher is reported by TSan,
  so the clean result means something.
- `faultline_bench` compares every timed run with the serial result.

## Server (Part 4)

Files: `socket.hpp/.cpp` (RAII descriptor and POSIX socket helpers),
`protocol.hpp/.cpp` (framing and request parsing), `server.hpp/.cpp` (the
`Server` class), `apps/faultline_server.cpp` (options, signals),
`client/faultline_client.py`. The wire format is in `docs/PROTOCOL.md`.

### Sockets (`include/faultline/socket.hpp`)

- **`Socket` owns one file descriptor** and closes it in its destructor.
  Move-only: a copy would close the same number twice, and after the first
  `close()` the kernel may already have given that number to another file.
  Moving leaves the source at `-1`.
- **`send_all`** loops because `send()` may accept only part of the buffer
  when the kernel's send buffer is full, and retries on `EINTR`.
  `MSG_NOSIGNAL` turns "peer has closed" into an `EPIPE` error instead of
  `SIGPIPE`, whose default action would kill the whole server because of one
  client.
- **`recv_exact`** loops because `recv()` returns whatever has arrived (one
  byte of a 4-byte header is legal). It returns the byte count, so the caller
  can tell a clean close (`recv` returned 0) before a frame, a close in the
  middle of a frame, and real errors (thrown as `std::system_error` with
  `errno`).
- `listen_tcp` sets `SO_REUSEADDR` (a restarted server can bind again while
  old connections sit in `TIME_WAIT`), returns a non-blocking socket (accept
  happens only after `poll()` says a connection waits; if it vanished
  meanwhile, `accept` returns `EAGAIN` instead of blocking the loop), and
  binds port 0 for tests, read back with `getsockname`. IPv4 literals only
  (`inet_pton`), which keeps it free of resolver code. All descriptors are
  `CLOEXEC`.

### Framing (`src/protocol.cpp`)

4-byte big-endian length + payload, built in one buffer and sent with one
`send_all`. `read_frame` checks the length against the 16 MiB limit before
allocating, so a hostile header cannot make the server allocate gigabytes.
A violation (too long, or the connection closes mid-frame) throws
`ProtocolError`; the connection thread answers `ERR ...` and closes, because
after a bad length nobody knows where the next frame starts. A bad *request*
inside a good frame (unknown command, bad argument, parse error) only gets
an `ERR` reply; the connection stays usable. `parse_request` checks the
command name and argument count from one table, `parse_faultsim_args` the
`key=value` options and their ranges, so `Server` code only sees valid input.

### Threads and what each one owns

| thread | count | owns | blocks in |
|---|---|---|---|
| main (`Server::run`) | 1 | listening socket, the `connections_` list structure (adds, reaps, joins) | `poll()` on the listening socket and the signalfd |
| connection | one per client, at most `--max-connections` | its `Socket` (closes it), its request/reply loop | `recv()` in `read_frame` |
| job worker | `--job-workers` | the job it is running | the job queue's `pop()` |
| simulation | `threads=` of each running job | one chunk of faults (Part 3) | its own pool |

Shared state, each with its own `std::mutex`:

| state | guarded by | used by |
|---|---|---|
| `circuits_` (name -> `shared_ptr<const Netlist>`) | `circuits_mutex_` | LOAD writes; SIM, FAULTSIM, STATUS read |
| `jobs_`, `finished_jobs_`, every `Job`'s `state/summary/result`, `stopping_` | `jobs_mutex_` | connection threads, workers, shutdown |
| `connections_`, each `Connection::finished` | `connections_mutex_` | main thread, each connection thread at its end, STATUS |
| `Job::stop` (`std::stop_source`) | none needed: `request_stop()` is thread-safe | CANCEL, disconnect, shutdown |

**No deadlock:** no code holds two of these mutexes at once. The one nesting
is `jobs_mutex_` -> the pool's queue mutex inside `try_submit`; the workers
release the queue mutex before running a task that takes `jobs_mutex_`, so
the opposite order never happens.

**Registry: why a plain mutex and not `std::shared_mutex`.** Parsing happens
before taking the lock; readers hold it only to copy one `shared_ptr`
(a few nanoseconds). A reader/writer lock helps when readers hold the lock
for a long time and would otherwise wait for each other; here they do not,
and `shared_mutex` costs more per lock. The `shared_ptr<const Netlist>` does
the real work: a SIM or job keeps its own reference, so it can keep using a
netlist after the lock is released, even if a LOAD replaces that name, and
`const` means nobody can modify a netlist that other threads read.

### Connection lifecycle

- The main thread accepts, reaps finished connections (moves them out of the
  list under the lock, then lets their `std::jthread`s join *outside* the
  lock, since a finishing thread needs that lock to say it is finished), then
  counts the remaining open ones. Over the cap it sends `ERR server full` and
  closes. Otherwise it adds a `Connection` to a `std::list` and only then
  starts its thread: the thread keeps a reference to the element, and list
  elements never move (`splice` relinks nodes without moving them either).
- The connection thread loops: read a frame, `handle()` it, write the reply.
  It catches every exception, because an exception leaving a thread function
  calls `std::terminate`. On the way out it cancels its jobs, then, under
  `connections_mutex_`, closes its socket and sets `finished`.
- **Who closes the fd.** Only the connection thread closes its socket, and
  only under `connections_mutex_`; the main thread touches that fd (with
  `shutdown()`) only under the same mutex and only if `finished` is false.
  Otherwise the main thread could call `shutdown()` on a number that was just
  closed and reused for another client's socket.
- No thread is detached: every connection thread is joined, either when it
  is reaped or at shutdown.

### Job lifecycle

```
FAULTSIM -> queued --worker picks it up--> running --> done
              |                              |   \---> failed (exception)
              +-------- CANCEL/disconnect ---+---> cancelled
```

- **Submit.** The connection thread parses the arguments and (if given) the
  vector body, so mistakes are reported in the reply. Under `jobs_mutex_` it
  calls `ThreadPool::try_submit()` (never blocks) and records the job only if
  that succeeded: when the queue is full the client gets `BUSY` immediately
  instead of its connection thread blocking (backpressure). Submitting and
  recording under one lock means a worker that starts the job at once waits
  for the lock and then finds the record.
- **Ownership.** The task holds `shared_ptr`s to its `Job` and its netlist,
  so both live as long as the task, even if the record is forgotten or the
  name reloaded. The `std::future` from `try_submit` is dropped: `run_job`
  catches every exception, and a `packaged_task` future (unlike one from
  `std::async`) does not block in its destructor.
- **Run.** The worker marks the job `running` (or returns if it was cancelled
  while queued), generates random vectors if asked, and calls
  `run_fault_simulation_parallel(..., job.stop.get_token())`. That function
  makes its own pool for the call; running the chunks on the server's job
  pool and waiting for them from a job could deadlock (Part 3, "do not wait
  on the pool from inside the pool").
- **Results by polling.** `RESULT <id>` returns `PENDING` until the job is
  finished. A blocking RESULT would park the connection thread on a condition
  variable instead of in `recv()`, and then it could not notice that its
  client had gone. Polling keeps one rule: a connection thread only ever
  waits on its own socket.
- **Memory.** Finished jobs stay queryable; the oldest are dropped once more
  than 256 have finished (trimmed when a job is submitted, never while
  another loop iterates over the map).

### Cancellation

Each job has its own `std::stop_source`; one source per job is what lets one
client's job stop without touching anyone else's.

- `CANCEL` on a queued job: `request_stop()` and state `cancelled` at once;
  the worker skips it when it reaches it. On a running job:
  `request_stop()`; the simulation checks the token before each block and
  throws `Cancelled`, and the worker records `cancelled`. Latency is one block
  (64 vectors) of one chunk: on c7552 in `test_cancel_long_job`, 20-62 ms
  release, 81-231 ms debug, 649-673 ms TSan
  (`docs/results/part5_verification.txt`). Random vector generation is not interruptible
  (fast compared with the simulation).
- **Client disconnect.** The connection thread sees end of stream or an
  error from `recv()`/`send()`, then cancels every unfinished job whose
  `owner` is its connection id. A client that disappears without a word
  (cable pulled, no FIN/RST) is only noticed when TCP gives up; there is no
  idle timeout (see limitations).

### Shutdown (SIGINT/SIGTERM)

`main()` blocks SIGINT and SIGTERM with `pthread_sigmask` **before** the
`Server` (and so any thread) exists; threads inherit the mask, so no thread is
ever interrupted by these signals. A blocked signal stays pending, and a
`signalfd` makes "pending" readable. `Server::run()` polls the listening
socket and the signalfd together. Why this and not the alternatives:

- a handler that sets a flag: `accept()` and `recv()` in other threads would
  not wake up reliably, and the handler may only touch `volatile
  sig_atomic_t`/lock-free atomics and async-signal-safe functions;
- the self-pipe trick (handler writes a byte to a pipe that `poll()`
  watches) works everywhere, but needs a handler and a global descriptor;
- `signalfd`: no handler, no global, no async-signal-safety rules, one
  extra `pollfd`. Linux-only, which this project accepts.

Then `shutdown()`, in this order:

1. Close the listening socket: new connections are refused.
2. Under `jobs_mutex_` set `stopping_` (a FAULTSIM arriving now gets `ERR
   server is shutting down`), then cancel every job: queued ones become
   `cancelled`, running ones get `request_stop()`.
3. Under `connections_mutex_`, call `::shutdown(fd, SHUT_RDWR)` on every open
   connection. A thread blocked in `recv()` gets end of stream; a thread busy
   with a request finishes it and its `send()` fails. `shutdown()` instead of
   `close()`: the descriptor stays valid until its owning thread closes it.
4. Join the connection threads (outside the lock).
5. `pool_.shutdown()`: queued tasks see `cancelled` and return, running jobs
   throw `Cancelled` within one block, workers are joined.

`test_sigterm_stops_running_job_quickly` checks a SIGTERM during a long job
exits with status 0 well within the limit. `pool_` is the last member, so
even without `shutdown()` it would be destroyed (and joined) before the
members its jobs use.

### Why thread-per-connection with a cap

Each connection's code is a plain blocking loop (read, handle, write) with
its state in local variables, which is easy to read and to reason about. Its
costs are a thread (stack memory, scheduling) per client, which is fine for
tens of clients; the cap keeps a flood of connections from creating
thousands of threads. Heavy work does not run on connection threads anyway:
FAULTSIM goes to the bounded job pool, so the CPU-bound part is limited by
`--job-workers` and `--queue`, not by the number of clients.

**What epoll would change (future work).** One or a few threads would wait
in `epoll_wait()` on all sockets, which are non-blocking. Each connection
would need an explicit state machine (reading header / reading payload /
writing reply, with partial buffers kept between events), because a
half-read frame can no longer live on a thread's stack. That scales to many
thousands of mostly idle connections with a fixed number of threads, at the
cost of more complex code. Long requests (SIM) would have to move to the
worker pool so they do not stall the event loop.

### Limitations

- No idle timeout: a client that connects and sends nothing keeps a slot
  (and a thread) until it disconnects. `SO_RCVTIMEO` or `poll()` with a
  timeout in the connection loop would fix it.
- `accept()` failing with `EMFILE` (out of descriptors) ends `run()` with an
  error (clean shutdown, exit status 1) instead of backing off.
- IPv4 only; no TLS, no authentication. The default host is 127.0.0.1 for
  that reason.
- SIM runs on the connection thread; a huge SIM keeps that thread busy, and
  its reply is limited to 16 MiB.
- RESULT is polled; clients wait at most one poll interval longer than needed.
- A queued job keeps its parsed vector body as one `std::vector` per vector,
  several times the size of the text for small circuits; with a full queue
  of 16 MiB bodies that is a lot of memory. Packing vectors into
  `PatternBlock`s at submit time would bound it.

### Verification (Part 4)

- GoogleTest (`tests/test_socket.cpp`, `tests/test_protocol.cpp`): `Socket`
  closes on destruction and on move-assignment, moved-from is invalid;
  `send_all`/`recv_exact` move 4 MiB through a `socketpair` (many partial
  sends/receives) unchanged; `recv_exact` returns a short count on close;
  sending to a closed peer throws `EPIPE` and does not kill the process;
  TCP listen on port 0 + connect; frame header bytes; several frames on one
  stream; a frame fed one byte at a time; oversize encode and oversize
  header rejected; truncated header and truncated payload rejected; request
  splitting, unknown commands, wrong argument counts, every FAULTSIM option
  error.
- Python (`tests/python/test_server.py`, ctest `server_integration`, 14
  tests) against the real binary on port 0: LOAD + SIM equal the CLI on all
  32 c17 vectors; parse errors carry the line; bad requests keep the
  connection usable; FAULTSIM on c432 (1000 random vectors, seed 7, 4
  threads) gives the same summary and the same per-fault first detecting
  vector as `faultline faultsim`; FAULTSIM with a vector body; BUSY with one
  worker and a queue of one; CANCEL of a running c7552 job; disconnect
  cancels the job (seen from a second connection); SIGTERM during a job;
  connection cap; oversize frame; truncated frame; byte-by-byte frames;
  8 concurrent clients doing LOAD, SIM and FAULTSIM at once. Each test stops
  its server with SIGTERM and requires exit status 0 and no "Sanitizer" in
  stderr.
- The whole ctest suite passes in the `debug`, `asan` and `tsan` presets, so
  the server runs under ASan/UBSan and TSan in the integration tests. A race
  injected on purpose into `Server::handle` made the TSan server exit with
  status 66 and the test fail, so a clean run is meaningful.

## Building

```
python3 -m venv .venv && .venv/bin/pip install cmake ninja
export PATH="$PWD/.venv/bin:$PATH"
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
tools/fetch_benchmarks.sh   # optional: downloads the larger ISCAS-85 circuits
build/debug/faultline_server --port 5555 &   # then: client/faultline_client.py --port 5555 ...
```

The `tsan` preset runs tests under `setarch <arch> -R` (address randomization
off) because g++ 11's ThreadSanitizer aborts on this kernel otherwise; see the
comment in `CMakeLists.txt`.
