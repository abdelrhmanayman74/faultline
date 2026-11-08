#pragma once

#include <cstddef>
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

// Formats values as a string of '0'/'1' characters.
std::string to_bit_string(const std::vector<Logic>& values);

}  // namespace faultline
