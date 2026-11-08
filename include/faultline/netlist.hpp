#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace faultline {

// In a .bench file every net is driven by exactly one gate (primary inputs are
// driven by an INPUT pseudo-gate). So one index identifies both the net and the
// gate that drives it. std::size_t indexes std::vector directly, no casts.
using NetId = std::size_t;

enum class GateType {
    Input,  // primary input pseudo-gate: no fan-in, value comes from the vector
    And,
    Nand,
    Or,
    Nor,
    Xor,  // multi-input XOR is parity (odd number of 1s), same as Verilog's xor
    Xnor,
    Not,
    Buf,
};

// Gate name as written in .bench ("NAND", "BUF", ...); used for messages.
std::string_view gate_type_name(GateType type);

// Parses a .bench keyword. Accepts both "BUF" and "BUFF" because the public
// ISCAS-85 files use "BUFF". Returns nullopt for unknown names (including DFF,
// which the parser reports separately).
std::optional<GateType> gate_type_from_name(std::string_view name);

struct Gate {
    std::string name;           // name of the net this gate drives
    GateType type = GateType::Input;
    std::vector<NetId> fanin;   // nets read by this gate, in file order
    std::vector<NetId> fanout;  // gates that read this net (filled by finalize)
    int level = 0;              // 0 for inputs, else 1 + max level of fan-in
};

// Gate-level combinational circuit.
//
// Storage is a flat std::vector<Gate> addressed by NetId, plus a name->index
// map. Flat arrays keep later passes simple and cache friendly: a simulator
// keeps its own per-net value array of the same size and walks eval_order().
//
// Build protocol: add_gate() for every net, set_fanin(), mark_output(), then
// finalize() once. finalize() computes fan-out and levels and checks for cycles.
class Netlist {
public:
    // Adds a gate driving a new net. The name must be new (checked by caller,
    // which knows the source line); this function asserts it in debug builds.
    NetId add_gate(std::string name, GateType type);
    void set_fanin(NetId gate, std::vector<NetId> fanin);
    void mark_output(NetId net);

    // Computes fan-out lists, levels and evaluation order.
    // Throws CycleError (see levelize.hpp) if the circuit has a loop.
    void finalize();

    std::size_t size() const { return gates_.size(); }
    const Gate& gate(NetId id) const { return gates_[id]; }
    const std::vector<Gate>& gates() const { return gates_; }
    std::optional<NetId> find(std::string_view name) const;

    const std::vector<NetId>& primary_inputs() const { return inputs_; }
    const std::vector<NetId>& primary_outputs() const { return outputs_; }

    // Every net in topological order: a gate always comes after its fan-in.
    // Inputs come first. Valid only after finalize().
    const std::vector<NetId>& eval_order() const { return eval_order_; }

    // Largest gate level (0 for a circuit made only of inputs).
    int depth() const { return depth_; }

    // Number of real logic gates (INPUT pseudo-gates not counted).
    std::size_t logic_gate_count() const { return gates_.size() - inputs_.size(); }

private:
    std::vector<Gate> gates_;
    // find() builds a temporary std::string for the lookup. That copy is fine:
    // name lookups happen while parsing, never inside simulation loops.
    std::unordered_map<std::string, NetId> index_of_;
    std::vector<NetId> inputs_;
    std::vector<NetId> outputs_;
    std::vector<NetId> eval_order_;
    int depth_ = 0;
};

}  // namespace faultline
