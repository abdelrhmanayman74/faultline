#include "faultline/protocol.hpp"

#include <arpa/inet.h>

#include <algorithm>
#include <charconv>
#include <cstring>

#include "faultline/errors.hpp"

namespace faultline {

namespace {

constexpr std::size_t kHeaderSize = 4;

struct CommandShape {
    std::string_view name;
    std::size_t min_args;
    std::size_t max_args;
};

constexpr CommandShape kCommands[] = {
    {"LOAD", 1, 1},     {"SIM", 1, 1},    {"FAULTSIM", 1, 5}, {"STATUS", 0, 1},
    {"RESULT", 1, 1},   {"CANCEL", 1, 1}, {"QUIT", 0, 0},
};

std::uint64_t parse_number(const std::string& text, const std::string& what) {
    std::uint64_t value = 0;
    const char* end = text.data() + text.size();
    const auto [ptr, error] = std::from_chars(text.data(), end, value);
    if (text.empty() || error != std::errc() || ptr != end) {
        throw ProtocolError(what + " must be a non-negative integer, got '" + text + "'");
    }
    return value;
}

std::size_t parse_in_range(const std::string& text, const std::string& key, std::size_t low,
                           std::size_t high) {
    const std::uint64_t value = parse_number(text, key);
    if (value < low || value > high) {
        throw ProtocolError(key + " must be between " + std::to_string(low) + " and " +
                            std::to_string(high));
    }
    return static_cast<std::size_t>(value);
}

}  // namespace

std::string encode_frame(std::string_view payload) {
    if (payload.size() > kMaxFrameSize) {
        throw ProtocolError("frame of " + std::to_string(payload.size()) +
                            " bytes is over the limit of " + std::to_string(kMaxFrameSize));
    }
    // htonl: host byte order (little-endian on x86) to network order (big-endian).
    const std::uint32_t length = htonl(static_cast<std::uint32_t>(payload.size()));
    std::string frame(kHeaderSize, '\0');
    std::memcpy(frame.data(), &length, kHeaderSize);
    frame.append(payload);
    return frame;
}

void write_frame(const Socket& socket, std::string_view payload) {
    // One buffer and one send_all: header and payload usually leave in a single
    // TCP segment instead of a tiny 4-byte one followed by the rest.
    send_all(socket, encode_frame(payload));
}

std::optional<std::string> read_frame(const Socket& socket) {
    char header[kHeaderSize];
    const std::size_t got = recv_exact(socket, header, kHeaderSize);
    if (got == 0) {
        return std::nullopt;  // closed between frames: a normal end of the conversation
    }
    if (got < kHeaderSize) {
        throw ProtocolError("connection closed in the middle of a frame header");
    }
    std::uint32_t network_length = 0;
    std::memcpy(&network_length, header, kHeaderSize);  // memcpy: header may be unaligned
    const std::size_t length = ntohl(network_length);
    // Checked before allocating anything.
    if (length > kMaxFrameSize) {
        throw ProtocolError("frame of " + std::to_string(length) + " bytes is over the limit of " +
                            std::to_string(kMaxFrameSize));
    }
    std::string payload(length, '\0');
    if (recv_exact(socket, payload.data(), length) < length) {
        throw ProtocolError("connection closed in the middle of a frame");
    }
    return payload;
}

Request parse_request(std::string_view payload) {
    Request request;
    const std::size_t newline = payload.find('\n');
    std::string_view first_line = payload.substr(0, newline);
    if (newline != std::string_view::npos) {
        request.body = std::string(payload.substr(newline + 1));
    }
    if (!first_line.empty() && first_line.back() == '\r') {
        first_line.remove_suffix(1);  // tolerate CRLF from hand-typed requests
    }

    std::vector<std::string> words;
    std::size_t pos = 0;
    while (pos < first_line.size()) {
        const std::size_t end = std::min(first_line.find(' ', pos), first_line.size());
        if (end > pos) {
            words.emplace_back(first_line.substr(pos, end - pos));
        }
        pos = end + 1;
    }
    if (words.empty()) {
        throw ProtocolError("empty request");
    }
    request.command = words.front();
    request.args.assign(words.begin() + 1, words.end());

    for (const CommandShape& shape : kCommands) {
        if (shape.name == request.command) {
            if (request.args.size() < shape.min_args || request.args.size() > shape.max_args) {
                throw ProtocolError("wrong number of arguments for " + request.command);
            }
            return request;
        }
    }
    throw ProtocolError("unknown command '" + request.command + "'");
}

FaultsimArgs parse_faultsim_args(const Request& request) {
    FaultsimArgs result;
    result.circuit = request.args.at(0);  // parse_request guarantees at least one argument
    std::vector<std::string> seen;
    for (std::size_t i = 1; i < request.args.size(); ++i) {
        const std::string& arg = request.args[i];
        const std::size_t equals = arg.find('=');
        if (equals == std::string::npos) {
            throw ProtocolError("expected key=value, got '" + arg + "'");
        }
        const std::string key = arg.substr(0, equals);
        const std::string value = arg.substr(equals + 1);
        for (const std::string& previous : seen) {
            if (previous == key) {
                throw ProtocolError(key + " given twice");
            }
        }
        seen.push_back(key);
        if (key == "random") {
            result.random_count = parse_in_range(value, key, 1, kMaxRandomVectors);
        } else if (key == "seed") {
            result.seed = parse_number(value, key);
        } else if (key == "threads") {
            result.threads = parse_in_range(value, key, 1, kMaxJobThreads);
        } else if (key == "details") {
            result.details = parse_in_range(value, key, 0, 1) == 1;
        } else {
            throw ProtocolError("unknown FAULTSIM option '" + key + "'");
        }
    }
    const bool has_body = request.body.find_first_not_of(" \t\r\n") != std::string::npos;
    if ((result.random_count > 0) == has_body) {
        throw ProtocolError("FAULTSIM needs exactly one of random=N or vector lines in the body");
    }
    return result;
}

std::uint64_t parse_job_id(const std::string& text) { return parse_number(text, "job id"); }

}  // namespace faultline
