// fjarr-desktop-1's transport (docs/23#desktop-helper-protocol): one JSON object per SOCK_SEQPACKET
// datagram, descriptors attached to the message that names them, and never a descriptor leaked on a
// message that is refused.
#include <cstring>
#include <filesystem>

#include <sys/socket.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "desktop/helper_protocol.hpp"

using namespace fjarr::desktop;

namespace {
struct Pair {
    int a = -1, b = -1;
    Pair() { EXPECT_EQ(::socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, &a), 0); }
    ~Pair() {
        if (a >= 0) ::close(a);
        if (b >= 0) ::close(b);
    }
};
std::size_t open_fds() {
    std::size_t n = 0;
    for ([[maybe_unused]] const auto& e : std::filesystem::directory_iterator("/proc/self/fd")) n++;
    return n;
}
} // namespace

TEST(HelperProtocol, aMessageAndItsDescriptorsArriveTogether) {
    Pair p;
    int pipe_fds[2];
    ASSERT_EQ(::pipe(pipe_fds), 0);
    ASSERT_TRUE(proto::send(p.a, {{"type", "capture-started"}, {"id", 7}, {"node", 42}}, {pipe_fds[1]}));
    ::close(pipe_fds[1]);
    proto::Message m;
    ASSERT_EQ(proto::receive(p.b, m), proto::Received::Message);
    EXPECT_EQ(m.type(), "capture-started");
    EXPECT_EQ(m.body["node"], 42);
    ASSERT_EQ(m.fds.size(), 1u);
    // The received descriptor is the pipe's write end: what goes in comes out of the read end.
    ASSERT_EQ(::write(m.fds[0].get(), "x", 1), 1);
    char c = 0;
    ASSERT_EQ(::read(pipe_fds[0], &c, 1), 1);
    EXPECT_EQ(c, 'x');
    ::close(pipe_fds[0]);
}

TEST(HelperProtocol, messagesKeepTheirBoundaries) {
    Pair p;
    ASSERT_TRUE(proto::send(p.a, {{"type", "hello"}, {"protocol", proto::PROTOCOL}}));
    ASSERT_TRUE(proto::send(p.a, {{"type", "monitors"}, {"monitors", nlohmann::json::array()}}));
    proto::Message m;
    ASSERT_EQ(proto::receive(p.b, m), proto::Received::Message);
    EXPECT_EQ(m.type(), "hello");
    ASSERT_EQ(proto::receive(p.b, m), proto::Received::Message);
    EXPECT_EQ(m.type(), "monitors");
}

TEST(HelperProtocol, whatIsNotAMessageIsRefusedAndItsDescriptorsClosed) {
    Pair p;
    std::string err;
    EXPECT_FALSE(proto::send(p.a, nlohmann::json{{"no-type", 1}}, {}, &err));
    EXPECT_FALSE(proto::send(p.a, {{"type", std::string(proto::MAX_MESSAGE, 'x')}}, {}, &err));
    EXPECT_NE(err.find("larger"), std::string::npos) << err;

    // A peer that sends garbage with descriptors attached: refused, and nothing leaks.
    const std::size_t before = open_fds();
    int pipe_fds[2];
    ASSERT_EQ(::pipe(pipe_fds), 0);
    const char junk[] = "not json";
    iovec iov{const_cast<char*>(junk), sizeof junk - 1};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 2)] = {};
    msg.msg_control = control;
    msg.msg_controllen = sizeof control;
    cmsghdr* cm = CMSG_FIRSTHDR(&msg);
    cm->cmsg_level = SOL_SOCKET;
    cm->cmsg_type = SCM_RIGHTS;
    cm->cmsg_len = CMSG_LEN(sizeof(int) * 2);
    std::memcpy(CMSG_DATA(cm), pipe_fds, sizeof pipe_fds);
    ASSERT_GT(::sendmsg(p.a, &msg, 0), 0);
    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
    proto::Message m;
    EXPECT_EQ(proto::receive(p.b, m, &err), proto::Received::Invalid);
    EXPECT_EQ(open_fds(), before) << "a refused message's descriptors were left open";
}

TEST(HelperProtocol, aHangUpIsClosedAndThePeerHasAUid) {
    Pair p;
    EXPECT_EQ(proto::peer_uid(p.b), std::optional<uid_t>(::getuid()));
    ::close(p.a);
    p.a = -1;
    proto::Message m;
    EXPECT_EQ(proto::receive(p.b, m), proto::Received::Closed);
}
