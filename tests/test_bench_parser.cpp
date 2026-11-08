#include <gtest/gtest.h>

#include <sstream>
#include <string>

#include "faultline/bench_parser.hpp"
#include "faultline/errors.hpp"

namespace faultline {
namespace {

Netlist parse(const std::string& text) {
    std::istringstream in(text);
    return parse_bench(in, "test.bench");
}

// Parses text that must fail; returns the error so tests can check it.
ParseError parse_error(const std::string& text) {
    try {
        parse(text);
    } catch (const ParseError& error) {
        return error;
    }
    ADD_FAILURE() << "expected a ParseError for:\n" << text;
    return ParseError("none", 0, "none");
}

bool contains(const std::string& text, const std::string& part) {
    return text.find(part) != std::string::npos;
}

TEST(BenchParser, ParsesC17FromFile) {
    const Netlist netlist = parse_bench_file(FAULTLINE_SOURCE_DIR "/benchmarks/iscas85/c17.bench");
    EXPECT_EQ(netlist.primary_inputs().size(), 5U);
    EXPECT_EQ(netlist.primary_outputs().size(), 2U);
    EXPECT_EQ(netlist.logic_gate_count(), 6U);
    EXPECT_EQ(netlist.depth(), 3);
}

TEST(BenchParser, AcceptsCommentsWhitespaceCrlfAndForwardReferences) {
    const Netlist netlist = parse(
        "# header comment\r\n"
        "\r\n"
        "  INPUT( a )   # trailing comment\r\n"
        "INPUT(b)\r\n"
        "INPUT(c)\r\n"
        "OUTPUT(y)\r\n"
        "y=nand(t,c)\r\n"  // uses t before its definition; lower-case keyword
        "t   =   AND( a ,b,  c )\r\n"
        "u = BUFF(t)\r\n");
    ASSERT_EQ(netlist.size(), 6U);
    const NetId t = *netlist.find("t");
    EXPECT_EQ(netlist.gate(t).type, GateType::And);
    EXPECT_EQ(netlist.gate(t).fanin.size(), 3U);
    EXPECT_EQ(netlist.gate(*netlist.find("u")).type, GateType::Buf);
    EXPECT_EQ(netlist.gate(*netlist.find("y")).fanin.front(), t);
    EXPECT_EQ(netlist.primary_outputs().front(), *netlist.find("y"));
}

TEST(BenchParser, OutputMayBeAPrimaryInput) {
    const Netlist netlist = parse("INPUT(a)\nOUTPUT(a)\n");
    EXPECT_EQ(netlist.primary_outputs().front(), netlist.primary_inputs().front());
}

TEST(BenchParser, FanoutIsFilled) {
    const Netlist netlist = parse("INPUT(a)\nOUTPUT(x)\nOUTPUT(y)\nx = NOT(a)\ny = BUF(a)\n");
    EXPECT_EQ(netlist.gate(*netlist.find("a")).fanout.size(), 2U);
}

TEST(BenchParserErrors, UndefinedNet) {
    const ParseError error = parse_error("INPUT(a)\nOUTPUT(y)\ny = AND(a, ghost)\n");
    EXPECT_EQ(error.line(), 3);
    EXPECT_TRUE(contains(error.what(), "test.bench:3:"));
    EXPECT_TRUE(contains(error.what(), "undefined net 'ghost'"));
}

TEST(BenchParserErrors, DuplicateDefinition) {
    const ParseError error = parse_error("INPUT(a)\nOUTPUT(y)\ny = NOT(a)\ny = BUF(a)\n");
    EXPECT_EQ(error.line(), 4);
    EXPECT_TRUE(contains(error.what(), "defined twice (first definition at line 3)"));
}

TEST(BenchParserErrors, GateRedefinesInput) {
    const ParseError error = parse_error("INPUT(a)\nOUTPUT(a)\na = NOT(a)\n");
    EXPECT_EQ(error.line(), 3);
    EXPECT_TRUE(contains(error.what(), "defined twice"));
}

TEST(BenchParserErrors, UnknownGate) {
    const ParseError error = parse_error("INPUT(a)\nOUTPUT(y)\ny = MUX(a, a)\n");
    EXPECT_EQ(error.line(), 3);
    EXPECT_TRUE(contains(error.what(), "unknown gate type 'MUX'"));
}

TEST(BenchParserErrors, DffIsRejectedClearly) {
    const ParseError error = parse_error("INPUT(a)\nOUTPUT(q)\nq = DFF(a)\n");
    EXPECT_EQ(error.line(), 3);
    EXPECT_TRUE(contains(error.what(), "sequential circuits not supported yet"));
}

TEST(BenchParserErrors, NotWithTwoInputs) {
    const ParseError error = parse_error("INPUT(a)\nINPUT(b)\nOUTPUT(y)\ny = NOT(a, b)\n");
    EXPECT_EQ(error.line(), 4);
    EXPECT_TRUE(contains(error.what(), "needs exactly 1 input, got 2"));
}

TEST(BenchParserErrors, BufWithTwoInputs) {
    const ParseError error = parse_error("INPUT(a)\nOUTPUT(y)\ny = BUFF(a, a)\n");
    EXPECT_TRUE(contains(error.what(), "needs exactly 1 input"));
}

TEST(BenchParserErrors, OutputNeverDriven) {
    const ParseError error = parse_error("INPUT(a)\nOUTPUT(a)\nOUTPUT(nowhere)\n");
    EXPECT_EQ(error.line(), 3);
    EXPECT_TRUE(contains(error.what(), "output 'nowhere' is never driven"));
}

TEST(BenchParserErrors, OutputDeclaredTwice) {
    const ParseError error = parse_error("INPUT(a)\nOUTPUT(a)\nOUTPUT(a)\n");
    EXPECT_EQ(error.line(), 3);
}

TEST(BenchParserErrors, MalformedLines) {
    EXPECT_TRUE(contains(parse_error("INPUT a\n").what(), "expected"));
    EXPECT_TRUE(contains(parse_error("WIRE(a)\n").what(), "expected INPUT(net)"));
    EXPECT_TRUE(contains(parse_error("INPUT(a)\ny = AND(a,)\n").what(), "missing net name"));
    EXPECT_TRUE(contains(parse_error("INPUT(a)\n = NOT(a)\n").what(), "missing net name"));
    EXPECT_TRUE(contains(parse_error("INPUT(a)\ny = NOT(a) x\n").what(), "expected NAME(arguments)"));
}

TEST(BenchParserErrors, CycleReportsLineAndNets) {
    const ParseError error = parse_error(
        "INPUT(a)\n"
        "OUTPUT(y)\n"
        "y = NOT(p)\n"
        "p = AND(a, q)\n"
        "q = NOT(p)\n");
    EXPECT_TRUE(contains(error.what(), "combinational cycle"));
    EXPECT_TRUE(contains(error.what(), "p"));
    EXPECT_TRUE(contains(error.what(), "q"));
    EXPECT_TRUE(error.line() == 4 || error.line() == 5);
}

TEST(BenchParserErrors, MissingFile) {
    EXPECT_THROW(parse_bench_file("/nonexistent/file.bench"), ParseError);
}

}  // namespace
}  // namespace faultline
