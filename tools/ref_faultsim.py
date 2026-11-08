#!/usr/bin/env python3
"""Naive reference stuck-at fault simulator (Python stdlib only).

Usage: ref_faultsim.py <file.bench> <vectors.txt>

Prints the same fault lines as `faultline faultsim --detections --undetected`:
    D <net> SA<v> <index of first detecting vector>
    U <net> SA<v>
after a two-line summary ("faults: N", "detected: D").

This is a test oracle, so it is deliberately the slow, obvious algorithm and
shares nothing with the C++ fault simulator (no bit-parallel words, no
levelization, no partial re-evaluation): for every stem fault (stuck-at-0/1
on every net) and every vector in order, evaluate the WHOLE faulty circuit
one vector at a time and compare its outputs with the good circuit. The first
vector that differs is the first detecting vector, and the search for that
fault stops there. Netlist and vector parsing come from ref_eval.py.
"""

import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import ref_eval  # noqa: E402  (needs the sys.path line above)


def evaluate(circuit, vector, fault=None):
    """Output string for one vector; `fault` is (net, value) or None.

    Demand-driven memoized recursion as in ref_eval.py. The faulty net is put
    into the memo table before evaluation starts, so every reader sees the
    stuck value and its own gate function is never computed.
    """
    inputs, outputs, gates = circuit
    values = dict(zip(inputs, vector))
    if fault is not None:
        values[fault[0]] = fault[1]

    def value_of(net):
        if net not in values:
            func, args = gates[net]
            values[net] = ref_eval.FUNCTIONS[func]([value_of(arg) for arg in args])
        return values[net]

    return "".join(str(value_of(net)) for net in outputs)


def fault_simulate(bench_path, vectors):
    """Returns {(net, value): first detecting vector index or None}."""
    circuit = ref_eval.parse_bench(bench_path)
    inputs, _, gates = circuit
    sys.setrecursionlimit(max(10000, 4 * len(gates)))
    good = [evaluate(circuit, vector) for vector in vectors]
    results = {}
    for net in inputs + list(gates):
        for value in (0, 1):
            results[(net, value)] = next(
                (index for index, vector in enumerate(vectors)
                 if evaluate(circuit, vector, (net, value)) != good[index]),
                None)
    return results


def format_results(results):
    detected = sum(1 for first in results.values() if first is not None)
    lines = [f"faults: {len(results)}", f"detected: {detected}"]
    for (net, value), first in results.items():
        lines.append(f"D {net} SA{value} {first}" if first is not None else f"U {net} SA{value}")
    return "\n".join(lines) + "\n"


def main(argv):
    if len(argv) != 3:
        sys.exit("usage: ref_faultsim.py <file.bench> <vectors.txt>")
    inputs, _, _ = ref_eval.parse_bench(argv[1])
    vectors = ref_eval.read_vectors(argv[2], len(inputs))
    sys.stdout.write(format_results(fault_simulate(argv[1], vectors)))


if __name__ == "__main__":
    main(sys.argv)
