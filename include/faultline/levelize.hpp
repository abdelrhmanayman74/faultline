#pragma once

#include <vector>

#include "faultline/netlist.hpp"

namespace faultline {

struct Levelization {
    std::vector<NetId> order;  // topological order of all nets
    std::vector<int> level;    // level[net]: 0 for inputs, else 1 + max fan-in level
};

// Kahn's algorithm. Reads only gate types and fan-in/fan-out lists, so it can
// run before the Netlist stores its results. Throws CycleError naming the nets
// of one loop if the graph is not acyclic.
Levelization levelize(const std::vector<Gate>& gates);

}  // namespace faultline
