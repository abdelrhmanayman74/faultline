#include "faultline/fault_simulator.hpp"

#include <algorithm>
#include <bit>
#include <cassert>

namespace faultline {

std::vector<Fault> stem_fault_list(const Netlist& netlist) {
    std::vector<Fault> faults;
    faults.reserve(2 * netlist.size());
    for (NetId net = 0; net < netlist.size(); ++net) {
        faults.push_back(Fault{.net = net, .stuck_at = 0});
        faults.push_back(Fault{.net = net, .stuck_at = 1});
    }
    return faults;
}

std::string fault_name(const Netlist& netlist, const Fault& fault) {
    return netlist.gate(fault.net).name + (fault.stuck_at ? " SA1" : " SA0");
}

FaultSimulator::FaultSimulator(const Netlist& netlist)
    : netlist_(netlist), position_(netlist.size(), 0), faulty_(netlist.size(), 0) {
    const std::vector<NetId>& order = netlist.eval_order();
    for (std::size_t i = 0; i < order.size(); ++i) {
        position_[order[i]] = i;
    }
}

std::size_t FaultSimulator::simulate_block(const PatternBlock& block, const std::vector<Word>& good,
                                           std::span<const Fault> faults,
                                           std::span<FaultStatus> status) {
    assert(faults.size() == status.size());
    assert(good.size() == netlist_.size());
    faulty_ = good;  // detecting_lanes() restores this state after every fault
    const Word valid = block.valid_mask();
    std::size_t simulated = 0;
    for (std::size_t i = 0; i < faults.size(); ++i) {
        if (status[i].detected) {
            continue;  // fault dropping: one detection is enough
        }
        ++simulated;
        const Word lanes = detecting_lanes(faults[i], good, valid);
        if (lanes != 0) {
            // Lowest set bit = earliest vector of this block that detects it.
            const auto lane = static_cast<std::size_t>(std::countr_zero(lanes));
            status[i] = FaultStatus{.detected = true, .first_vector = block.first_vector + lane};
        }
    }
    return simulated;
}

// Why re-evaluate the suffix of eval_order() and not just the fan-out cone?
// Every gate the fault can reach comes after the faulty net in topological
// order, so re-evaluating that suffix is correct for any valid order, needs no
// extra data structure, and gates outside the cone simply recompute their good
// value. An event-driven cone walk would skip those gates but needs a queue
// ordered by level and per-gate "changed" flags; that is a later optimization.
Word FaultSimulator::detecting_lanes(const Fault& fault, const std::vector<Word>& good, Word valid) {
    const Word forced = fault.stuck_at ? kAllOnes : Word{0};
    if (((forced ^ good[fault.net]) & valid) == 0) {
        // The good value already equals the stuck value in every valid lane:
        // the fault is not activated, so the faulty circuit equals the good one.
        return 0;
    }

    const std::vector<NetId>& order = netlist_.eval_order();
    const std::size_t start = position_[fault.net];
    faulty_[fault.net] = forced;  // never re-evaluated: the loop starts after it
    for (std::size_t i = start + 1; i < order.size(); ++i) {
        const Gate& gate = netlist_.gate(order[i]);
        if (gate.type != GateType::Input) {
            faulty_[order[i]] = evaluate_gate_word(gate, faulty_);
        }
    }

    Word differ = 0;
    for (const NetId po : netlist_.primary_outputs()) {
        differ |= faulty_[po] ^ good[po];
    }

    // Undo this fault so faulty_ equals good again for the next one. Only
    // positions >= start were written.
    for (std::size_t i = start; i < order.size(); ++i) {
        faulty_[order[i]] = good[order[i]];
    }
    return differ & valid;
}

std::size_t FaultSimReport::detected_count() const {
    return static_cast<std::size_t>(
        std::count_if(status.begin(), status.end(), [](const FaultStatus& s) { return s.detected; }));
}

double FaultSimReport::coverage_percent() const {
    if (faults.empty()) {
        return 0.0;
    }
    return 100.0 * static_cast<double>(detected_count()) / static_cast<double>(faults.size());
}

FaultSimReport run_fault_simulation(const Netlist& netlist, const std::vector<InputVector>& vectors) {
    FaultSimReport report;
    report.faults = stem_fault_list(netlist);
    report.status.assign(report.faults.size(), FaultStatus{});
    report.num_vectors = vectors.size();

    BitParallelSimulator good_simulator(netlist);
    FaultSimulator fault_simulator(netlist);
    for (const PatternBlock& block : pack_vectors(vectors, netlist.primary_inputs().size())) {
        good_simulator.simulate(block);  // good values once per block, shared by all faults
        const std::size_t simulated = fault_simulator.simulate_block(
            block, good_simulator.values(), report.faults, report.status);
        if (simulated == 0) {
            break;  // every fault was already detected
        }
        report.fault_simulations += simulated;
    }
    return report;
}

}  // namespace faultline
