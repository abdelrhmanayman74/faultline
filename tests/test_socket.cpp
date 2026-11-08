#include <gtest/gtest.h>

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <system_error>
#include <thread>
#include <utility>

#include "faultline/socket.hpp"

namespace faultline {
namespace {

// A connected pair of local stream sockets: same send/recv semantics as TCP
// (byte stream, partial reads), no network needed.
std::pair<Socket, Socket> socket_pair() {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        throw std::system_error(errno, std::generic_category(), "socketpair");
    }
    return {Socket(fds[0]), Socket(fds[1])};
}

bool fd_is_open(int fd) { return ::fcntl(fd, F_GETFD) != -1; }

TEST(Socket, ClosesInDestructorAndMoves) {
    auto [a, b] = socket_pair();
    const int fd = a.fd();
    {
        Socket moved(std::move(a));
        EXPECT_FALSE(a.valid());  // NOLINT: checking the moved-from state is the point
        EXPECT_EQ(moved.fd(), fd);
        EXPECT_TRUE(fd_is_open(fd));
    }
    EXPECT_FALSE(fd_is_open(fd));  // closed exactly once, by the new owner
}

TEST(Socket, MoveAssignmentClosesTheOldDescriptor) {
    auto [a, b] = socket_pair();
    const int old_fd = a.fd();
    a = std::move(b);
    EXPECT_FALSE(fd_is_open(old_fd));
    EXPECT_TRUE(a.valid());
    EXPECT_FALSE(b.valid());  // NOLINT
}

// 4 MiB is far more than a socket buffer holds, so send() returns early many
// times and the receiver sees many partial reads: both loops are exercised.
TEST(Socket, SendAllAndRecvExactMoveLargeBuffers) {
    auto [a, b] = socket_pair();
    std::string data(4 * 1024 * 1024, '\0');
    for (std::size_t i = 0; i < data.size(); ++i) {
        data[i] = static_cast<char>(i * 31 % 251);
    }
    std::thread sender([&a, &data] { send_all(a, data); });
    std::string received(data.size(), '\0');
    const std::size_t got = recv_exact(b, received.data(), received.size());
    sender.join();
    EXPECT_EQ(got, data.size());
    EXPECT_EQ(received, data);
}

TEST(Socket, RecvExactReportsEndOfStream) {
    auto [a, b] = socket_pair();
    send_all(a, "abc");
    a.close();
    char buffer[10];
    EXPECT_EQ(recv_exact(b, buffer, sizeof buffer), 3U);  // short count: peer closed after 3
    EXPECT_EQ(std::string(buffer, 3), "abc");
    EXPECT_EQ(recv_exact(b, buffer, sizeof buffer), 0U);  // closed, nothing more
}

// Without MSG_NOSIGNAL this would raise SIGPIPE and kill the test binary.
TEST(Socket, SendToClosedPeerThrowsInsteadOfSigpipe) {
    auto [a, b] = socket_pair();
    b.close();
    try {
        send_all(a, "hello");
        FAIL() << "send_all should have thrown";
    } catch (const std::system_error& error) {
        EXPECT_EQ(error.code().value(), EPIPE);
    }
}

TEST(Socket, TcpListenOnPortZeroConnectAndTalk) {
    const Socket listener = listen_tcp("127.0.0.1", 0);
    const std::uint16_t port = local_port(listener);
    EXPECT_NE(port, 0);

    const Socket client = connect_tcp("127.0.0.1", port);
    Socket server_side;
    while (!server_side.valid()) {  // the listener is non-blocking: retry until ready
        server_side = accept_connection(listener);
    }
    send_all(client, "ping");
    char buffer[4];
    ASSERT_EQ(recv_exact(server_side, buffer, 4), 4U);
    EXPECT_EQ(std::string(buffer, 4), "ping");
}

TEST(Socket, BadAddressIsRejected) {
    EXPECT_THROW(listen_tcp("localhost", 0), std::invalid_argument);  // IPv4 literals only
    EXPECT_THROW(listen_tcp("300.1.1.1", 0), std::invalid_argument);
}

}  // namespace
}  // namespace faultline
