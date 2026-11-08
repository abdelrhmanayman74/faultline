# Third-party material

## ISCAS-85 benchmark netlists

- `benchmarks/iscas85/c17.bench` was written by hand from the c17 netlist that
  is published widely in testing textbooks and papers (6 NAND gates).
- The other ISCAS-85 circuits (c432, c499, c880, c1355, c1908, c2670, c3540,
  c5315, c6288, c7552) are **not** part of this repository.
  `tools/fetch_benchmarks.sh` downloads them into `benchmarks/downloaded/`
  (ignored by git) and checks every file against the SHA-256 hashes in
  `tools/benchmarks.sha256`.
  - Download: release v1.0.2 of https://github.com/Coloquinte/LogicBenchmarks
    (`logic_benchmarks.zip`, SHA-256
    `91012556d72d5f0cc66087b021530bf069f31b4d574a9fa0108283a5b5e7342a`).
    The repository's git tree holds only the build script; the netlists are
    published inside the release zip, file names `bench/iscas85-<name>.bench`.
  - That collection says it took the ISCAS files from
    https://pld.ttu.ee/~maksim/benchmarks/ .
  - Neither source states a licence, so the files are not redistributed here.

The ISCAS-85 circuits were introduced in: F. Brglez and H. Fujiwara, "A Neutral
Netlist of 10 Combinational Benchmark Circuits and a Target Translator in
Fortran", ISCAS 1985.

## GoogleTest

Downloaded at configure time by CMake FetchContent (v1.15.2, pinned by SHA-256),
not stored in this repository. BSD 3-Clause licence.
