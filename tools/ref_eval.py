#!/usr/bin/env python3
"""Independent reference evaluator for .bench netlists (Python stdlib only).

Usage: ref_eval.py <file.bench> <vectors.txt>

Prints one line per vector with the primary outputs as '0'/'1' characters,
the same format as `faultline sim`.

This is a test oracle, so it deliberately shares no design with the C++ code:
it parses with regular expressions, keeps the netlist as a dict of
name -> (function, inputs), and computes each output by memoized recursion
from the output back towards the inputs (demand-driven). The C++ simulator
instead levelizes the graph and sweeps forward over every gate.
"""

import re
import sys
from functools import reduce

DECLARATION = re.compile(r"^(INPUT|OUTPUT)\s*\(\s*([^()\s,]+)\s*\)$", re.IGNORECASE)
ASSIGNMENT = re.compile(r"^([^=\s]+)\s*=\s*(\w+)\s*\((.*)\)$")


def _and(bits):
    return int(all(bits))


def _or(bits):
    return int(any(bits))


def _xor(bits):
    return reduce(lambda x, y: x ^ y, bits, 0)


FUNCTIONS = {
    "AND": _and,
    "NAND": lambda bits: 1 - _and(bits),
    "OR": _or,
    "NOR": lambda bits: 1 - _or(bits),
    "XOR": _xor,
    "XNOR": lambda bits: 1 - _xor(bits),
    "NOT": lambda bits: 1 - bits[0],
    "BUF": lambda bits: bits[0],
    "BUFF": lambda bits: bits[0],
}


def parse_bench(path):
    """Returns (inputs, outputs, gates) where gates maps net -> (FUNC, [inputs])."""
    inputs, outputs, gates = [], [], {}
    with open(path, encoding="ascii") as handle:
        for number, raw in enumerate(handle, start=1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            match = DECLARATION.match(line)
            if match:
                target = inputs if match.group(1).upper() == "INPUT" else outputs
                target.append(match.group(2))
                continue
            match = ASSIGNMENT.match(line)
            if not match or match.group(2).upper() not in FUNCTIONS:
                sys.exit(f"{path}:{number}: cannot parse '{line}'")
            args = [arg.strip() for arg in match.group(3).split(",")]
            gates[match.group(1)] = (match.group(2).upper(), args)
    return inputs, outputs, gates


def evaluate(inputs, outputs, gates, vector):
    values = dict(zip(inputs, vector))

    def value_of(net):
        if net not in values:
            func, args = gates[net]
            values[net] = FUNCTIONS[func]([value_of(arg) for arg in args])
        return values[net]

    return "".join(str(value_of(net)) for net in outputs)


def read_vectors(path, width):
    vectors = []
    with open(path, encoding="ascii") as handle:
        for number, raw in enumerate(handle, start=1):
            bits = "".join(raw.split("#", 1)[0].split())
            if not bits:
                continue
            if len(bits) != width or set(bits) - {"0", "1"}:
                sys.exit(f"{path}:{number}: bad vector '{bits}' (need {width} bits of 0/1)")
            vectors.append([int(bit) for bit in bits])
    return vectors


def main(argv):
    if len(argv) != 3:
        sys.exit("usage: ref_eval.py <file.bench> <vectors.txt>")
    inputs, outputs, gates = parse_bench(argv[1])
    # Recursion depth is bounded by circuit depth (c6288, the deepest ISCAS-85
    # circuit, is about 125 levels), but raise the limit for safety.
    sys.setrecursionlimit(max(10000, 4 * len(gates)))
    lines = [evaluate(inputs, outputs, gates, v) for v in read_vectors(argv[2], len(inputs))]
    sys.stdout.write("".join(line + "\n" for line in lines))


if __name__ == "__main__":
    main(sys.argv)
