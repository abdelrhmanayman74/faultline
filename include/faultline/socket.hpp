#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace faultline {

// Owns one file descriptor and closes it exactly once (RAII).
//
// Move-only: a copy would mean two owners and a double close(), and after the
// first close() the kernel may hand the same number to an unrelated new file.
// Nothing here is socket-specific, so it also owns the server's signalfd.
//
// Errors from the functions below are thrown as std::system_error carrying
// errno; what() reads like "connect: Connection refused".
class Socket {
public:
    Socket() = default;
    explicit Socket(int fd) noexcept : fd_(fd) {}
    ~Socket() { close(); }

    Socket(const Socket&) = delete;
    Socket& operator=(const Socket&) = delete;
    Socket(Socket&& other) noexcept;
    Socket& operator=(Socket&& other) noexcept;

    int fd() const { return fd_; }
    bool valid() const { return fd_ >= 0; }
    void close() noexcept;

private:
    int fd_ = -1;
};

// IPv4 TCP listening socket on `host` (dotted quad, e.g. "127.0.0.1"; "0.0.0.0"
// for every interface) and `port` (0 = let the kernel pick a free port, see
// local_port()). SO_REUSEADDR is set so a restarted server can bind the port
// again while old connections are still in TIME_WAIT. The socket is
// non-blocking: the server only calls accept after poll() said a connection is
// waiting, and if that connection vanished meanwhile accept must not block.
Socket listen_tcp(const std::string& host, std::uint16_t port, int backlog = 64);

// Port a socket is bound to (getsockname). Used after listen_tcp(host, 0).
std::uint16_t local_port(const Socket& socket);

// Accepts one connection on a listen_tcp() socket. The new socket is blocking.
// Returns an invalid Socket when no connection is ready after all (EAGAIN,
// EINTR, ECONNABORTED: the caller just polls again).
Socket accept_connection(const Socket& listener);

// Blocking client connection, used by the tests.
Socket connect_tcp(const std::string& host, std::uint16_t port);

// send() may write only part of the buffer (the kernel's send buffer is full)
// and may be interrupted by a signal (EINTR); this loops until every byte is
// sent. MSG_NOSIGNAL: writing to a connection the peer has closed returns
// EPIPE instead of raising SIGPIPE, whose default action kills the process.
void send_all(const Socket& socket, std::string_view data);

// recv() returns whatever has arrived, possibly 1 byte of a 4-byte header;
// this loops until `size` bytes are in `buffer`. Returns `size` on success. A
// smaller return value means the peer closed the connection cleanly (recv
// returned 0) after that many bytes; 0 means it closed before sending anything.
// Real errors (e.g. ECONNRESET) throw.
std::size_t recv_exact(const Socket& socket, char* buffer, std::size_t size);

}  // namespace faultline
