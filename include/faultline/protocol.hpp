#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "faultline/socket.hpp"

namespace faultline {

// Wire format (docs/PROTOCOL.md): every message is a frame
//     [4-byte payload length, big-endian][payload bytes]
// TCP is a byte stream with no message boundaries: one send() can arrive as
// several recv()s and two sends as one. The length prefix tells the reader
// exactly how many bytes belong to the message.
//
// The limit stops a peer from making us allocate gigabytes by sending a huge
// length. A frame over it is a protocol violation: the stream cannot be
// resynchronized, so the connection is closed.
inline constexpr std::size_t kMaxFrameSize = 16 * 1024 * 1024;  // 16 MiB

// Header + payload. Throws ProtocolError if the payload is over the limit.
std::string encode_frame(std::string_view payload);

// encode_frame() + send_all().
void write_frame(const Socket& socket, std::string_view payload);

// Reads one frame. std::nullopt: the peer closed the connection cleanly
// between frames. Throws ProtocolError for a length over the limit (without
// reading the payload) and for a connection closed in the middle of a frame.
std::optional<std::string> read_frame(const Socket& socket);

// A request payload split up. The first line holds the command and its
// space-separated arguments; everything after the first '\n' is the body
// (netlist text or vector lines).
struct Request {
    std::string command;            // "LOAD", "SIM", ... (case-sensitive)
    std::vector<std::string> args;
    std::string body;
};

// Checks that the command exists and has an allowed number of arguments.
// Throws ProtocolError otherwise.
Request parse_request(std::string_view payload);

// FAULTSIM <name> [random=N] [seed=S] [threads=T] [details=0|1]
struct FaultsimArgs {
    std::string circuit;
    std::size_t random_count = 0;  // 0 = vectors come from the body
    std::uint64_t seed = 1;        // same default as the CLI
    std::size_t threads = 1;
    bool details = false;          // add the per-fault D/U lines to the result
};

// Limits so one request cannot exhaust the server's memory or threads.
inline constexpr std::size_t kMaxRandomVectors = 1'000'000;
inline constexpr std::size_t kMaxJobThreads = 64;

// Parses and range-checks the key=value arguments of a FAULTSIM request
// (also: random= and a vector body are mutually exclusive, one is required).
FaultsimArgs parse_faultsim_args(const Request& request);

// Decimal job id; throws ProtocolError for anything else.
std::uint64_t parse_job_id(const std::string& text);

}  // namespace faultline
