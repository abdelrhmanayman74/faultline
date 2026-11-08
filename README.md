# faultline

A parallel stuck-at fault simulator for ISCAS-85 gate-level netlists, served
over a multithreaded TCP job server, and checked against Icarus Verilog and an
independent Python oracle. C++20, Linux.

## Highlights

- **Outputs checked three ways.** `faultline sim`, an independent Python
  evaluator and Icarus Verilog agree on every output of 1000 random vectors
  (all 32 for c17) on all 11 ISCAS-85 circuits.
  [part1_iverilog_check.txt](docs/results/part1_iverilog_check.txt)
- **Fault results checked against a naive oracle.** Fault list, detected set
  and first detecting vector of every fault match a Python per-fault, per-vector
  re-simulation on all 11 circuits (256 vectors), with 1 thread and with 12.
  [part2_faultsim_check.txt](docs/results/part2_faultsim_check.txt),
  [part3_faultsim_check.txt](docs/results/part3_faultsim_check.txt)
- **Bit-parallel fault simulation (64 vectors per word) is 14x to 64x faster**
  than a scalar baseline that re-simulates the whole circuit per fault and
  vector (c1908: 892.84 ms to 13.94 ms).
  [part3_benchmarks.md](docs/results/part3_benchmarks.md)
- **6.17x on 12 threads for c7552** (239.84 ms to 38.87 ms, 1024 vectors) on a
  6-core / 12-thread laptop CPU; 4.75x on 6 threads.
  [part3_benchmarks.md](docs/results/part3_benchmarks.md)
- **Same answer for any thread count**: every timed run and the tests (1, 2, 3, 4,
  8 threads, all circuits) compare the full per-fault result with 1 thread.
  [part3_benchmarks.md](docs/results/part3_benchmarks.md)
- **Sanitizer-clean.** The whole test suite (99 tests, including 14 server
  integration tests against the real binary) passes under ASan + UBSan and
  under TSan. [part5_verification.txt](docs/results/part5_verification.txt)
- **Cancelling a running job** takes effect within one 64-vector block:
  20-62 ms from CANCEL to `cancelled` for a c7552 job in the release build.
  [part5_verification.txt](docs/results/part5_verification.txt)

## Architecture

```
  bench_parser  .bench text, two passes, file:line errors
      v
  Netlist       flat vector of gates, fan-in/fan-out as indices,
      |         levelized evaluation order (Kahn), cycle detection
      +--> ScalarSimulator        one byte per net; the reference
      +--> BitParallelSimulator   one uint64_t per net = 64 vectors
               v
           FaultSimulator         PPSFP: force the fault, re-evaluate the
               |                  suffix of the order, compare outputs, drop
               v
           run_fault_simulation_parallel
               fault list -> 4N chunks -> ThreadPool (N std::jthread,
                                          BoundedQueue, stop_token cancel)

  faultline_server
      main thread: poll(listening socket, signalfd)
          | accept, at most --max-connections
          v
      one thread per connection: LOAD / SIM / STATUS / RESULT / CANCEL
          | FAULTSIM: try_submit, BUSY if the queue is full
          v
      job pool (--job-workers, --queue) -> run_fault_simulation_parallel
```

## Quick start

Prerequisites: Linux (tested on Ubuntu 22.04), g++ 11 or newer, Python 3.10+,
CMake 3.29 or newer and Ninja, `curl` and `unzip` for the benchmark download,
Icarus Verilog optional (`sudo apt install iverilog`). If the system CMake is
older, install both tools into a virtual environment as below.

```sh
python3 -m venv .venv && .venv/bin/pip install cmake ninja
export PATH="$PWD/.venv/bin:$PATH"

tools/fetch_benchmarks.sh        # downloads c432..c7552, checks SHA-256 (optional)
cmake --preset release && cmake --build --preset release
ctest --preset release           # circuit tests skip if the download was skipped
```

Command-line tool:

```sh
build/release/faultline info benchmarks/iscas85/c17.bench
printf '10101\n00000\n11111\n' > c17_vectors.txt
build/release/faultline sim benchmarks/iscas85/c17.bench c17_vectors.txt
build/release/faultline faultsim benchmarks/downloaded/c7552.bench --random 1024 --threads 12
build/release/faultline faultsim benchmarks/iscas85/c17.bench --vectors c17_vectors.txt --detections --undetected
build/release/faultline_bench --runs 5 benchmarks/downloaded/c7552.bench
```

Server and Python client (standard library only; protocol in
[docs/PROTOCOL.md](docs/PROTOCOL.md)):

```sh
build/release/faultline_server --port 5555 &
client/faultline_client.py --port 5555 load c17 benchmarks/iscas85/c17.bench
client/faultline_client.py --port 5555 sim c17 c17_vectors.txt
client/faultline_client.py --port 5555 load c432 benchmarks/downloaded/c432.bench
client/faultline_client.py --port 5555 faultsim c432 --random 1000 --seed 7 --threads 4
client/faultline_client.py --port 5555 status
kill %1                          # SIGTERM: cancels jobs, closes connections, exit 0
```

## Results

1024 random vectors (seed 1), single stuck-at faults on every net stem, no
collapsing. Coverage from [part2_coverage.txt](docs/results/part2_coverage.txt),
times (median of 5 runs, fault simulation only) from
[part3_benchmarks.md](docs/results/part3_benchmarks.md). Measured 2026-11-08 on
an Intel Core i7-8750H (6 cores, 12 threads), Linux 6.8, g++ 11.4 `-O3`.

| circuit | gates | faults | detected | coverage | scalar ms | bit-parallel ms | 12 threads ms | speed-up (12T) |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| c17 | 6 | 22 | 22 | 100.00% | 0.10 | 0.01 | 0.39 | 0.02 |
| c432 | 160 | 392 | 389 | 99.23% | 8.91 | 0.28 | 0.46 | 0.60 |
| c499 | 202 | 486 | 485 | 99.79% | 12.41 | 0.34 | 0.49 | 0.70 |
| c880 | 383 | 886 | 868 | 97.97% | 56.99 | 1.79 | 0.95 | 1.89 |
| c1355 | 546 | 1174 | 1160 | 98.81% | 118.63 | 1.86 | 0.90 | 2.06 |
| c1908 | 880 | 1826 | 1751 | 95.89% | 892.84 | 13.94 | 3.52 | 3.96 |
| c2670 | 1193 | 2852 | 2381 | 83.49% | skipped | 57.72 | 11.03 | 5.23 |
| c3540 | 1669 | 3438 | 3294 | 95.81% | skipped | 46.59 | 8.14 | 5.72 |
| c5315 | 2307 | 4970 | 4944 | 99.48% | skipped | 61.17 | 10.31 | 5.94 |
| c6288 | 2416 | 4896 | 4879 | 99.65% | skipped | 31.70 | 6.07 | 5.22 |
| c7552 | 3512 | 7438 | 7026 | 94.46% | skipped | 239.84 | 38.87 | 6.17 |

"bit-parallel" is the single-thread PPSFP code; speed-up = bit-parallel / 12
threads. Below 1 ms of work, thread start-up dominates (CLI default: 1 thread).
Scaling bends after 6 threads, the number of physical cores. 2/4/6 threads,
16384 vectors and a chunk-size study are in the same file. Coverage counts stem
faults only, so it is not comparable with published collapsed-fault figures.

## Verification

| what | how | where |
|---|---|---|
| parser, levelization, simulators, fault simulator, queue, pool, sockets, framing | GoogleTest unit tests | `tests/*.cpp` |
| logic values | `faultline sim` vs `tools/ref_eval.py` (memoized recursion, no shared code) vs Icarus Verilog on generated structural Verilog | `tools/iverilog_check.py` |
| fault results | CLI vs `tools/ref_faultsim.py` (whole faulty circuit per fault and vector) | `tools/faultsim_check.py` |
| bit-parallel = scalar | every net, random blocks incl. partial ones, all circuits | `tests/test_bit_parallel_simulator.cpp` |
| thread-count independence | full status array equal for 1, 2, 3, 4, 8 threads | `tests/test_parallel_fault_simulator.cpp` |
| server | real binary on port 0: CLI-equal results, BUSY, CANCEL, disconnect, SIGTERM, cap, bad frames, 8 concurrent clients | `tests/python/test_server.py` |
| memory and threads | whole suite under `asan` (address + undefined) and `tsan` presets | `CMakePresets.json` |

ctest runs the oracle checks on c17. The full runs:

```sh
python3 tools/iverilog_check.py --cli build/release/faultline --vectors 1000 --seed 1 \
    benchmarks/iscas85/c17.bench benchmarks/downloaded/c*.bench
python3 tools/faultsim_check.py --cli build/release/faultline --threads 12 --vectors 256 --seed 1 \
    benchmarks/iscas85/c17.bench benchmarks/downloaded/c432.bench benchmarks/downloaded/c880.bench
cmake --preset asan && cmake --build --preset asan && ctest --preset asan
cmake --preset tsan && cmake --build --preset tsan && ctest --preset tsan
```

The naive oracle is slow (500 s for c7552), so ctest uses c17. The `tsan` preset
runs tests under `setarch -R` (g++ 11's TSan aborts on newer kernels otherwise);
a race injected on purpose was reported ([DESIGN.md](docs/DESIGN.md#verification-part-4)).

## Design notes

Details in [docs/DESIGN.md](docs/DESIGN.md) and [docs/PROTOCOL.md](docs/PROTOCOL.md).

- **Flat arrays, indices not pointers.** A net and its driving gate share one
  index; simulators walk a stored topological order over a value array.
- **PPSFP.** One fault at a time, 64 vectors per `uint64_t`; only the gates
  after the faulty net in the order are re-evaluated; detected faults are dropped.
- **Independent fault chunks, no barrier.** Each pool job runs the serial
  algorithm on its own slice of the fault list with its own good simulation.
  One good simulation of all blocks is 0.2% of the serial c7552 time, so
  repeating it per chunk is cheap. Each job writes only its own slice of the
  result array: no locks during the run, and no dependence on scheduling.
- **Hand-written `BoundedQueue`.** One mutex, two condition variables, `close()`,
  `try_push()` so the server answers `BUSY` instead of blocking.
- **Thread per connection, with a cap.** Simple blocking request loops; the
  CPU-heavy work goes to a bounded job pool, so clients cannot oversubscribe it.
- **Shutdown via `signalfd`.** SIGINT/SIGTERM are blocked in every thread and
  read in the main `poll()` loop: no signal handler, then an ordered shutdown
  that cancels jobs and joins every thread.

## Limitations

- Stem faults only (no fan-out branch faults) and no fault collapsing.
- Combinational ISCAS-85 only: `DFF` is rejected, so no ISCAS-89.
- Zero-delay logic simulation: settled values only, no timing or delays.
- Linux only (`signalfd`), IPv4 only, no authentication or TLS (the server
  listens on 127.0.0.1 by default), no idle timeout for connections.
- Thread per connection; an `epoll` event loop is future work (see DESIGN.md).
- The ISCAS-85 netlists other than c17 are downloaded, not redistributed
  ([NOTICE.md](NOTICE.md)).

## Repository layout

```
include/faultline/   library headers (netlist, simulators, queue, pool, socket, protocol, server)
src/                 library sources (faultline_core)
apps/                faultline (CLI), faultline_server, faultline_bench
client/              Python client, standard library only
tests/               GoogleTest unit tests; tests/python: oracle and server tests
tools/               benchmark fetch, Python oracles, Verilog export, check scripts
benchmarks/iscas85/  c17.bench (others go to benchmarks/downloaded/, git-ignored)
docs/                DESIGN.md, PROTOCOL.md, results/ (raw measurement logs)
```

## Licence

MIT, see [LICENSE](LICENSE). Third-party material: [NOTICE.md](NOTICE.md).
