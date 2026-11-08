#!/usr/bin/env python3
"""Converts a combinational .bench netlist to structural Verilog (stdlib only).

Usage: bench_to_verilog.py <file.bench> [module_name] > out.v

The module has two vector ports so a testbench can drive it with one
$readmemb line per vector:
    input  [0:N-1] pi;   // pi[i] = i-th INPUT() in file order
    output [0:M-1] po;   // po[j] = j-th OUTPUT() in file order
With [0:N-1] the leftmost character of a $readmemb line lands in pi[0], so
the vector text means the same thing here as in `faultline sim`.

Each gate becomes one Verilog gate primitive (and, nand, or, nor, xor, xnor,
not, buf). Internal wires are named w<k> because .bench names such as "22"
are not legal Verilog identifiers; a comment keeps the original name.
"""

import re
import sys

PRIMITIVES = {
    "AND": "and", "NAND": "nand", "OR": "or", "NOR": "nor",
    "XOR": "xor", "XNOR": "xnor", "NOT": "not", "BUF": "buf", "BUFF": "buf",
}
DECLARATION = re.compile(r"^(INPUT|OUTPUT)\s*\(\s*(\S+?)\s*\)$", re.IGNORECASE)
ASSIGNMENT = re.compile(r"^(\S+?)\s*=\s*(\w+)\s*\((.*)\)$")


def read_bench(path):
    inputs, outputs, gates = [], [], []
    with open(path, encoding="ascii") as handle:
        for number, raw in enumerate(handle, start=1):
            line = raw.split("#", 1)[0].strip()
            if not line:
                continue
            match = DECLARATION.match(line)
            if match:
                (inputs if match.group(1).upper() == "INPUT" else outputs).append(match.group(2))
                continue
            match = ASSIGNMENT.match(line)
            if not match or match.group(2).upper() not in PRIMITIVES:
                raise ValueError(f"{path}:{number}: unsupported line '{line}'")
            args = [arg.strip() for arg in match.group(3).split(",")]
            gates.append((match.group(1), PRIMITIVES[match.group(2).upper()], args))
    return inputs, outputs, gates


def to_verilog(path, module_name):
    inputs, outputs, gates = read_bench(path)
    nets = inputs + [net for net, _, _ in gates]
    wire = {net: f"w{index}" for index, net in enumerate(nets)}

    out = [f"// Generated from {path} by tools/bench_to_verilog.py",
           f"module {module_name}(pi, po);",
           f"  input  [0:{len(inputs) - 1}] pi;",
           f"  output [0:{len(outputs) - 1}] po;"]
    out += [f"  wire {wire[net]};  // {net}" for net in nets]
    out += [f"  assign {wire[net]} = pi[{i}];" for i, net in enumerate(inputs)]
    for index, (net, primitive, args) in enumerate(gates):
        operands = ", ".join([wire[net]] + [wire[arg] for arg in args])
        out.append(f"  {primitive} g{index} ({operands});")
    out += [f"  assign po[{j}] = {wire[net]};" for j, net in enumerate(outputs)]
    out.append("endmodule")
    return "\n".join(out) + "\n"


def main(argv):
    if len(argv) not in (2, 3):
        sys.exit("usage: bench_to_verilog.py <file.bench> [module_name]")
    module_name = argv[2] if len(argv) == 3 else "dut"
    sys.stdout.write(to_verilog(argv[1], module_name))


if __name__ == "__main__":
    main(sys.argv)
