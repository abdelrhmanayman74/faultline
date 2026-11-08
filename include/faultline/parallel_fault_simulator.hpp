#pragma once

#include <cstddef>
#include <stop_token>
#include <vector>

#include "faultline/fault_simulator.hpp"
#include "faultline/netlist.hpp"
#include "faultline/vectors.hpp"

namespace faultline {

// Same result as run_fault_simulation() (same detected set, same first
// detecting vectors, same fault_simulations count) for any thread count.
//
// The fault list is cut into chunks of consecutive faults, about
// kChunksPerThread per thread. Each chunk is one ThreadPool job that walks all
// blocks in vector order with its own good-circuit and fault simulators, so
// jobs share nothing mutable and need no synchronization until the end.
// DESIGN.md ("Concurrency") explains why this beats a per-block barrier.
//
// `num_threads` must be at least 1; the pool lives only for this call. If
// `stop` is requested before every job is done, the jobs give up at their
// next block and the call throws Cancelled after all of them have returned.
FaultSimReport run_fault_simulation_parallel(const Netlist& netlist,
                                             const std::vector<InputVector>& vectors,
                                             std::size_t num_threads,
                                             std::stop_token stop = {});

// More chunks than threads, so a thread that drew cheap faults takes another
// chunk instead of idling while one thread works through an expensive one.
// Not more than needed: every chunk repeats the good-circuit simulation.
inline constexpr std::size_t kChunksPerThread = 4;

}  // namespace faultline
