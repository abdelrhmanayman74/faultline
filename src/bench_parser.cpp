#include "faultline/bench_parser.hpp"

#include <cctype>
#include <fstream>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "faultline/errors.hpp"

namespace faultline {

namespace {

// The parser works in two passes. Pass 1 turns each text line into a statement
// and only checks syntax. Pass 2 resolves names into indices. Two passes are
// needed because .bench allows using a net before the line that defines it.

struct Declaration {  // INPUT(x) or OUTPUT(x)
    std::string net;
    int line = 0;
};

struct GateStatement {  // net = TYPE(a, b, ...)
    std::string net;
    GateType type = GateType::Buf;
    std::vector<std::string> inputs;
    int line = 0;
};

struct BenchStatements {
    std::vector<Declaration> inputs;
    std::vector<Declaration> outputs;
    std::vector<GateStatement> gates;
};

std::string trim(const std::string& text) {
    const auto is_space = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && is_space(text[begin])) {
        ++begin;
    }
    while (end > begin && is_space(text[end - 1])) {
        --end;
    }
    return text.substr(begin, end - begin);
}

std::string to_upper(std::string text) {
    for (char& c : text) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return text;
}

class StatementReader {
public:
    explicit StatementReader(std::string source) : source_(std::move(source)) {}

    BenchStatements read(std::istream& in) {
        BenchStatements statements;
        std::string raw;
        int line = 0;
        while (std::getline(in, raw)) {
            ++line;
            // Drop the comment; trim() also removes the '\r' of CRLF files.
            const std::string text = trim(raw.substr(0, raw.find('#')));
            if (!text.empty()) {
                read_statement(text, line, statements);
            }
        }
        return statements;
    }

private:
    [[noreturn]] void fail(int line, const std::string& message) const {
        throw ParseError(source_, line, message);
    }

    void read_statement(const std::string& text, int line, BenchStatements& out) const {
        const std::size_t equals = text.find('=');
        if (equals == std::string::npos) {
            read_declaration(text, line, out);
        } else {
            out.gates.push_back(read_gate(text, equals, line));
        }
    }

    void read_declaration(const std::string& text, int line, BenchStatements& out) const {
        std::vector<std::string> args;
        const std::string keyword = to_upper(split_call(text, line, args));
        if ((keyword != "INPUT" && keyword != "OUTPUT") || args.size() != 1) {
            fail(line, "expected INPUT(net), OUTPUT(net) or 'net = GATE(inputs)', got '" + text + "'");
        }
        auto& list = keyword == "INPUT" ? out.inputs : out.outputs;
        list.push_back(Declaration{.net = args[0], .line = line});
    }

    GateStatement read_gate(const std::string& text, std::size_t equals, int line) const {
        GateStatement gate;
        gate.line = line;
        gate.net = trim(text.substr(0, equals));
        check_net_name(gate.net, line);

        const std::string keyword = to_upper(split_call(text.substr(equals + 1), line, gate.inputs));
        if (keyword == "DFF") {
            fail(line, "sequential circuits not supported yet (DFF '" + gate.net + "')");
        }
        const std::optional<GateType> type = gate_type_from_name(keyword);
        if (!type) {
            fail(line, "unknown gate type '" + keyword + "'");
        }
        gate.type = *type;

        const bool single_input = gate.type == GateType::Not || gate.type == GateType::Buf;
        if (single_input && gate.inputs.size() != 1) {
            fail(line, keyword + " gate '" + gate.net + "' needs exactly 1 input, got " +
                           std::to_string(gate.inputs.size()));
        }
        return gate;
    }

    // Splits "NAME(a, b, c)" into "NAME" (returned) and {"a", "b", "c"}.
    std::string split_call(const std::string& text, int line, std::vector<std::string>& args) const {
        const std::size_t open = text.find('(');
        const std::size_t close = text.rfind(')');
        if (open == std::string::npos || close == std::string::npos || close < open ||
            !trim(text.substr(close + 1)).empty()) {
            fail(line, "expected NAME(arguments), got '" + trim(text) + "'");
        }
        const std::string inside = text.substr(open + 1, close - open - 1);
        std::size_t start = 0;
        while (true) {
            const std::size_t comma = inside.find(',', start);
            args.push_back(trim(inside.substr(start, comma - start)));
            check_net_name(args.back(), line);
            if (comma == std::string::npos) {
                break;
            }
            start = comma + 1;
        }
        return trim(text.substr(0, open));
    }

    void check_net_name(const std::string& name, int line) const {
        if (name.empty()) {
            fail(line, "missing net name");
        }
        for (const char c : name) {
            if (std::isspace(static_cast<unsigned char>(c)) || c == '(' || c == ')' || c == ',' ||
                c == '=') {
                fail(line, "invalid net name '" + name + "'");
            }
        }
    }

    std::string source_;
};

// Pass 2: name resolution. Errors point at the line that caused them.
class NetlistBuilder {
public:
    explicit NetlistBuilder(std::string source) : source_(std::move(source)) {}

    Netlist build(const BenchStatements& statements) {
        for (const Declaration& input : statements.inputs) {
            define(input.net, GateType::Input, input.line);
        }
        for (const GateStatement& gate : statements.gates) {
            define(gate.net, gate.type, gate.line);
        }
        for (const GateStatement& gate : statements.gates) {
            connect(gate);
        }
        add_outputs(statements.outputs);
        finalize_or_report_cycle();
        return std::move(netlist_);
    }

private:
    [[noreturn]] void fail(int line, const std::string& message) const {
        throw ParseError(source_, line, message);
    }

    void define(const std::string& net, GateType type, int line) {
        const auto [it, inserted] = defined_at_line_.emplace(net, line);
        if (!inserted) {
            fail(line, "net '" + net + "' is defined twice (first definition at line " +
                           std::to_string(it->second) + ")");
        }
        netlist_.add_gate(net, type);
    }

    void connect(const GateStatement& gate) {
        std::vector<NetId> fanin;
        fanin.reserve(gate.inputs.size());
        for (const std::string& input : gate.inputs) {
            const std::optional<NetId> id = netlist_.find(input);
            if (!id) {
                fail(gate.line, "undefined net '" + input + "' used by gate '" + gate.net + "'");
            }
            fanin.push_back(*id);
        }
        netlist_.set_fanin(*netlist_.find(gate.net), std::move(fanin));
    }

    void add_outputs(const std::vector<Declaration>& outputs) {
        std::unordered_set<std::string> seen;
        for (const Declaration& output : outputs) {
            const std::optional<NetId> id = netlist_.find(output.net);
            if (!id) {
                fail(output.line, "output '" + output.net + "' is never driven");
            }
            if (!seen.insert(output.net).second) {
                fail(output.line, "output '" + output.net + "' is declared twice");
            }
            netlist_.mark_output(*id);
        }
    }

    void finalize_or_report_cycle() {
        try {
            netlist_.finalize();
        } catch (const CycleError& error) {
            // Point at the line that defines the first net of the loop.
            fail(defined_at_line_.at(error.cycle().front()), error.what());
        }
    }

    std::string source_;
    Netlist netlist_;
    std::unordered_map<std::string, int> defined_at_line_;
};

}  // namespace

Netlist parse_bench(std::istream& in, const std::string& source_name) {
    const BenchStatements statements = StatementReader(source_name).read(in);
    return NetlistBuilder(source_name).build(statements);
}

Netlist parse_bench_file(const std::string& path) {
    std::ifstream file(path);
    if (!file) {
        throw ParseError(path, 0, "cannot open file");
    }
    return parse_bench(file, path);
}

}  // namespace faultline
