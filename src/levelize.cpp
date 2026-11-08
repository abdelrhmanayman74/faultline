#include "faultline/levelize.hpp"

#include <algorithm>
#include <string>

#include "faultline/errors.hpp"

namespace faultline {

namespace {

// Called when Kahn's algorithm could not place every net. Every unplaced net
// still has at least one unplaced fan-in (otherwise its in-degree would have
// reached zero). So walking backwards through unplaced fan-ins never gets stuck,
// and because the graph is finite the walk must revisit a net: the part of the
// walk from that net onwards is a loop.
std::vector<std::string> find_cycle(const std::vector<Gate>& gates,
                                    const std::vector<bool>& placed) {
    NetId current = 0;
    while (placed[current]) {
        ++current;
    }

    constexpr int kNotOnPath = -1;
    std::vector<int> position_on_path(gates.size(), kNotOnPath);
    std::vector<NetId> path;
    while (position_on_path[current] == kNotOnPath) {
        position_on_path[current] = static_cast<int>(path.size());
        path.push_back(current);
        for (const NetId source : gates[current].fanin) {
            if (!placed[source]) {
                current = source;
                break;
            }
        }
    }

    // path[k + 1] drives path[k]. Keep the loop part and reverse it so the
    // names read in signal-flow order.
    std::vector<std::string> cycle;
    const auto loop_start = static_cast<std::size_t>(position_on_path[current]);
    for (std::size_t k = path.size(); k > loop_start; --k) {
        cycle.push_back(gates[path[k - 1]].name);
    }
    return cycle;
}

}  // namespace

Levelization levelize(const std::vector<Gate>& gates) {
    Levelization result;
    result.level.assign(gates.size(), 0);
    result.order.reserve(gates.size());

    // remaining_inputs[g] = fan-in nets of g not yet placed in the order.
    std::vector<std::size_t> remaining_inputs(gates.size());
    for (NetId id = 0; id < gates.size(); ++id) {
        remaining_inputs[id] = gates[id].fanin.size();
        if (remaining_inputs[id] == 0) {
            result.order.push_back(id);
        }
    }

    // result.order doubles as the FIFO queue: entries before `next` are done,
    // entries after it are ready but not processed yet. Processing in index
    // order makes the result deterministic for a given file.
    for (std::size_t next = 0; next < result.order.size(); ++next) {
        const NetId ready = result.order[next];
        for (const NetId reader : gates[ready].fanout) {
            result.level[reader] = std::max(result.level[reader], result.level[ready] + 1);
            if (--remaining_inputs[reader] == 0) {
                result.order.push_back(reader);
            }
        }
    }

    if (result.order.size() != gates.size()) {
        std::vector<bool> placed(gates.size(), false);
        for (const NetId id : result.order) {
            placed[id] = true;
        }
        throw CycleError(find_cycle(gates, placed));
    }
    return result;
}

}  // namespace faultline
