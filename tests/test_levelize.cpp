#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "faultline/errors.hpp"
#include "faultline/levelize.hpp"
#include "faultline/netlist.hpp"

namespace faultline {
namespace {

// Builds a netlist by hand (no parser) so these tests check levelize alone.
// Gates are added in reverse topological order on purpose: the evaluation
// order must not depend on the order of definition.
Netlist make_chain() {
    Netlist netlist;
    const NetId out = netlist.add_gate("out", GateType::Nand);
    const NetId mid = netlist.add_gate("mid", GateType::Not);
    const NetId a = netlist.add_gate("a", GateType::Input);
    const NetId b = netlist.add_gate("b", GateType::Input);
    netlist.set_fanin(out, {mid, b});
    netlist.set_fanin(mid, {a});
    netlist.mark_output(out);
    netlist.finalize();
    return netlist;
}

TEST(Levelize, OrderPutsEveryGateAfterItsFanin) {
    const Netlist netlist = make_chain();
    const std::vector<NetId>& order = netlist.eval_order();
    ASSERT_EQ(order.size(), netlist.size());

    std::vector<std::size_t> position(netlist.size());
    for (std::size_t i = 0; i < order.size(); ++i) {
        position[order[i]] = i;
    }
    for (NetId id = 0; id < netlist.size(); ++id) {
        for (const NetId source : netlist.gate(id).fanin) {
            EXPECT_LT(position[source], position[id]) << netlist.gate(id).name;
        }
    }
}

TEST(Levelize, LevelsAreLongestPathFromInputs) {
    const Netlist netlist = make_chain();
    EXPECT_EQ(netlist.gate(*netlist.find("a")).level, 0);
    EXPECT_EQ(netlist.gate(*netlist.find("b")).level, 0);
    EXPECT_EQ(netlist.gate(*netlist.find("mid")).level, 1);
    EXPECT_EQ(netlist.gate(*netlist.find("out")).level, 2);  // max(1, 0) + 1
    EXPECT_EQ(netlist.depth(), 2);
}

TEST(Levelize, CycleIsDetectedAndNamed) {
    // a -> x -> y -> z -> x (loop x, y, z), plus w hanging off the loop.
    Netlist netlist;
    const NetId a = netlist.add_gate("a", GateType::Input);
    const NetId x = netlist.add_gate("x", GateType::And);
    const NetId y = netlist.add_gate("y", GateType::Not);
    const NetId z = netlist.add_gate("z", GateType::Buf);
    const NetId w = netlist.add_gate("w", GateType::Not);
    netlist.set_fanin(x, {a, z});
    netlist.set_fanin(y, {x});
    netlist.set_fanin(z, {y});
    netlist.set_fanin(w, {z});
    try {
        netlist.finalize();
        FAIL() << "expected CycleError";
    } catch (const CycleError& error) {
        std::vector<std::string> names = error.cycle();
        std::sort(names.begin(), names.end());
        EXPECT_EQ(names, (std::vector<std::string>{"x", "y", "z"}));
        // Signal-flow order: each net drives the next, the last drives the first.
        EXPECT_NE(std::string(error.what()).find("->"), std::string::npos);
    }
}

TEST(Levelize, SelfLoopIsACycle) {
    Netlist netlist;
    const NetId a = netlist.add_gate("a", GateType::Input);
    const NetId s = netlist.add_gate("s", GateType::Or);
    netlist.set_fanin(s, {a, s});
    try {
        netlist.finalize();
        FAIL() << "expected CycleError";
    } catch (const CycleError& error) {
        EXPECT_EQ(error.cycle(), (std::vector<std::string>{"s"}));
    }
}

}  // namespace
}  // namespace faultline
