#include "faultline/netlist.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <utility>

#include "faultline/levelize.hpp"

namespace faultline {

namespace {

struct GateTypeName {
    GateType type;
    std::string_view name;
};

// One table drives both directions of the name <-> type conversion, so the
// two can never disagree.
constexpr std::array<GateTypeName, 9> kGateTypeNames{{
    {GateType::Input, "INPUT"},
    {GateType::And, "AND"},
    {GateType::Nand, "NAND"},
    {GateType::Or, "OR"},
    {GateType::Nor, "NOR"},
    {GateType::Xor, "XOR"},
    {GateType::Xnor, "XNOR"},
    {GateType::Not, "NOT"},
    {GateType::Buf, "BUF"},
}};

}  // namespace

std::string_view gate_type_name(GateType type) {
    for (const GateTypeName& entry : kGateTypeNames) {
        if (entry.type == type) {
            return entry.name;
        }
    }
    return "?";
}

std::optional<GateType> gate_type_from_name(std::string_view name) {
    if (name == "BUFF") {
        return GateType::Buf;
    }
    for (const GateTypeName& entry : kGateTypeNames) {
        // INPUT is a declaration in .bench, never a gate on the right of '='.
        if (entry.name == name && entry.type != GateType::Input) {
            return entry.type;
        }
    }
    return std::nullopt;
}

NetId Netlist::add_gate(std::string name, GateType type) {
    assert(!index_of_.count(name) && "caller must reject duplicate names");
    const NetId id = gates_.size();
    index_of_.emplace(name, id);
    gates_.push_back(Gate{.name = std::move(name), .type = type, .fanin = {}, .fanout = {}, .level = 0});
    if (type == GateType::Input) {
        inputs_.push_back(id);
    }
    return id;
}

void Netlist::set_fanin(NetId gate, std::vector<NetId> fanin) {
    gates_[gate].fanin = std::move(fanin);
}

void Netlist::mark_output(NetId net) {
    outputs_.push_back(net);
}

std::optional<NetId> Netlist::find(std::string_view name) const {
    const auto it = index_of_.find(std::string(name));
    if (it == index_of_.end()) {
        return std::nullopt;
    }
    return it->second;
}

void Netlist::finalize() {
    // Fan-out is derived from fan-in, so it is rebuilt from scratch. A gate that
    // reads the same net twice (AND(a, a)) appears twice in that net's fan-out;
    // levelize() counts in-degree the same way, so the two stay consistent.
    for (Gate& gate : gates_) {
        gate.fanout.clear();
    }
    for (NetId id = 0; id < gates_.size(); ++id) {
        for (const NetId source : gates_[id].fanin) {
            gates_[source].fanout.push_back(id);
        }
    }

    Levelization result = levelize(gates_);
    for (NetId id = 0; id < gates_.size(); ++id) {
        gates_[id].level = result.level[id];
    }
    eval_order_ = std::move(result.order);
    depth_ = result.level.empty() ? 0 : *std::max_element(result.level.begin(), result.level.end());
}

}  // namespace faultline
