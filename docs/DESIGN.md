# faultline design notes

Part 1 covers parsing, the netlist graph, levelization and scalar simulation.
Part 2 adds bit-parallel simulation and single-threaded stuck-at fault simulation.

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

## CLI (Part 2)

```
faultline sim <file.bench> <vectors.txt> [--bitparallel]
faultline faultsim <file.bench> (--vectors <file> | --random N [--seed S]) [--detections] [--undetected]
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

## Building

```
python3 -m venv .venv && .venv/bin/pip install cmake ninja
export PATH="$PWD/.venv/bin:$PATH"
cmake --preset debug && cmake --build --preset debug && ctest --preset debug
tools/fetch_benchmarks.sh   # optional: downloads the larger ISCAS-85 circuits
```

The `tsan` preset runs tests under `setarch <arch> -R` (address randomization
off) because g++ 11's ThreadSanitizer aborts on this kernel otherwise; see the
comment in `CMakeLists.txt`.
