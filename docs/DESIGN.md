# faultline design notes

Part 1 covers parsing, the netlist graph, levelization and scalar simulation.

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
