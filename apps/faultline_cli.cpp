// faultline command-line tool.
//   faultline info <file.bench>                 circuit statistics
//   faultline sim  <file.bench> <vectors.txt>   one output line per vector

#include <cstdlib>
#include <exception>
#include <iostream>
#include <string>
#include <vector>

#include "faultline/bench_parser.hpp"
#include "faultline/scalar_simulator.hpp"
#include "faultline/vectors.hpp"

namespace {

int usage() {
    std::cerr << "usage:\n"
                 "  faultline info <file.bench>\n"
                 "  faultline sim <file.bench> <vectors.txt>\n";
    return EXIT_FAILURE;
}

int run_info(const std::string& bench_path) {
    const faultline::Netlist netlist = faultline::parse_bench_file(bench_path);
    std::cout << "file:    " << bench_path << '\n'
              << "inputs:  " << netlist.primary_inputs().size() << '\n'
              << "outputs: " << netlist.primary_outputs().size() << '\n'
              << "gates:   " << netlist.logic_gate_count() << '\n'
              << "depth:   " << netlist.depth() << '\n';
    return EXIT_SUCCESS;
}

int run_sim(const std::string& bench_path, const std::string& vectors_path) {
    const faultline::Netlist netlist = faultline::parse_bench_file(bench_path);
    const std::vector<faultline::InputVector> vectors =
        faultline::read_vectors_file(vectors_path, netlist.primary_inputs().size());

    faultline::ScalarSimulator simulator(netlist);
    std::string out;  // collect output and write once: much faster than many small writes
    for (const faultline::InputVector& vector : vectors) {
        out += faultline::to_bit_string(simulator.simulate(vector));
        out += '\n';
    }
    std::cout << out;
    return EXIT_SUCCESS;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    try {
        if (args.size() == 2 && args[0] == "info") {
            return run_info(args[1]);
        }
        if (args.size() == 3 && args[0] == "sim") {
            return run_sim(args[1], args[2]);
        }
        return usage();
    } catch (const std::exception& error) {
        // ParseError messages already start with "file:line:".
        std::cerr << "faultline: error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
