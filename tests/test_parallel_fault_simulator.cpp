#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <thread>
#include <vector>

#include "faultline/bench_parser.hpp"
#include "faultline/errors.hpp"
#include "faultline/fault_simulator.hpp"
#include "faultline/parallel_fault_simulator.hpp"
#include "faultline/vectors.hpp"

namespace faultline {
namespace {

using namespace std::chrono_literals;

// Thread counts that do and do not divide the fault count evenly, and 8 on
// c17 gives more chunks than faults (one fault per chunk).
const std::vector<std::size_t> kThreadCounts = {1, 2, 3, 4, 8};

void expect_same_report(const FaultSimReport& parallel, const FaultSimReport& serial) {
    EXPECT_EQ(parallel.faults, serial.faults);
    EXPECT_EQ(parallel.status, serial.status);  // detected flags and first vectors
    EXPECT_EQ(parallel.num_vectors, serial.num_vectors);
    EXPECT_EQ(parallel.fault_simulations, serial.fault_simulations);
    EXPECT_EQ(parallel.detected_count(), serial.detected_count());
}

Netlist load_c17() {
    return parse_bench_file(FAULTLINE_SOURCE_DIR "/benchmarks/iscas85/c17.bench");
}

std::string downloaded(const std::string& name) {
    return FAULTLINE_SOURCE_DIR "/benchmarks/downloaded/" + name + ".bench";
}

TEST(ParallelFaultSimulation, C17MatchesSerial) {
    const Netlist netlist = load_c17();
    // 3 vectors only, so some faults stay undetected and the result has both kinds.
    for (const std::size_t count : {std::size_t{3}, std::size_t{200}}) {
        const std::vector<InputVector> vectors = random_vectors(count, 5, 7);
        const FaultSimReport serial = run_fault_simulation(netlist, vectors);
        for (const std::size_t threads : kThreadCounts) {
            SCOPED_TRACE("vectors " + std::to_string(count) + ", threads " + std::to_string(threads));
            expect_same_report(run_fault_simulation_parallel(netlist, vectors, threads), serial);
        }
    }
}

TEST(ParallelFaultSimulation, NoVectorsDetectsNothing) {
    const FaultSimReport report = run_fault_simulation_parallel(load_c17(), {}, 4);
    EXPECT_EQ(report.faults.size(), 22U);
    EXPECT_EQ(report.detected_count(), 0U);
}

TEST(ParallelFaultSimulation, ZeroThreadsIsRejected) {
    EXPECT_THROW(run_fault_simulation_parallel(load_c17(), random_vectors(4, 5, 1), 0),
                 std::invalid_argument);
}

TEST(ParallelFaultSimulation, StopBeforeStartThrowsCancelled) {
    std::stop_source cancel;
    cancel.request_stop();
    EXPECT_THROW(run_fault_simulation_parallel(load_c17(), random_vectors(64, 5, 1), 4,
                                               cancel.get_token()),
                 Cancelled);
}

// A long run (c7552, 20000 vectors: seconds even in a release build) stopped
// shortly after it starts must end with Cancelled, and quickly.
TEST(ParallelFaultSimulation, StopDuringRunThrowsCancelled) {
    const std::string path = downloaded("c7552");
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << path << " not found; run tools/fetch_benchmarks.sh";
    }
    const Netlist netlist = parse_bench_file(path);
    const std::vector<InputVector> vectors =
        random_vectors(20'000, netlist.primary_inputs().size(), 1);
    std::stop_source cancel;
    std::future<FaultSimReport> run = std::async(std::launch::async, [&] {
        return run_fault_simulation_parallel(netlist, vectors, 4, cancel.get_token());
    });
    std::this_thread::sleep_for(20ms);
    cancel.request_stop();
    ASSERT_EQ(run.wait_for(10s), std::future_status::ready);
    EXPECT_THROW(run.get(), Cancelled);
}

// 130 vectors = 2 full blocks and a partial block of 2.
class DownloadedParallel : public ::testing::TestWithParam<const char*> {};

TEST_P(DownloadedParallel, MatchesSerialForEveryThreadCount) {
    const std::string path = downloaded(GetParam());
    if (!std::filesystem::exists(path)) {
        GTEST_SKIP() << path << " not found; run tools/fetch_benchmarks.sh";
    }
    const Netlist netlist = parse_bench_file(path);
    const std::vector<InputVector> vectors = random_vectors(130, netlist.primary_inputs().size(), 1);
    const FaultSimReport serial = run_fault_simulation(netlist, vectors);
    for (const std::size_t threads : kThreadCounts) {
        SCOPED_TRACE("threads " + std::to_string(threads));
        expect_same_report(run_fault_simulation_parallel(netlist, vectors, threads), serial);
    }
}

INSTANTIATE_TEST_SUITE_P(Iscas85, DownloadedParallel,
                         ::testing::Values("c432", "c499", "c880", "c1355", "c1908", "c2670",
                                           "c3540", "c5315", "c6288", "c7552"));

}  // namespace
}  // namespace faultline
