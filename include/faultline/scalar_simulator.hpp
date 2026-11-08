#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "faultline/netlist.hpp"

namespace faultline {

// One logic value per net, 0 or 1.
// Why std::uint8_t and not std::vector<bool>? vector<bool> packs bits and
// returns proxy objects instead of real references, so every read is a shift
// and mask and the data cannot be viewed through std::span. One byte per net
// is simple and fast; the bit-parallel simulator uses the same layout with
// one std::uint64_t (64 vectors) per net.
using Logic = std::uint8_t;

// Evaluates one gate from the values of its fan-in nets.
// Not valid for GateType::Input (inputs have no function).
Logic evaluate_gate(const Gate& gate, const std::vector<Logic>& values);

// Simulates one input vector at a time, zero delay, in topological order.
// Keeps a reference to the netlist: the netlist must outlive the simulator.
class ScalarSimulator {
public:
    explicit ScalarSimulator(const Netlist& netlist);

    // `inputs` holds one value per primary input, in primary_inputs() order.
    // Returns one value per primary output, in primary_outputs() order.
    std::vector<Logic> simulate(std::span<const Logic> inputs);

    // Value of any net after the last simulate() call.
    Logic value(NetId net) const { return values_[net]; }

private:
    const Netlist& netlist_;
    std::vector<Logic> values_;  // indexed by NetId; reused between calls
};

}  // namespace faultline
