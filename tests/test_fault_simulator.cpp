#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "faultline/bench_parser.hpp"
#include "faultline/bit_parallel_simulator.hpp"
#include "faultline/fault_simulator.hpp"
#include "faultline/vectors.hpp"

namespace faultline {
namespace {

Netlist load_c17() {
    return parse_bench_file(FAULTLINE_SOURCE_DIR "/benchmarks/iscas85/c17.bench");
}

InputVector bits(const std::string& text) {
    InputVector values;
    for (const char c : text) {
        values.push_back(c == '1' ? 1 : 0);
    }
    return values;
}

// c17 inputs (1, 2, 3, 6, 7); vector v is the 5-bit binary form of v.
std::vector<InputVector> c17_exhaustive() {
    std::vector<InputVector> vectors;
    for (unsigned v = 0; v < 32; ++v) {
        InputVector vector;
        for (int bit = 4; bit >= 0; --bit) {
            vector.push_back(static_cast<Logic>((v >> bit) & 1U));
        }
        vectors.push_back(vector);
    }
    return vectors;
}

// Index of a fault in stem_fault_list() order (2 * net + stuck_at).
std::size_t fault_index(const Netlist& netlist, const std::string& net, Logic stuck_at) {
    return 2 * netlist.find(net).value() + stuck_at;
}

TEST(FaultList, TwoStemFaultsPerNet) {
    const Netlist netlist = load_c17();
    const std::vector<Fault> faults = stem_fault_list(netlist);
    ASSERT_EQ(faults.size(), 22U);  // 11 nets: 5 inputs + 6 NANDs
    const std::size_t i = fault_index(netlist, "10", 1);
    EXPECT_EQ(fault_name(netlist, faults[i]), "10 SA1");
}

// Worked by hand. 10 = NAND(1, 3) is 0 only when 1 = 3 = 1, so 10 SA1 needs
// such a vector; the first in counting order is 10100 (index 20). There
// 2 = 0, so 16 = NAND(2, 11) = 1 and 22 = NAND(10, 16) = NOT 10: good 22 = 1,
// faulty 22 = 0. Output 22 SA0 needs 22 = 1; the truth table shows the first
// such vector is 01000 (index 8).
TEST(FaultSimulation, C17HandCheckedDetections) {
    const Netlist netlist = load_c17();
    const FaultSimReport report = run_fault_simulation(netlist, c17_exhaustive());

    const FaultStatus& sa1_on_10 = report.status[fault_index(netlist, "10", 1)];
    EXPECT_TRUE(sa1_on_10.detected);
    EXPECT_EQ(sa1_on_10.first_vector, 20U);

    const FaultStatus& sa0_on_22 = report.status[fault_index(netlist, "22", 0)];
    EXPECT_TRUE(sa0_on_22.detected);
    EXPECT_EQ(sa0_on_22.first_vector, 8U);

    // c17 has no redundant logic: all 22 stem faults are testable.
    EXPECT_EQ(report.detected_count(), 22U);
    EXPECT_DOUBLE_EQ(report.coverage_percent(), 100.0);
}

TEST(FaultSimulation, SingleVectorDetectsOnlyItsFaults) {
    const Netlist netlist = load_c17();
    // 10100: good outputs 22 = 1, 23 = 0, so 22 SA1 and 23 SA0 are not detected.
    const FaultSimReport report = run_fault_simulation(netlist, {bits("10100")});
    EXPECT_FALSE(report.status[fault_index(netlist, "22", 1)].detected);
    EXPECT_FALSE(report.status[fault_index(netlist, "23", 0)].detected);
    EXPECT_TRUE(report.status[fault_index(netlist, "22", 0)].detected);
    EXPECT_TRUE(report.status[fault_index(netlist, "23", 1)].detected);
}

// Block 0 is 64 copies of 10100, block 1 the 32 exhaustive vectors. 10 SA1 is
// detected in both blocks; with fault dropping it is simulated only in block 0,
// so its first vector stays 0 (re-simulating it would report 64 + 20).
TEST(FaultSimulation, DetectedFaultsAreDropped) {
    const Netlist netlist = load_c17();
    const std::vector<InputVector> block0(64, bits("10100"));
    std::vector<InputVector> both = block0;
    for (const InputVector& vector : c17_exhaustive()) {
        both.push_back(vector);
    }

    const std::size_t left_after_block0 = 22 - run_fault_simulation(netlist, block0).detected_count();
    ASSERT_GT(left_after_block0, 0U);
    const FaultSimReport report = run_fault_simulation(netlist, both);
    EXPECT_EQ(report.status[fault_index(netlist, "10", 1)].first_vector, 0U);
    EXPECT_EQ(report.fault_simulations, 22 + left_after_block0);
    EXPECT_EQ(report.detected_count(), 22U);
}

TEST(FaultSimulator, SkipsFaultsAlreadyDetected) {
    const Netlist netlist = load_c17();
    const std::vector<Fault> faults = stem_fault_list(netlist);
    std::vector<FaultStatus> status(faults.size());
    const std::size_t i = fault_index(netlist, "22", 0);
    status[i] = FaultStatus{.detected = true, .first_vector = 999};

    const PatternBlock block = pack_vectors(c17_exhaustive(), 5).front();
    BitParallelSimulator good(netlist);
    good.simulate(block);
    FaultSimulator simulator(netlist);
    EXPECT_EQ(simulator.simulate_block(block, good.values(), faults, status), 21U);
    EXPECT_EQ(status[i].first_vector, 999U);
}

// Lane 0 holds 10100 (22 = 1), every other lane holds 00000 (22 = 0). 00000
// detects 22 SA1, so only the valid-lanes mask keeps it from being reported.
TEST(FaultSimulator, IgnoresLanesOutsideTheBlock) {
    const Netlist netlist = load_c17();
    const std::vector<Fault> faults = stem_fault_list(netlist);
    const std::size_t i = fault_index(netlist, "22", 1);

    PatternBlock block;
    block.first_vector = 128;
    block.inputs = {1, 0, 1, 0, 0};  // lane 0 = 10100, lanes 1..63 = 0
    BitParallelSimulator good(netlist);
    good.simulate(block);
    FaultSimulator simulator(netlist);

    block.count = 1;
    std::vector<FaultStatus> status(faults.size());
    simulator.simulate_block(block, good.values(), faults, status);
    EXPECT_FALSE(status[i].detected);

    block.count = 2;  // now lane 1 (00000) is real
    status.assign(faults.size(), FaultStatus{});
    simulator.simulate_block(block, good.values(), faults, status);
    EXPECT_TRUE(status[i].detected);
    EXPECT_EQ(status[i].first_vector, 129U);
}

TEST(FaultSimulation, NoVectorsDetectsNothing) {
    const Netlist netlist = load_c17();
    const FaultSimReport report = run_fault_simulation(netlist, {});
    EXPECT_EQ(report.faults.size(), 22U);
    EXPECT_EQ(report.detected_count(), 0U);
    EXPECT_EQ(report.fault_simulations, 0U);
}

}  // namespace
}  // namespace faultline
