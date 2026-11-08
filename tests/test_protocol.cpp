#include <gtest/gtest.h>

#include <sys/socket.h>

#include <cerrno>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "faultline/errors.hpp"
#include "faultline/protocol.hpp"
#include "faultline/socket.hpp"

namespace faultline {
namespace {

std::pair<Socket, Socket> socket_pair() {
    int fds[2];
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        throw std::system_error(errno, std::generic_category(), "socketpair");
    }
    return {Socket(fds[0]), Socket(fds[1])};
}

std::string header(unsigned char b0, unsigned char b1, unsigned char b2, unsigned char b3) {
    return {static_cast<char>(b0), static_cast<char>(b1), static_cast<char>(b2),
            static_cast<char>(b3)};
}

// ---- framing ----

TEST(Framing, HeaderIsBigEndianLength) {
    const std::string payload(258, 'x');  // 258 = 0x00000102
    const std::string frame = encode_frame(payload);
    ASSERT_EQ(frame.size(), 4 + payload.size());
    EXPECT_EQ(frame.substr(0, 4), header(0, 0, 1, 2));
    EXPECT_EQ(frame.substr(4), payload);
    EXPECT_EQ(encode_frame(""), header(0, 0, 0, 0));
}

TEST(Framing, RoundTripSeveralFramesOnOneStream) {
    auto [a, b] = socket_pair();
    write_frame(a, "STATUS");
    write_frame(a, "");
    write_frame(a, "LOAD x\nINPUT(a)\n");
    a.close();
    EXPECT_EQ(read_frame(b), "STATUS");
    EXPECT_EQ(read_frame(b), "");
    EXPECT_EQ(read_frame(b), "LOAD x\nINPUT(a)\n");
    EXPECT_EQ(read_frame(b), std::nullopt);  // clean close between frames
}

TEST(Framing, FrameSplitIntoSingleBytesIsReassembled) {
    auto [a, b] = socket_pair();
    const std::string frame = encode_frame("SIM c17\n10101\n");
    for (const char byte : frame) {
        send_all(a, std::string(1, byte));
    }
    EXPECT_EQ(read_frame(b), "SIM c17\n10101\n");
}

TEST(Framing, OversizePayloadIsNotEncoded) {
    EXPECT_NO_THROW(encode_frame(std::string(kMaxFrameSize, 'a')));
    EXPECT_THROW(encode_frame(std::string(kMaxFrameSize + 1, 'a')), ProtocolError);
}

TEST(Framing, OversizeLengthIsRejectedBeforeReadingPayload) {
    auto [a, b] = socket_pair();
    const std::size_t length = kMaxFrameSize + 1;  // 0x01000001
    send_all(a, header(static_cast<unsigned char>(length >> 24), 0, 0, 1));
    EXPECT_THROW(read_frame(b), ProtocolError);
}

TEST(Framing, TruncatedHeaderIsAnError) {
    auto [a, b] = socket_pair();
    send_all(a, std::string(2, '\0'));
    a.close();
    EXPECT_THROW(read_frame(b), ProtocolError);
}

TEST(Framing, TruncatedPayloadIsAnError) {
    auto [a, b] = socket_pair();
    send_all(a, header(0, 0, 0, 10) + "only5");
    a.close();
    EXPECT_THROW(read_frame(b), ProtocolError);
}

// ---- request parsing ----

TEST(ParseRequest, SplitsCommandArgumentsAndBody) {
    const Request request = parse_request("LOAD  c17\nINPUT(1)\nOUTPUT(1)\n");
    EXPECT_EQ(request.command, "LOAD");
    ASSERT_EQ(request.args.size(), 1U);
    EXPECT_EQ(request.args[0], "c17");
    EXPECT_EQ(request.body, "INPUT(1)\nOUTPUT(1)\n");

    const Request status = parse_request("STATUS\r");
    EXPECT_EQ(status.command, "STATUS");
    EXPECT_TRUE(status.args.empty());
    EXPECT_TRUE(status.body.empty());
}

TEST(ParseRequest, RejectsBadRequests) {
    EXPECT_THROW(parse_request(""), ProtocolError);
    EXPECT_THROW(parse_request("   \nbody"), ProtocolError);
    EXPECT_THROW(parse_request("HELLO"), ProtocolError);
    EXPECT_THROW(parse_request("status"), ProtocolError);  // commands are case-sensitive
    EXPECT_THROW(parse_request("LOAD"), ProtocolError);    // missing name
    EXPECT_THROW(parse_request("LOAD a b"), ProtocolError);
    EXPECT_THROW(parse_request("QUIT now"), ProtocolError);
    EXPECT_THROW(parse_request("CANCEL"), ProtocolError);
}

TEST(ParseRequest, UnknownCommandMessageNamesIt) {
    try {
        parse_request("FROB x");
        FAIL() << "expected ProtocolError";
    } catch (const ProtocolError& error) {
        EXPECT_STREQ(error.what(), "unknown command 'FROB'");
    }
}

TEST(FaultsimArgs, ParsesOptionsAndDefaults) {
    const FaultsimArgs args =
        parse_faultsim_args(parse_request("FAULTSIM c432 random=1000 threads=4 details=1"));
    EXPECT_EQ(args.circuit, "c432");
    EXPECT_EQ(args.random_count, 1000U);
    EXPECT_EQ(args.seed, 1U);
    EXPECT_EQ(args.threads, 4U);
    EXPECT_TRUE(args.details);

    const FaultsimArgs with_body = parse_faultsim_args(parse_request("FAULTSIM c17\n10101\n"));
    EXPECT_EQ(with_body.random_count, 0U);
    EXPECT_EQ(with_body.threads, 1U);
    EXPECT_FALSE(with_body.details);
}

TEST(FaultsimArgs, RejectsBadOptions) {
    const char* bad[] = {
        "FAULTSIM c17 random=abc",      "FAULTSIM c17 random=-1",
        "FAULTSIM c17 random=0",        "FAULTSIM c17 random=10 random=20",
        "FAULTSIM c17 random=10 colour=red", "FAULTSIM c17 random=10 threads=0",
        "FAULTSIM c17 random=10 threads=65", "FAULTSIM c17 random=10 details=2",
        "FAULTSIM c17 random",          "FAULTSIM c17",  // neither random= nor a body
        "FAULTSIM c17 random=5\n10101\n",                // both
        "FAULTSIM c17 random=99999999999999999999",      // does not fit 64 bits
    };
    for (const char* payload : bad) {
        EXPECT_THROW(parse_faultsim_args(parse_request(payload)), ProtocolError) << payload;
    }
}

TEST(ParseJobId, AcceptsOnlyDecimalNumbers) {
    EXPECT_EQ(parse_job_id("42"), 42U);
    EXPECT_THROW(parse_job_id(""), ProtocolError);
    EXPECT_THROW(parse_job_id("4x"), ProtocolError);
    EXPECT_THROW(parse_job_id("-1"), ProtocolError);
}

}  // namespace
}  // namespace faultline
