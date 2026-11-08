#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <string>
#include <vector>

#include "faultline/scalar_simulator.hpp"

namespace faultline {

using InputVector = std::vector<Logic>;

// Reads test vectors: one per line, '0'/'1' characters in primary-input order.
// '#' starts a comment; spaces/tabs are ignored and blank lines skipped.
// Every vector must have exactly `num_inputs` bits. Throws ParseError with the
// line number otherwise.
std::vector<InputVector> read_vectors(std::istream& in, const std::string& source_name,
                                      std::size_t num_inputs);
std::vector<InputVector> read_vectors_file(const std::string& path, std::size_t num_inputs);

// `count` pseudo-random vectors of `num_inputs` bits from a seeded
// std::mt19937_64. That engine is fully specified by the C++ standard, so a
// seed gives the same vectors with every compiler and platform.
std::vector<InputVector> random_vectors(std::size_t count, std::size_t num_inputs,
                                        std::uint64_t seed);

// Formats values as a string of '0'/'1' characters.
std::string to_bit_string(const std::vector<Logic>& values);

}  // namespace faultline
