#pragma once

#include <istream>
#include <string>

#include "faultline/netlist.hpp"

namespace faultline {

// Reads an ISCAS .bench netlist. Throws ParseError ("file:line: message") on
// the first problem. The returned netlist is already finalize()d.
Netlist parse_bench_file(const std::string& path);

// Same, from any stream. `source_name` is only used in error messages, which
// lets tests parse strings and still check the reported line numbers.
Netlist parse_bench(std::istream& in, const std::string& source_name);

}  // namespace faultline
