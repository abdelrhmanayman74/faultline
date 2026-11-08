#include "faultline/socket.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <stdexcept>
#include <system_error>
#include <utility>

namespace faultline {

namespace {

// std::system_error builds its message with a thread-safe call, unlike
// strerror(), which may return a shared static buffer.
[[noreturn]] void throw_errno(const char* what) {
    throw std::system_error(errno, std::generic_category(), what);
}

sockaddr_in ipv4_address(const std::string& host, std::uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);  // the network uses big-endian byte order
    if (::inet_pton(AF_INET, host.c_str(), &address.sin_addr) != 1) {
        throw std::invalid_argument("not an IPv4 address: '" + host + "'");
    }
    return address;
}

}  // namespace

Socket::Socket(Socket&& other) noexcept : fd_(std::exchange(other.fd_, -1)) {}

Socket& Socket::operator=(Socket&& other) noexcept {
    if (this != &other) {
        close();
        fd_ = std::exchange(other.fd_, -1);
    }
    return *this;
}

void Socket::close() noexcept {
    if (fd_ >= 0) {
        // Not retried on EINTR: on Linux the descriptor is released even then,
        // and a retry could close a number another thread has just been given.
        ::close(fd_);
        fd_ = -1;
    }
}

Socket listen_tcp(const std::string& host, std::uint16_t port, int backlog) {
    const sockaddr_in address = ipv4_address(host, port);
    // SOCK_CLOEXEC: the descriptor is not inherited by programs we might exec.
    Socket listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0));
    if (!listener.valid()) {
        throw_errno("socket");
    }
    const int on = 1;
    if (::setsockopt(listener.fd(), SOL_SOCKET, SO_REUSEADDR, &on, sizeof on) != 0) {
        throw_errno("setsockopt(SO_REUSEADDR)");
    }
    // The sockets API takes a generic sockaddr*; this cast is how it is meant to be used.
    if (::bind(listener.fd(), reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        throw_errno("bind");
    }
    if (::listen(listener.fd(), backlog) != 0) {
        throw_errno("listen");
    }
    return listener;
}

std::uint16_t local_port(const Socket& socket) {
    sockaddr_in address{};
    socklen_t length = sizeof address;
    if (::getsockname(socket.fd(), reinterpret_cast<sockaddr*>(&address), &length) != 0) {
        throw_errno("getsockname");
    }
    return ntohs(address.sin_port);
}

Socket accept_connection(const Socket& listener) {
    // accept4 without SOCK_NONBLOCK: the new socket is blocking even though
    // the listener is not, so connection threads can simply wait in recv().
    const int fd = ::accept4(listener.fd(), nullptr, nullptr, SOCK_CLOEXEC);
    if (fd >= 0) {
        return Socket(fd);
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR || errno == ECONNABORTED) {
        return Socket();
    }
    throw_errno("accept");
}

Socket connect_tcp(const std::string& host, std::uint16_t port) {
    const sockaddr_in address = ipv4_address(host, port);
    Socket socket(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!socket.valid()) {
        throw_errno("socket");
    }
    if (::connect(socket.fd(), reinterpret_cast<const sockaddr*>(&address), sizeof address) != 0) {
        throw_errno("connect");
    }
    return socket;
}

void send_all(const Socket& socket, std::string_view data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t n = ::send(socket.fd(), data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR) {
                continue;  // interrupted before sending anything: just try again
            }
            throw_errno("send");
        }
        sent += static_cast<std::size_t>(n);  // n may be less than asked: loop for the rest
    }
}

std::size_t recv_exact(const Socket& socket, char* buffer, std::size_t size) {
    std::size_t received = 0;
    while (received < size) {
        const ssize_t n = ::recv(socket.fd(), buffer + received, size - received, 0);
        if (n == 0) {
            return received;  // orderly shutdown by the peer
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw_errno("recv");
        }
        received += static_cast<std::size_t>(n);
    }
    return received;
}

}  // namespace faultline
