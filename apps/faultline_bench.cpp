// faultline_bench: times the fault simulators and prints Markdown tables.
//   faultline_bench [--runs N] [--vectors N] [--seed S] [--scalar-max-gates G] <file.bench>...
// Every timed run is also checked against the single-thread bit-parallel
// result; any difference is reported and makes the exit status non-zero.

#include <algorithm>
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
#include "faultline/parallel_fault_simulator.hpp"
#include "faultline/scalar_simulator.hpp"
#include "faultline/vectors.hpp"

namespace {

using faultline::FaultSimReport;
using faultline::InputVector;
using faultline::Netlist;

const std::vector<std::size_t> kThreadCounts = {1, 2, 4, 6, 12};

struct Options {
    std::size_t runs = 5;
    std::size_t num_vectors = 1024;
    std::uint64_t seed = 1;
    std::size_t scalar_max_gates = 1000;  // the scalar baseline is far too slow beyond this
    std::vector<std::string> files;
};

// Baseline without any bit-parallelism: one fault and one vector at a time,
// the whole faulty circuit re-simulated each time, with fault dropping. Only
// the status array is filled.
FaultSimReport scalar_fault_simulation(const Netlist& netlist, const std::vector<InputVector>& vectors) {
    FaultSimReport report;
    report.faults = faultline::stem_fault_list(netlist);
    report.status.assign(report.faults.size(), faultline::FaultStatus{});
    report.num_vectors = vectors.size();

    std::vector<std::vector<faultline::Logic>> good_outputs;
    faultline::ScalarSimulator good(netlist);
    for (const InputVector& vector : vectors) {
        good_outputs.push_back(good.simulate(vector));
    }
    std::vector<std::size_t> input_position(netlist.size(), 0);
    for (std::size_t i = 0; i < netlist.primary_inputs().size(); ++i) {
        input_position[netlist.primary_inputs()[i]] = i;
    }

    std::vector<faultline::Logic> values(netlist.size(), 0);
    for (std::size_t f = 0; f < report.faults.size(); ++f) {
        const faultline::Fault& fault = report.faults[f];
        for (std::size_t v = 0; v < vectors.size() && !report.status[f].detected; ++v) {
            for (const faultline::NetId net : netlist.eval_order()) {
                const faultline::Gate& gate = netlist.gate(net);
                if (net == fault.net) {
                    values[net] = fault.stuck_at;
                } else if (gate.type == faultline::GateType::Input) {
                    values[net] = vectors[v][input_position[net]];
                } else {
                    values[net] = faultline::evaluate_gate(gate, values);
                }
            }
            const std::vector<faultline::NetId>& outputs = netlist.primary_outputs();
            for (std::size_t o = 0; o < outputs.size(); ++o) {
                if (values[outputs[o]] != good_outputs[v][o]) {
                    report.status[f] = faultline::FaultStatus{.detected = true, .first_vector = v};
                    break;
                }
            }
        }
    }
    return report;
}

// Median wall time of `runs` calls, in milliseconds.
template <typename Work>
double median_ms(std::size_t runs, Work work) {
    std::vector<double> times;
    for (std::size_t i = 0; i < runs; ++i) {
        const auto start = std::chrono::steady_clock::now();
        work();
        const std::chrono::duration<double, std::milli> elapsed = std::chrono::steady_clock::now() - start;
        times.push_back(elapsed.count());
    }
    std::sort(times.begin(), times.end());
    return times[times.size() / 2];
}

struct Row {
    std::string name;
    std::size_t gates = 0;
    std::size_t faults = 0;
    double scalar_ms = -1;  // negative: not run
    double good_only_ms = 0;
    double serial_ms = 0;
    std::vector<double> parallel_ms;  // one per kThreadCounts entry
};

std::string fixed(double value, const char* format) {
    char text[64];
    std::snprintf(text, sizeof text, format, value);
    return text;
}

bool check(const FaultSimReport& got, const FaultSimReport& expected, const std::string& what) {
    if (got.status == expected.status) {
        return true;
    }
    std::cerr << "MISMATCH: " << what << " differs from the single-thread result\n";
    return false;
}

Row measure(const std::string& path, const Options& options, bool& all_match) {
    const Netlist netlist = faultline::parse_bench_file(path);
    const std::vector<InputVector> vectors =
        faultline::random_vectors(options.num_vectors, netlist.primary_inputs().size(), options.seed);
    const FaultSimReport reference = faultline::run_fault_simulation(netlist, vectors);

    Row row;
    row.name = path.substr(path.find_last_of('/') + 1);
    row.gates = netlist.logic_gate_count();
    row.faults = reference.faults.size();

    FaultSimReport result;
    if (row.gates <= options.scalar_max_gates) {
        row.scalar_ms = median_ms(options.runs, [&] { result = scalar_fault_simulation(netlist, vectors); });
        all_match &= check(result, reference, row.name + " scalar");
    }

    // Good-circuit simulation alone: the part each parallel job repeats.
    faultline::Word sink = 0;
    row.good_only_ms = median_ms(options.runs, [&] {
        faultline::BitParallelSimulator good(netlist);
        for (const faultline::PatternBlock& block :
             faultline::pack_vectors(vectors, netlist.primary_inputs().size())) {
            good.simulate(block);
            sink ^= good.values().back();  // keeps the compiler from dropping the work
        }
    });

    row.serial_ms = median_ms(options.runs, [&] { result = faultline::run_fault_simulation(netlist, vectors); });
    all_match &= check(result, reference, row.name + " serial");
    for (const std::size_t threads : kThreadCounts) {
        row.parallel_ms.push_back(median_ms(options.runs, [&] {
            result = faultline::run_fault_simulation_parallel(netlist, vectors, threads);
        }));
        all_match &= check(result, reference, row.name + " threads=" + std::to_string(threads));
    }
    std::cerr << row.name << " done (checksum " << (sink & 0xff) << ")\n";
    return row;
}

void print_tables(const std::vector<Row>& rows) {
    std::string header = "| circuit | gates | faults | scalar | good sim only | bit-par serial |";
    std::string rule = "|---|---:|---:|---:|---:|---:|";
    for (const std::size_t threads : kThreadCounts) {
        header += " " + std::to_string(threads) + "T |";
        rule += "---:|";
    }
    std::cout << "Median wall time in ms:\n\n" << header << '\n' << rule << '\n';
    for (const Row& row : rows) {
        std::cout << "| " << row.name << " | " << row.gates << " | " << row.faults << " | "
                  << (row.scalar_ms < 0 ? std::string("skipped") : fixed(row.scalar_ms, "%.2f"))
                  << " | " << fixed(row.good_only_ms, "%.3f") << " | " << fixed(row.serial_ms, "%.2f")
                  << " |";
        for (const double ms : row.parallel_ms) {
            std::cout << ' ' << fixed(ms, "%.2f") << " |";
        }
        std::cout << '\n';
    }

    std::cout << "\nSpeed-ups. First column: scalar time / bit-parallel serial time. Thread\n"
                 "columns: bit-parallel serial time / time with N threads.\n\n"
                 "| circuit | bit-par vs scalar |";
    std::string rule2 = "|---|---:|";
    for (const std::size_t threads : kThreadCounts) {
        std::cout << ' ' << threads << "T |";
        rule2 += "---:|";
    }
    std::cout << '\n' << rule2 << '\n';
    for (const Row& row : rows) {
        std::cout << "| " << row.name << " | "
                  << (row.scalar_ms < 0 ? std::string("-") : fixed(row.scalar_ms / row.serial_ms, "%.1f"))
                  << " |";
        for (const double ms : row.parallel_ms) {
            std::cout << ' ' << fixed(row.serial_ms / ms, "%.2f") << " |";
        }
        std::cout << '\n';
    }
}

std::size_t parse_count(const std::string& text) {
    std::size_t value = 0;
    const char* end = text.data() + text.size();
    const auto [ptr, error] = std::from_chars(text.data(), end, value);
    if (error != std::errc() || ptr != end || value == 0) {
        throw std::invalid_argument("expected a positive integer, got '" + text + "'");
    }
    return value;
}

}  // namespace

int main(int argc, char** argv) {
    const std::vector<std::string> args(argv + 1, argv + argc);
    try {
        Options options;
        for (std::size_t i = 0; i < args.size(); ++i) {
            const bool has_value = i + 1 < args.size();
            if (args[i] == "--runs" && has_value) {
                options.runs = parse_count(args[++i]);
            } else if (args[i] == "--vectors" && has_value) {
                options.num_vectors = parse_count(args[++i]);
            } else if (args[i] == "--seed" && has_value) {
                options.seed = parse_count(args[++i]);
            } else if (args[i] == "--scalar-max-gates" && has_value) {
                options.scalar_max_gates = parse_count(args[++i]);
            } else if (args[i].starts_with("--")) {
                throw std::invalid_argument("unknown option " + args[i]);
            } else {
                options.files.push_back(args[i]);
            }
        }
        if (options.files.empty()) {
            std::cerr << "usage: faultline_bench [--runs N] [--vectors N] [--seed S] "
                         "[--scalar-max-gates G] <file.bench>...\n";
            return EXIT_FAILURE;
        }
        bool all_match = true;
        std::vector<Row> rows;
        for (const std::string& file : options.files) {
            rows.push_back(measure(file, options, all_match));
        }
        print_tables(rows);
        return all_match ? EXIT_SUCCESS : EXIT_FAILURE;
    } catch (const std::exception& error) {
        std::cerr << "faultline_bench: error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
