#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "faultline/netlist.hpp"
#include "faultline/vectors.hpp"

namespace faultline {

// 64 input vectors side by side in one machine word. Bit k of a net's word is
// the value of that net under vector k of the block ("lane" k). One bitwise
// AND of two words then evaluates an AND gate for 64 vectors at once, which is
// the whole speed-up of bit-parallel simulation.
using Word = std::uint64_t;
inline constexpr std::size_t kLanes = 64;

inline constexpr Word kAllOnes = ~Word{0};

// Value of one lane of a word, as a 0/1 Logic.
inline Logic lane_value(Word word, std::size_t lane) {
    return static_cast<Logic>((word >> lane) & 1U);
}

// Up to 64 consecutive input vectors, transposed: one word per primary input.
struct PatternBlock {
    std::size_t first_vector = 0;  // index of lane 0 in the full vector list
    std::size_t count = 0;         // valid lanes, 1..64 (only the last block has < 64)
    std::vector<Word> inputs;      // inputs[i] is the word of primary_inputs()[i]

    // Lanes [0, count) set. The unused lanes of a last, partial block hold
    // padding, and anything computed in them must be ignored: every result
    // is ANDed with this mask before it is reported.
    Word valid_mask() const { return count >= kLanes ? kAllOnes : (Word{1} << count) - 1; }
};

// Splits `vectors` into blocks of 64 (the last may be partial). Unused lanes
// are filled with 0. Throws std::invalid_argument if a vector does not have
// `num_inputs` bits.
std::vector<PatternBlock> pack_vectors(const std::vector<InputVector>& vectors,
                                       std::size_t num_inputs);

// Word version of evaluate_gate(): the same gate function on 64 lanes.
// Kept separate from the scalar code on purpose, so the scalar simulator stays
// an independent reference that the tests compare against.
Word evaluate_gate_word(const Gate& gate, const std::vector<Word>& values);

// Good-circuit simulation of one PatternBlock, one word per net.
// Keeps a reference to the netlist: the netlist must outlive the simulator.
class BitParallelSimulator {
public:
    explicit BitParallelSimulator(const Netlist& netlist);

    void simulate(const PatternBlock& block);

    // Values of every net (indexed by NetId) after the last simulate().
    const std::vector<Word>& values() const { return values_; }

    // Primary-output values of one lane after the last simulate(), in the
    // same format as ScalarSimulator::simulate() returns.
    std::vector<Logic> outputs(std::size_t lane) const;

private:
    const Netlist& netlist_;
    std::vector<Word> values_;
};

}  // namespace faultline
