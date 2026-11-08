// faultline command-line tool.
//   faultline info <file.bench>                                 circuit statistics
//   faultline sim  <file.bench> <vectors.txt> [--bitparallel]   one output line per vector
//   faultline faultsim <file.bench> (--vectors <file> | --random N [--seed S])
//                      [--detections] [--undetected]            stuck-at fault simulation

#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "faultline/bench_parser.hpp"
#include "faultline/bit_parallel_simulator.hpp"
#include "faultline/fault_simulator.hpp"
#include "faultline/scalar_simulator.hpp"
#include "faultline/vectors.hpp"

namespace {

// Bad command line: print the message and the usage text.
class UsageError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

int usage() {
    std::cerr << "usage:\n"
                 "  faultline info <file.bench>\n"
                 "  faultline sim <file.bench> <vectors.txt> [--bitparallel]\n"
                 "  faultline faultsim <file.bench> (--vectors <file> | --random N [--seed S])\n"
                 "                     [--detections] [--undetected]\n"
                 "faultsim prints a summary; --detections adds 'D <net> SA<v> <first vector>'\n"
                 "lines, --undetected adds 'U <net> SA<v>' lines. Default seed: 1.\n";
    return EXIT_FAILURE;
}

std::uint64_t parse_number(const std::string& text, const std::string& option) {
    std::uint64_t value = 0;
    const char* end = text.data() + text.size();
    const auto [ptr, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc() || ptr != end) {
        throw UsageError(option + " needs a non-negative integer, got '" + text + "'");
    }
    return value;
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

// Both modes print the same text; the scalar one is the reference.
int run_sim(const std::string& bench_path, const std::string& vectors_path, bool bit_parallel) {
    const faultline::Netlist netlist = faultline::parse_bench_file(bench_path);
    const std::size_t num_inputs = netlist.primary_inputs().size();
    const std::vector<faultline::InputVector> vectors =
        faultline::read_vectors_file(vectors_path, num_inputs);

    std::string out;  // collect output and write once: much faster than many small writes
    if (bit_parallel) {
        faultline::BitParallelSimulator simulator(netlist);
        for (const faultline::PatternBlock& block : faultline::pack_vectors(vectors, num_inputs)) {
            simulator.simulate(block);
            for (std::size_t lane = 0; lane < block.count; ++lane) {
                out += faultline::to_bit_string(simulator.outputs(lane)) + '\n';
            }
        }
    } else {
        faultline::ScalarSimulator simulator(netlist);
        for (const faultline::InputVector& vector : vectors) {
            out += faultline::to_bit_string(simulator.simulate(vector)) + '\n';
        }
    }
    std::cout << out;
    return EXIT_SUCCESS;
}

struct FaultsimOptions {
    std::string bench_path;
    std::string vectors_path;  // empty when --random is used
    std::uint64_t random_count = 0;
    std::uint64_t seed = 1;
    bool use_random = false;
    bool list_detections = false;
    bool list_undetected = false;
};

// args[0] is "faultsim".
FaultsimOptions parse_faultsim_args(const std::vector<std::string>& args) {
    FaultsimOptions options;
    for (std::size_t i = 1; i < args.size(); ++i) {
        const std::string& arg = args[i];
        const bool has_value = i + 1 < args.size();
        if (arg == "--vectors" && has_value) {
            options.vectors_path = args[++i];
        } else if (arg == "--random" && has_value) {
            options.random_count = parse_number(args[++i], arg);
            options.use_random = true;
        } else if (arg == "--seed" && has_value) {
            options.seed = parse_number(args[++i], arg);
        } else if (arg == "--detections") {
            options.list_detections = true;
        } else if (arg == "--undetected") {
            options.list_undetected = true;
        } else if (!arg.starts_with("--") && options.bench_path.empty()) {
            options.bench_path = arg;
        } else {
            throw UsageError("unexpected argument '" + arg + "'");
        }
    }
    if (options.bench_path.empty() || options.use_random == !options.vectors_path.empty()) {
        throw UsageError("faultsim needs a .bench file and exactly one of --vectors, --random");
    }
    return options;
}

std::string format_fixed(double value, const char* format) {
    char text[64];
    std::snprintf(text, sizeof text, format, value);
    return text;
}

int run_faultsim(const FaultsimOptions& options) {
    const faultline::Netlist netlist = faultline::parse_bench_file(options.bench_path);
    const std::size_t num_inputs = netlist.primary_inputs().size();
    const std::vector<faultline::InputVector> vectors =
        options.use_random ? faultline::random_vectors(options.random_count, num_inputs, options.seed)
                           : faultline::read_vectors_file(options.vectors_path, num_inputs);

    // Timed part: fault simulation only, not parsing or vector generation.
    const auto start = std::chrono::steady_clock::now();
    const faultline::FaultSimReport report = faultline::run_fault_simulation(netlist, vectors);
    const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;

    const std::size_t total = report.faults.size();
    const std::size_t detected = report.detected_count();
    std::string out = "circuit:    " + options.bench_path + '\n' +
                      "vectors:    " + std::to_string(report.num_vectors) + '\n' +
                      "faults:     " + std::to_string(total) +
                      " (stuck-at-0/1 on every net stem; no branch faults, no collapsing)\n" +
                      "detected:   " + std::to_string(detected) + '\n' +
                      "undetected: " + std::to_string(total - detected) + '\n' +
                      "coverage:   " + format_fixed(report.coverage_percent(), "%.2f%%") + '\n' +
                      "time_ms:    " + format_fixed(elapsed.count(), "%.3f") +
                      " (fault simulation only, single thread)\n";
    for (std::size_t i = 0; i < total; ++i) {
        const faultline::FaultStatus& status = report.status[i];
        const std::string name = faultline::fault_name(netlist, report.faults[i]);
        if (status.detected && options.list_detections) {
            out += "D " + name + ' ' + std::to_string(status.first_vector) + '\n';
        } else if (!status.detected && options.list_undetected) {
            out += "U " + name + '\n';
        }
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
            return run_sim(args[1], args[2], false);
        }
        if (args.size() == 4 && args[0] == "sim" && args[3] == "--bitparallel") {
            return run_sim(args[1], args[2], true);
        }
        if (!args.empty() && args[0] == "faultsim") {
            return run_faultsim(parse_faultsim_args(args));
        }
        return usage();
    } catch (const UsageError& error) {
        std::cerr << "faultline: " << error.what() << '\n';
        return usage();
    } catch (const std::exception& error) {
        // ParseError messages already start with "file:line:".
        std::cerr << "faultline: error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
