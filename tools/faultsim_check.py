#!/usr/bin/env python3
"""Compare `faultline faultsim` with the naive oracle tools/ref_faultsim.py.

Usage: faultsim_check.py [--cli PATH] [--vectors N] [--seed S] file.bench...

For each circuit: make N seeded random vectors (all 2^n vectors if
2^n <= N, same generator as iverilog_check.py), run the C++ CLI with
--detections --undetected and the Python oracle on the same vector file, and
require the same fault list, the same detected set and the same first
detecting vector for every detected fault. Exit status is 1 on any mismatch.
"""

import argparse
import subprocess
import sys
import tempfile
import time
from pathlib import Path

TOOLS = Path(__file__).resolve().parent
sys.path.insert(0, str(TOOLS))
import ref_eval  # noqa: E402
import ref_faultsim  # noqa: E402
from iverilog_check import make_vectors  # noqa: E402


def parse_cli_output(text):
    """{(net, value): first vector or None} from the D/U lines of the CLI."""
    results = {}
    for line in text.splitlines():
        fields = line.split()
        if fields and fields[0] in ("D", "U"):
            key = (fields[1], int(fields[2][2:]))  # "SA0" -> 0
            if key in results:
                raise ValueError(f"fault listed twice: {line}")
            results[key] = int(fields[3]) if fields[0] == "D" else None
    return results


def check_circuit(bench, cli, count, seed, threads):
    inputs, _, _ = ref_eval.parse_bench(str(bench))
    vectors = make_vectors(len(inputs), count, seed, bench.stem)
    with tempfile.TemporaryDirectory() as tmp:
        vector_file = Path(tmp) / "vectors.txt"
        vector_file.write_text("\n".join(vectors) + "\n")
        cli_run = subprocess.run(
            [cli, "faultsim", str(bench), "--vectors", str(vector_file), "--detections",
             "--undetected", "--threads", str(threads)], capture_output=True, text=True, check=False)
    if cli_run.returncode != 0:
        print(f"{bench.stem}: FAIL, faultline exited {cli_run.returncode}: {cli_run.stderr}")
        return False
    cli_results = parse_cli_output(cli_run.stdout)

    start = time.perf_counter()
    ref_results = ref_faultsim.fault_simulate(str(bench), [[int(b) for b in v] for v in vectors])
    oracle_seconds = time.perf_counter() - start

    if set(cli_results) != set(ref_results):
        missing = sorted(set(ref_results) - set(cli_results))[:5]
        extra = sorted(set(cli_results) - set(ref_results))[:5]
        print(f"{bench.stem}: FAIL, fault lists differ; missing {missing}, extra {extra}")
        return False
    mismatches = [(key, cli_results[key], ref_results[key])
                  for key in ref_results if cli_results[key] != ref_results[key]]
    if mismatches:
        print(f"{bench.stem}: MISMATCH in {len(mismatches)} faults, first ones "
              f"(fault, faultline, oracle): {mismatches[:5]}")
        return False
    detected = sum(1 for first in ref_results.values() if first is not None)
    print(f"{bench.stem}: {len(vectors)} vectors, {len(ref_results)} faults, {detected} detected "
          f"({100.0 * detected / len(ref_results):.2f}%), detected sets and first-detecting "
          f"vectors match (oracle {oracle_seconds:.1f} s)")
    return True


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("benches", nargs="+", type=Path)
    parser.add_argument("--cli", default=str(TOOLS.parent / "build" / "debug" / "faultline"))
    parser.add_argument("--vectors", type=int, default=256)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--threads", type=int, default=1, help="passed to faultline faultsim")
    args = parser.parse_args()

    print(f"# seed={args.seed}; vectors per circuit={args.vectors}; threads={args.threads}")
    all_ok = True
    for bench in args.benches:
        all_ok = check_circuit(bench, args.cli, args.vectors, args.seed, args.threads) and all_ok
    print("ALL MATCH" if all_ok else "SOME CIRCUITS FAILED")
    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
