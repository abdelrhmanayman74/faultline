#include "faultline/scalar_simulator.hpp"

#include <cassert>
#include <stdexcept>
#include <string>

namespace faultline {

namespace {

// AND/NAND, OR/NOR and XOR/XNOR share a base function; the second gate of each
// pair inverts its output. Values are 0/1, so bitwise &, | and ^ on them act
// as logical AND, OR and XOR.
bool is_inverting(GateType type) {
    return type == GateType::Nand || type == GateType::Nor || type == GateType::Xnor ||
           type == GateType::Not;
}

Logic base_function(const Gate& gate, const std::vector<Logic>& values) {
    switch (gate.type) {
        case GateType::And:
        case GateType::Nand: {
            Logic result = 1;
            for (const NetId in : gate.fanin) {
                result &= values[in];
            }
            return result;
        }
        case GateType::Or:
        case GateType::Nor: {
            Logic result = 0;
            for (const NetId in : gate.fanin) {
                result |= values[in];
            }
            return result;
        }
        case GateType::Xor:
        case GateType::Xnor: {
            Logic result = 0;
            for (const NetId in : gate.fanin) {
                result ^= values[in];
            }
            return result;
        }
        case GateType::Not:
        case GateType::Buf:
            return values[gate.fanin.front()];
        case GateType::Input:
            break;
    }
    throw std::logic_error("evaluate_gate called on an INPUT pseudo-gate");
}

}  // namespace

Logic evaluate_gate(const Gate& gate, const std::vector<Logic>& values) {
    const Logic base = base_function(gate, values);
    return is_inverting(gate.type) ? static_cast<Logic>(base ^ 1U) : base;
}

ScalarSimulator::ScalarSimulator(const Netlist& netlist)
    : netlist_(netlist), values_(netlist.size(), 0) {}

std::vector<Logic> ScalarSimulator::simulate(std::span<const Logic> inputs) {
    const std::vector<NetId>& pis = netlist_.primary_inputs();
    if (inputs.size() != pis.size()) {
        throw std::invalid_argument("expected " + std::to_string(pis.size()) +
                                    " input values, got " + std::to_string(inputs.size()));
    }
    for (std::size_t i = 0; i < pis.size(); ++i) {
        assert(inputs[i] <= 1);
        values_[pis[i]] = inputs[i];
    }

    // Topological order guarantees every fan-in value is final before a gate
    // reads it, so one pass gives the settled (zero-delay) result.
    for (const NetId id : netlist_.eval_order()) {
        const Gate& gate = netlist_.gate(id);
        if (gate.type != GateType::Input) {
            values_[id] = evaluate_gate(gate, values_);
        }
    }

    std::vector<Logic> outputs;
    outputs.reserve(netlist_.primary_outputs().size());
    for (const NetId po : netlist_.primary_outputs()) {
        outputs.push_back(values_[po]);
    }
    return outputs;
}

}  // namespace faultline
