#include "faultline/vectors.hpp"

#include <fstream>

#include "faultline/errors.hpp"

namespace faultline {

namespace {

std::string strip_comment_and_spaces(const std::string& raw) {
    std::string bits;
    for (const char c : raw) {
        if (c == '#') {
            break;
        }
        if (c != ' ' && c != '\t' && c != '\r') {
            bits.push_back(c);
        }
    }
    return bits;
}

}  // namespace

std::vector<InputVector> read_vectors(std::istream& in, const std::string& source_name,
                                      std::size_t num_inputs) {
    std::vector<InputVector> vectors;
    std::string raw;
    int line = 0;
    while (std::getline(in, raw)) {
        ++line;
        const std::string bits = strip_comment_and_spaces(raw);
        if (bits.empty()) {
            continue;
        }
        if (bits.size() != num_inputs) {
            throw ParseError(source_name, line,
                             "vector has " + std::to_string(bits.size()) + " bits, circuit has " +
                                 std::to_string(num_inputs) + " primary inputs");
        }
        InputVector vector;
        vector.reserve(bits.size());
        for (const char c : bits) {
            if (c != '0' && c != '1') {
                throw ParseError(source_name, line,
                                 std::string("invalid character '") + c + "' (only 0 and 1 allowed)");
            }
            vector.push_back(c == '1' ? 1 : 0);
        }
        vectors.push_back(std::move(vector));
    }
    return vectors;
}

std::vector<InputVector> read_vectors_file(const std::string& path, std::size_t num_inputs) {
    std::ifstream file(path);
    if (!file) {
        throw ParseError(path, 0, "cannot open file");
    }
    return read_vectors(file, path, num_inputs);
}

std::string to_bit_string(const std::vector<Logic>& values) {
    std::string text;
    text.reserve(values.size());
    for (const Logic value : values) {
        text.push_back(value ? '1' : '0');
    }
    return text;
}

}  // namespace faultline
