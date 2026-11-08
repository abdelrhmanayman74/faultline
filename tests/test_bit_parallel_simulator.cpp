#include <gtest/gtest.h>

#include <filesystem>
#include <sstream>
#include <string>
#include <vector>

#include "faultline/bench_parser.hpp"
#include "faultline/bit_parallel_simulator.hpp"
#include "faultline/scalar_simulator.hpp"
#include "faultline/vectors.hpp"

namespace faultline {
namespace {

// Simulates `vectors` both ways and requires every net (not only the outputs)
// to have the same value in every vector.
void expect_same_as_scalar(const Netlist& netlist, const std::vector<InputVector>& vectors) {
    ScalarSimulator scalar(netlist);
    BitParallelSimulator parallel(netlist);
    for (const PatternBlock& block : pack_vectors(vectors, netlist.primary_inputs().size())) {
        parallel.simulate(block);
        for (std::size_t lane = 0; lane < block.count; ++lane) {
            const std::size_t index = block.first_vector + lane;
            scalar.simulate(vectors[index]);
            for (NetId net = 0; net < netlist.size(); ++net) {
                ASSERT_EQ(lane_value(parallel.values()[net], lane), scalar.value(net))
                    << "net " << netlist.gate(net).name << ", vector " << index;
            }
        }
    }
}

std::vector<InputVector> exhaustive_vectors(std::size_t num_inputs) {
    std::vector<InputVector> vectors;
    for (std::size_t v = 0; v < (std::size_t{1} << num_inputs); ++v) {
        InputVector vector(num_inputs);
        for (std::size_t i = 0; i < num_inputs; ++i) {
            vector[i] = static_cast<Logic>((v >> (num_inputs - 1 - i)) & 1U);
        }
        vectors.push_back(vector);
    }
    return vectors;
}

TEST(PackVectors, BlocksAndValidMask) {
    const std::vector<InputVector> vectors = random_vectors(130, 3, 7);
    const std::vector<PatternBlock> blocks = pack_vectors(vectors, 3);
    ASSERT_EQ(blocks.size(), 3U);
    EXPECT_EQ(blocks[0].valid_mask(), kAllOnes);
    EXPECT_EQ(blocks[1].first_vector, 64U);
    EXPECT_EQ(blocks[2].count, 2U);
    EXPECT_EQ(blocks[2].valid_mask(), Word{0b11});
    // Lane 1 of block 2 is vector 129, input 2.
    EXPECT_EQ(lane_value(blocks[2].inputs[2], 1), vectors[129][2]);
    // Padding lanes are zero.
    EXPECT_EQ(blocks[2].inputs[0] & ~blocks[2].valid_mask(), Word{0});
    EXPECT_TRUE(pack_vectors({}, 3).empty());
    EXPECT_THROW(pack_vectors({InputVector{1, 0}}, 3), std::invalid_argument);
}

TEST(BitParallelSimulator, C17ExhaustiveMatchesScalar) {
    const Netlist netlist = parse_bench_file(FAULTLINE_SOURCE_DIR "/benchmarks/iscas85/c17.bench");
    expect_same_as_scalar(netlist, exhaustive_vectors(5));  // 32 vectors: one partial block
}

TEST(BitParallelSimulator, AllGateTypesMatchScalar) {
    std::istringstream in(
        "INPUT(a)\nINPUT(b)\nINPUT(c)\n"
        "OUTPUT(and)\nOUTPUT(nand)\nOUTPUT(or)\nOUTPUT(nor)\n"
        "OUTPUT(xor)\nOUTPUT(xnor)\nOUTPUT(not)\nOUTPUT(buf)\nOUTPUT(twice)\n"
        "and = AND(a, b, c)\nnand = NAND(a, b, c)\nor = OR(a, b, c)\nnor = NOR(a, b, c)\n"
        "xor = XOR(a, b, c)\nxnor = XNOR(a, b, c)\nnot = NOT(a)\nbuf = BUF(a)\n"
        "twice = XOR(a, a)\n");
    expect_same_as_scalar(parse_bench(in, "gates.bench"), exhaustive_vectors(3));
}

// 200 random vectors = 3 full blocks and one partial block of 8.
class DownloadedCircuit : public ::testing::TestWithParam<const char*> {};

TEST_P(DownloadedCircuit, RandomVectorsMatchScalar) {
    const std::string path =
        std::string(FAULTLINE_SOURCE_DIR "/benchmarks/downloaded/") + GetParam() + ".bench";
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << path << " not found; run tools/fetch_benchmarks.sh";
    }
    const Netlist netlist = parse_bench_file(path);
    expect_same_as_scalar(netlist, random_vectors(200, netlist.primary_inputs().size(), 1));
}

INSTANTIATE_TEST_SUITE_P(Iscas85, DownloadedCircuit,
                         ::testing::Values("c432", "c499", "c880", "c1355", "c1908", "c2670",
                                           "c3540", "c5315", "c6288", "c7552"));

TEST(RandomVectors, DeterministicPerSeed) {
    EXPECT_EQ(random_vectors(10, 100, 5), random_vectors(10, 100, 5));
    EXPECT_NE(random_vectors(10, 100, 5), random_vectors(10, 100, 6));
}

}  // namespace
}  // namespace faultline
