#include "faultline/bit_parallel_simulator.hpp"

#include <algorithm>
#include <stdexcept>
#include <string>

namespace faultline {

std::vector<PatternBlock> pack_vectors(const std::vector<InputVector>& vectors,
                                       std::size_t num_inputs) {
    std::vector<PatternBlock> blocks;
    for (std::size_t first = 0; first < vectors.size(); first += kLanes) {
        PatternBlock block;
        block.first_vector = first;
        block.count = std::min(kLanes, vectors.size() - first);
        block.inputs.assign(num_inputs, 0);
        for (std::size_t lane = 0; lane < block.count; ++lane) {
            const InputVector& vector = vectors[first + lane];
            if (vector.size() != num_inputs) {
                throw std::invalid_argument("vector " + std::to_string(first + lane) + " has " +
                                            std::to_string(vector.size()) + " bits, expected " +
                                            std::to_string(num_inputs));
            }
            for (std::size_t i = 0; i < num_inputs; ++i) {
                block.inputs[i] |= Word{vector[i] & 1U} << lane;
            }
        }
        blocks.push_back(std::move(block));
    }
    return blocks;
}

Word evaluate_gate_word(const Gate& gate, const std::vector<Word>& values) {
    // Same structure as the scalar evaluate_gate(): compute AND, OR or XOR of
    // the fan-in, then invert for NAND/NOR/XNOR/NOT. The invert must be ~ (all
    // 64 bits); the scalar code's "^ 1" would only flip lane 0.
    Word result = 0;
    bool invert = false;
    switch (gate.type) {
        case GateType::Nand:
            invert = true;
            [[fallthrough]];
        case GateType::And:
            result = kAllOnes;
            for (const NetId in : gate.fanin) {
                result &= values[in];
            }
            break;
        case GateType::Nor:
            invert = true;
            [[fallthrough]];
        case GateType::Or:
            for (const NetId in : gate.fanin) {
                result |= values[in];
            }
            break;
        case GateType::Xnor:
            invert = true;
            [[fallthrough]];
        case GateType::Xor:
            for (const NetId in : gate.fanin) {
                result ^= values[in];
            }
            break;
        case GateType::Not:
            invert = true;
            [[fallthrough]];
        case GateType::Buf:
            result = values[gate.fanin.front()];
            break;
        case GateType::Input:
            throw std::logic_error("evaluate_gate_word called on an INPUT pseudo-gate");
    }
    return invert ? ~result : result;
}

BitParallelSimulator::BitParallelSimulator(const Netlist& netlist)
    : netlist_(netlist), values_(netlist.size(), 0) {}

void BitParallelSimulator::simulate(const PatternBlock& block) {
    const std::vector<NetId>& pis = netlist_.primary_inputs();
    if (block.inputs.size() != pis.size()) {
        throw std::invalid_argument("pattern block has " + std::to_string(block.inputs.size()) +
                                    " input words, circuit has " + std::to_string(pis.size()) +
                                    " primary inputs");
    }
    for (std::size_t i = 0; i < pis.size(); ++i) {
        values_[pis[i]] = block.inputs[i];
    }
    // One topological pass, exactly like the scalar simulator.
    for (const NetId id : netlist_.eval_order()) {
        const Gate& gate = netlist_.gate(id);
        if (gate.type != GateType::Input) {
            values_[id] = evaluate_gate_word(gate, values_);
        }
    }
}

std::vector<Logic> BitParallelSimulator::outputs(std::size_t lane) const {
    std::vector<Logic> result;
    result.reserve(netlist_.primary_outputs().size());
    for (const NetId po : netlist_.primary_outputs()) {
        result.push_back(lane_value(values_[po], lane));
    }
    return result;
}

}  // namespace faultline
