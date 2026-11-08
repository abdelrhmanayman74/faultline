#include <gtest/gtest.h>

#include <sstream>
#include <string>
#include <vector>

#include "faultline/bench_parser.hpp"
#include "faultline/errors.hpp"
#include "faultline/scalar_simulator.hpp"
#include "faultline/vectors.hpp"

namespace faultline {
namespace {

InputVector bits(const std::string& text) {
    InputVector values;
    for (const char c : text) {
        values.push_back(c == '1' ? 1 : 0);
    }
    return values;
}

// c17 inputs in order (1, 2, 3, 6, 7), outputs (22, 23). Simplifying the six
// NANDs by hand gives:
//   22 = (1 AND 3) OR (2 AND NOT(3 AND 6))
//   23 = NOT(3 AND 6) AND (2 OR 7)
// Example, input 10111: 10=0, 11=0, 16=1, 19=1, so 22=1, 23=0.
struct TruthRow {
    const char* inputs;
    const char* outputs;
};
constexpr TruthRow kC17TruthTable[] = {
    {"00000", "00"}, {"00001", "01"}, {"00010", "00"}, {"00011", "01"},
    {"00100", "00"}, {"00101", "01"}, {"00110", "00"}, {"00111", "00"},
    {"01000", "11"}, {"01001", "11"}, {"01010", "11"}, {"01011", "11"},
    {"01100", "11"}, {"01101", "11"}, {"01110", "00"}, {"01111", "00"},
    {"10000", "00"}, {"10001", "01"}, {"10010", "00"}, {"10011", "01"},
    {"10100", "10"}, {"10101", "11"}, {"10110", "10"}, {"10111", "10"},
    {"11000", "11"}, {"11001", "11"}, {"11010", "11"}, {"11011", "11"},
    {"11100", "11"}, {"11101", "11"}, {"11110", "10"}, {"11111", "10"},
};

TEST(ScalarSimulator, C17FullTruthTable) {
    const Netlist netlist = parse_bench_file(FAULTLINE_SOURCE_DIR "/benchmarks/iscas85/c17.bench");
    ScalarSimulator simulator(netlist);
    for (const TruthRow& row : kC17TruthTable) {
        EXPECT_EQ(to_bit_string(simulator.simulate(bits(row.inputs))), row.outputs)
            << "inputs " << row.inputs;
    }
}

// Every gate type over all 3-input combinations, including multi-input XOR
// (parity) and XNOR.
TEST(ScalarSimulator, AllGateTypes) {
    std::istringstream in(
        "INPUT(a)\nINPUT(b)\nINPUT(c)\n"
        "OUTPUT(and)\nOUTPUT(nand)\nOUTPUT(or)\nOUTPUT(nor)\n"
        "OUTPUT(xor)\nOUTPUT(xnor)\nOUTPUT(not)\nOUTPUT(buf)\n"
        "and = AND(a, b, c)\nnand = NAND(a, b, c)\nor = OR(a, b, c)\nnor = NOR(a, b, c)\n"
        "xor = XOR(a, b, c)\nxnor = XNOR(a, b, c)\nnot = NOT(a)\nbuf = BUF(a)\n");
    const Netlist netlist = parse_bench(in, "gates.bench");
    ScalarSimulator simulator(netlist);
    for (int v = 0; v < 8; ++v) {
        const int a = (v >> 2) & 1;
        const int b = (v >> 1) & 1;
        const int c = v & 1;
        const int all = a & b & c;
        const int any = a | b | c;
        const int parity = a ^ b ^ c;
        const std::string expected = {
            char('0' + all), char('0' + !all), char('0' + any),    char('0' + !any),
            char('0' + parity), char('0' + !parity), char('0' + !a), char('0' + a)};
        const InputVector input = {Logic(a), Logic(b), Logic(c)};
        EXPECT_EQ(to_bit_string(simulator.simulate(input)), expected) << "v=" << v;
    }
}

TEST(ScalarSimulator, WrongInputCountThrows) {
    const Netlist netlist = parse_bench_file(FAULTLINE_SOURCE_DIR "/benchmarks/iscas85/c17.bench");
    ScalarSimulator simulator(netlist);
    EXPECT_THROW(simulator.simulate(bits("101")), std::invalid_argument);
}

TEST(Vectors, ReadsCommentsAndRejectsBadLines) {
    std::istringstream good("# header\n\n10 1  # spaces are ignored\n000\n");
    const std::vector<InputVector> vectors = read_vectors(good, "v.txt", 3);
    ASSERT_EQ(vectors.size(), 2U);
    EXPECT_EQ(to_bit_string(vectors[0]), "101");

    std::istringstream too_short("101\n10\n");
    try {
        read_vectors(too_short, "v.txt", 3);
        FAIL() << "expected ParseError";
    } catch (const ParseError& error) {
        EXPECT_EQ(error.line(), 2);
    }

    std::istringstream bad_char("1x1\n");
    EXPECT_THROW(read_vectors(bad_char, "v.txt", 3), ParseError);
}

}  // namespace
}  // namespace faultline
