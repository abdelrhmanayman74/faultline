#pragma once

#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace faultline {

// Why exceptions and not a result struct?
// A bad input file is rare and stops the whole job: nothing useful can be done
// with half a netlist. With an exception, the small helper functions of the
// parser can stop at the first error without passing an error code back
// through every return statement, and the CLI catches it in one place.
// A result struct would be the better choice on a hot path where failure is
// common and expected.

// Problem in an input file, with its location. what() is "file:line: message",
// the format compilers use, so editors can jump to the line.
class ParseError : public std::runtime_error {
public:
    ParseError(const std::string& file, int line, const std::string& message)
        : std::runtime_error(file + ":" + std::to_string(line) + ": " + message),
          file_(file),
          line_(line) {}

    const std::string& file() const { return file_; }
    int line() const { return line_; }

private:
    std::string file_;
    int line_;
};

// The netlist contains a combinational loop. cycle() lists the net names on
// the loop in signal-flow order; the last one drives the first one.
class CycleError : public std::runtime_error {
public:
    explicit CycleError(std::vector<std::string> cycle)
        : std::runtime_error("combinational cycle through nets: " + describe(cycle)),
          cycle_(std::move(cycle)) {}

    const std::vector<std::string>& cycle() const { return cycle_; }

private:
    static std::string describe(const std::vector<std::string>& cycle) {
        std::string text;
        for (const std::string& name : cycle) {
            text += name + " -> ";
        }
        return cycle.empty() ? text : text + cycle.front();
    }

    std::vector<std::string> cycle_;
};

}  // namespace faultline
