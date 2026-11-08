#pragma once

#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "faultline/bit_parallel_simulator.hpp"
#include "faultline/netlist.hpp"
#include "faultline/vectors.hpp"

namespace faultline {

// Single stuck-at fault: `net` is tied to `stuck_at` (0 or 1).
//
// Only stem faults are modelled: the fault sits on the gate output that drives
// the net, so every reader of the net sees the stuck value. A net with fan-out
// also has one "branch" site per reader, where only that reader sees the
// stuck value; those branch faults are NOT in the list. See DESIGN.md for what
// this means for the coverage numbers.
struct Fault {
    NetId net = 0;
    Logic stuck_at = 0;

    bool operator==(const Fault&) const = default;
};

// Stuck-at-0 and stuck-at-1 on every net (primary inputs included), ordered
// by NetId: 2 * netlist.size() faults. No fault collapsing.
std::vector<Fault> stem_fault_list(const Netlist& netlist);

// "<net name> SA0" or "<net name> SA1".
std::string fault_name(const Netlist& netlist, const Fault& fault);

// Result for one fault. first_vector is meaningful only when detected.
struct FaultStatus {
    bool detected = false;
    std::size_t first_vector = 0;  // index of the first vector that detects it

    bool operator==(const FaultStatus&) const = default;  // member-wise; used to compare runs
};

// Parallel-pattern single-fault propagation (PPSFP) against one block.
//
// Designed so work can be split by fault range: simulate_block() reads only
// the netlist, the block and the good values (all read-only), and writes only
// the FaultStatus entries it was given plus its own scratch array. Two
// FaultSimulator objects working on disjoint fault ranges share no mutable
// state. The netlist must outlive the simulator.
class FaultSimulator {
public:
    explicit FaultSimulator(const Netlist& netlist);

    // Simulates every fault in `faults` whose status is not yet detected
    // (fault dropping) against `block`. `good` holds the good-circuit values
    // of the same block (BitParallelSimulator::values()). Marks newly detected
    // faults in `status` (same length as `faults`). Blocks must be passed in
    // increasing vector order for first_vector to be the earliest detection.
    // Returns how many faults were simulated, i.e. were still undetected.
    std::size_t simulate_block(const PatternBlock& block, const std::vector<Word>& good,
                               std::span<const Fault> faults, std::span<FaultStatus> status);

private:
    // Lanes (within `valid`) in which some primary output differs from the
    // good circuit when `fault` is present.
    Word detecting_lanes(const Fault& fault, const std::vector<Word>& good, Word valid);

    const Netlist& netlist_;
    std::vector<std::size_t> position_;  // position_[net]: index of net in eval_order()
    std::vector<Word> faulty_;           // faulty-circuit values; equals `good` between faults
};

struct FaultSimReport {
    std::vector<Fault> faults;
    std::vector<FaultStatus> status;     // status[i] belongs to faults[i]
    std::size_t num_vectors = 0;
    std::size_t fault_simulations = 0;   // (fault, block) pairs simulated; drops with fault dropping

    std::size_t detected_count() const;
    double coverage_percent() const;     // 100 * detected / faults (0 if no faults)
};

// Whole run, single thread: stem fault list, 64-vector blocks, good values
// once per block, then simulate_block() on all faults.
FaultSimReport run_fault_simulation(const Netlist& netlist, const std::vector<InputVector>& vectors);

}  // namespace faultline
