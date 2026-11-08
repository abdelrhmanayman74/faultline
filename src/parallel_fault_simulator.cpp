#include "faultline/parallel_fault_simulator.hpp"

#include <algorithm>
#include <future>
#include <numeric>
#include <span>
#include <stdexcept>

#include "faultline/bit_parallel_simulator.hpp"
#include "faultline/errors.hpp"
#include "faultline/thread_pool.hpp"

namespace faultline {
namespace {

// One job: the whole single-thread algorithm, restricted to one fault range.
// Reads only shared read-only data (netlist, blocks); writes only its own
// simulators and its own slice of the status array. Returns its share of
// FaultSimReport::fault_simulations.
std::size_t simulate_chunk(const Netlist& netlist, const std::vector<PatternBlock>& blocks,
                           std::span<const Fault> faults, std::span<FaultStatus> status,
                           const std::stop_token& stop) {
    BitParallelSimulator good_simulator(netlist);
    FaultSimulator fault_simulator(netlist);
    std::size_t simulations = 0;
    // Blocks in vector order, as in the single-thread loop: the first block
    // that detects a fault is the earliest one, whichever thread runs it.
    for (const PatternBlock& block : blocks) {
        if (stop.stop_requested()) {
            throw Cancelled();  // stored in this job's future by packaged_task
        }
        good_simulator.simulate(block);
        const std::size_t simulated =
            fault_simulator.simulate_block(block, good_simulator.values(), faults, status);
        if (simulated == 0) {
            break;  // every fault of this chunk is detected: no other chunk is affected
        }
        simulations += simulated;
    }
    return simulations;
}

}  // namespace

FaultSimReport run_fault_simulation_parallel(const Netlist& netlist,
                                             const std::vector<InputVector>& vectors,
                                             std::size_t num_threads, std::stop_token stop) {
    if (num_threads == 0) {
        throw std::invalid_argument("run_fault_simulation_parallel needs at least one thread");
    }
    // Everything the jobs touch is declared before the pool, so it outlives
    // the jobs even if this function leaves by an exception.
    FaultSimReport report;
    report.faults = stem_fault_list(netlist);
    report.status.assign(report.faults.size(), FaultStatus{});
    report.num_vectors = vectors.size();
    const std::vector<PatternBlock> blocks = pack_vectors(vectors, netlist.primary_inputs().size());

    const std::size_t num_faults = report.faults.size();
    const std::size_t num_chunks = std::min(num_faults, num_threads * kChunksPerThread);
    std::vector<std::size_t> simulations(num_chunks, 0);  // element c written only by job c
    {
        // Capacity = number of jobs, so submit() never blocks here.
        ThreadPool pool(num_threads, std::max<std::size_t>(num_chunks, 1));
        std::vector<std::future<void>> jobs;
        jobs.reserve(num_chunks);
        const std::span<const Fault> all_faults(report.faults);
        const std::span<FaultStatus> all_status(report.status);
        for (std::size_t c = 0; c < num_chunks; ++c) {
            // Even split; sizes differ by at most one fault.
            const std::size_t begin = c * num_faults / num_chunks;
            const std::size_t count = (c + 1) * num_faults / num_chunks - begin;
            jobs.push_back(pool.submit([&netlist, &blocks, &simulations, stop, c,
                                        faults = all_faults.subspan(begin, count),
                                        status = all_status.subspan(begin, count)] {
                simulations[c] = simulate_chunk(netlist, blocks, faults, status, stop);
            }));
        }
        // Wait for every job before get(): get() may throw, and no job may
        // still be running when we leave this scope and the data goes away.
        for (const std::future<void>& job : jobs) {
            job.wait();
        }
        for (std::future<void>& job : jobs) {
            job.get();  // rethrows a job's exception (Cancelled, std::bad_alloc, ...)
        }
    }  // ~ThreadPool joins the (now idle) workers

    // future::get() synchronizes with the end of the job, so these plain reads
    // of what the jobs wrote are not data races.
    report.fault_simulations = std::accumulate(simulations.begin(), simulations.end(), std::size_t{0});
    return report;
}

}  // namespace faultline
