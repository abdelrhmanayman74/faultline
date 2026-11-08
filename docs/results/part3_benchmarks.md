# Part 3 benchmarks: scalar vs bit-parallel vs multithreaded fault simulation

- **Date:** 2026-11-08, about 00:50 UTC.
- **Machine:** Intel Core i7-8750H @ 2.20 GHz (max 4.1 GHz), 1 socket, 6 cores,
  2 threads per core = 12 logical CPUs; L1d 32 KiB and L2 256 KiB per core, L3 9 MiB
  shared; 16 GB RAM; Linux 6.8.0-138-generic; cpufreq governor `performance`; laptop,
  on an otherwise idle desktop session (from `lscpu`, `top`).
- **Compiler / flags:** g++ 11.4.0 (Ubuntu 11.4.0-1ubuntu1~22.04.3), `-std=c++20 -O3 -DNDEBUG`
  (CMake `release` preset).
- **Build:** `cmake --preset release && cmake --build --preset release`
- **Command (main table):**

  ```
  build/release/faultline_bench --runs 5 benchmarks/iscas85/c17.bench \
      benchmarks/downloaded/{c432,c499,c880,c1355,c1908,c2670,c3540,c5315,c6288,c7552}.bench
  ```

  Defaults: 1024 random vectors from `random_vectors(1024, inputs, seed 1)` (the same
  vectors as `docs/results/part2_coverage.txt`), median of 5 runs, scalar baseline
  only for circuits with at most 1000 gates.

## What each column measures

All times cover fault simulation only (parsing and vector generation excluded),
same stem fault list (stuck-at-0/1 on every net, no collapsing), fault dropping on.

- **scalar:** one fault and one vector at a time, whole faulty circuit re-simulated
  with the scalar evaluator, fault dropping (`scalar_fault_simulation()` in
  `apps/faultline_bench.cpp`). Skipped above 1000 gates: c1908 already needs 0.9 s
  per run.
- **good sim only:** packing the vectors and simulating the good circuit for every
  block, with no faults. This is the work each parallel job repeats.
- **bit-par serial:** `run_fault_simulation()`, single thread (Part 2 code).
- **NT:** `run_fault_simulation_parallel(..., N)`: N worker threads, 4N chunks. Includes
  creating and joining the pool.

Every timed run is compared with the serial result (full status array: detected flag
and first detecting vector of every fault); the tool exits non-zero on any
difference. This run: all identical.

## Results (1024 vectors)

Median wall time in ms:

| circuit | gates | faults | scalar | good sim only | bit-par serial | 1T | 2T | 4T | 6T | 12T |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| c17 | 6 | 22 | 0.10 | 0.007 | 0.01 | 0.16 | 0.09 | 0.18 | 0.19 | 0.39 |
| c432 | 160 | 392 | 8.91 | 0.028 | 0.28 | 0.43 | 0.31 | 0.23 | 0.33 | 0.46 |
| c499 | 202 | 486 | 12.41 | 0.033 | 0.34 | 0.47 | 0.30 | 0.30 | 0.33 | 0.49 |
| c880 | 383 | 886 | 56.99 | 0.062 | 1.79 | 2.32 | 1.41 | 0.80 | 0.75 | 0.95 |
| c1355 | 546 | 1174 | 118.63 | 0.058 | 1.86 | 2.18 | 1.35 | 0.93 | 0.83 | 0.90 |
| c1908 | 880 | 1826 | 892.84 | 0.118 | 13.94 | 15.19 | 8.45 | 4.86 | 4.09 | 3.52 |
| c2670 | 1193 | 2852 | skipped | 0.243 | 57.72 | 58.61 | 32.22 | 17.12 | 12.66 | 11.03 |
| c3540 | 1669 | 3438 | skipped | 0.237 | 46.59 | 48.67 | 26.78 | 13.97 | 10.96 | 8.14 |
| c5315 | 2307 | 4970 | skipped | 0.387 | 61.17 | 64.12 | 34.72 | 18.82 | 14.13 | 10.31 |
| c6288 | 2416 | 4896 | skipped | 0.185 | 31.70 | 31.77 | 16.84 | 8.87 | 6.97 | 6.07 |
| c7552 | 3512 | 7438 | skipped | 0.515 | 239.84 | 241.82 | 127.92 | 66.89 | 50.45 | 38.87 |

Speed-ups. First column: scalar time / bit-parallel serial time. Thread
columns: bit-parallel serial time / time with N threads.

| circuit | bit-par vs scalar | 1T | 2T | 4T | 6T | 12T |
|---|---:|---:|---:|---:|---:|---:|
| c17 | 14.0 | 0.05 | 0.08 | 0.04 | 0.04 | 0.02 |
| c432 | 32.3 | 0.65 | 0.89 | 1.21 | 0.84 | 0.60 |
| c499 | 36.7 | 0.72 | 1.11 | 1.13 | 1.03 | 0.70 |
| c880 | 31.8 | 0.77 | 1.27 | 2.24 | 2.40 | 1.89 |
| c1355 | 63.8 | 0.85 | 1.37 | 2.01 | 2.23 | 2.06 |
| c1908 | 64.0 | 0.92 | 1.65 | 2.87 | 3.41 | 3.96 |
| c2670 | - | 0.98 | 1.79 | 3.37 | 4.56 | 5.23 |
| c3540 | - | 0.96 | 1.74 | 3.33 | 4.25 | 5.72 |
| c5315 | - | 0.95 | 1.76 | 3.25 | 4.33 | 5.94 |
| c6288 | - | 1.00 | 1.88 | 3.57 | 4.55 | 5.22 |
| c7552 | - | 0.99 | 1.87 | 3.59 | 4.75 | 6.17 |

## Longer runs (16384 vectors)

`build/release/faultline_bench --runs 3 --vectors 16384 --scalar-max-gates 1 benchmarks/downloaded/{c2670,c5315,c7552}.bench`

| circuit | good sim only | bit-par serial | 1T | 2T | 4T | 6T | 12T |
|---|---:|---:|---:|---:|---:|---:|---:|
| c2670 | 4.052 | 516.66 | 522.48 | 275.18 | 159.35 | 116.39 | 116.29 |
| c5315 | 6.064 | 74.97 | 83.24 | 44.85 | 24.97 | 22.67 | 19.01 |
| c7552 | 8.026 | 1549.61 | 1566.22 | 830.38 | 451.96 | 400.93 | 325.02 |

| circuit | 1T | 2T | 4T | 6T | 12T |
|---|---:|---:|---:|---:|---:|
| c2670 | 0.99 | 1.88 | 3.24 | 4.44 | 4.44 |
| c5315 | 0.90 | 1.67 | 3.00 | 3.31 | 3.94 |
| c7552 | 0.99 | 1.87 | 3.43 | 3.86 | 4.77 |

## Chunks per thread

Same command as the main table restricted to c2670, c5315 and c6288 (`--runs 5
--scalar-max-gates 1`), rebuilt with `kChunksPerThread` set to 1, 4 and 16.
Median ms:

| circuit | k | 1T | 2T | 4T | 6T | 12T |
|---|---:|---:|---:|---:|---:|---:|
| c2670 | 1 | 59.47 | 42.48 | 28.43 | 20.11 | 13.73 |
| c2670 | 4 | 59.23 | 31.47 | 17.08 | 12.82 | 10.59 |
| c2670 | 16 | 62.23 | 32.74 | 17.79 | 13.95 | 11.17 |
| c5315 | 1 | 63.77 | 47.94 | 29.02 | 19.50 | 13.37 |
| c5315 | 4 | 66.32 | 35.28 | 19.24 | 14.33 | 10.27 |
| c5315 | 16 | 67.09 | 35.70 | 20.77 | 14.92 | 11.68 |
| c6288 | 1 | 30.71 | 23.79 | 14.44 | 10.06 | 9.74 |
| c6288 | 4 | 31.82 | 16.72 | 8.91 | 7.33 | 6.18 |
| c6288 | 16 | 32.95 | 18.11 | 9.35 | 9.11 | 8.21 |

One chunk per thread balances badly (2 threads give only 1.3-1.4x: one half of
the fault list costs much more than the other). 16 per thread is slightly slower
than 4 everywhere: more repeated good simulation and more jobs, with no balance
left to gain. 4 is the value used.

## Analysis

- **Bit-parallel vs scalar: 14x to 64x** on the circuits where the scalar baseline
  ran. One 64-bit word evaluates 64 vectors per gate, and the bit-parallel simulator
  also re-evaluates only the part of `eval_order()` after the faulty net, while the
  scalar baseline re-simulates the whole circuit. The gain is below 64x on small
  circuits because fault dropping leaves partly used blocks and per-block overhead
  matters there.
- **Threads help only when there is enough work.** c17 to c499 take well under
  1 ms serially. The parallel c17 runs take 0.09-0.39 ms for 0.01 ms of work,
  which is roughly what starting and joining the threads costs, so on these
  circuits the parallel version is slower (speed-up below 1). A caller should keep 1 thread for
  tiny jobs; the CLI default is 1.
- **Large circuits (c2670 and up), 1024 vectors: 1.7-1.9x on 2 threads, 3.2-3.6x
  on 4, 4.2-4.8x on 6 and 5.2-6.2x on 12.** Scaling is close to linear up to 2 threads and
  bends after that.
- **Why it flattens beyond 6 threads.** The machine has 6 physical cores. Threads
  7-12 are hyperthreads that share a core's execution units with another thread.
  This code is integer ALU work on data that fits in L1/L2 (c7552: 3512 nets x
  8 bytes = 28 KiB per value array, two arrays per job), so there are few memory
  stalls for a second hyperthread to fill. The 6T to 12T step still gives 1.15-1.37x
  with 1024 vectors, and none at all for c2670 with 16384 vectors. Memory
  bandwidth is unlikely to be the limit, because the working set stays in cache.
  Not measured but likely: one busy core of this laptop CPU turbos higher than six
  busy cores, so the serial baseline runs at a higher clock than the parallel runs.
- **Longer runs scale worse (16384 vectors: 3.9-4.8x on 12 threads).** With many
  vectors, most of the time goes to the few hundred faults that are never detected
  and are simulated in all 256 blocks, so the cost per chunk gets more uneven.
  A rough check (`faultsim --detections --undetected`, counting fault-block
  pairs per chunk without weighting by suffix length) finds the largest of 48
  chunks holds 8-11% of the work, which alone would still allow about 9-12x. So
  one giant chunk is not the whole story; the rest is not broken down. The
  likely causes are the per-fault cost differences that this count ignores, plus
  hyperthreads and clock speed as above. `perf` is not usable here
  (`kernel.perf_event_paranoid = 4` for normal users), so there is no counter
  data.
- **Cost of the design choice.** The 1T column runs the parallel code with one
  worker and 4 chunks. It is 0-5% slower than the serial code on large circuits
  (up to 11% for c5315 with 16384 vectors). Repeating the good simulation explains
  part of this: "good sim only" is 0.2% of serial time for c7552 at 1024 vectors,
  so 4 copies cost about 0.9%. The rest (pool start-up, per-job simulator
  allocation) was not broken down. With 48 chunks at 12 threads the repeated good
  simulation is about 48 x 0.5 ms = 25 ms of CPU time for c7552, spread over
  12 threads: about 2 ms of a 39 ms run.
