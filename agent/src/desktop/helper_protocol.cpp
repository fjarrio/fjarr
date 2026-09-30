#include "desktop/helper_protocol.hpp"

#include <cerrno>
#include <cstring>

#include <sys/socket.h>
#include <unistd.h>

namespace fjarr::desktop::proto {

Fd& Fd::operator=(Fd&& o) noexcept {
    if (this != &o) {
        if (fd_ >= 0) ::close(fd_);
        fd_ = o.release();
    }
    return *this;
}

Fd::~Fd() {
    if (fd_ >= 0) ::close(fd_);
}

namespace {
void set_error(std::string* error, const std::string& what) {
    if (error) *error = what;
}
} // namespace

bool send(int sock, const nlohmann::json& body, const std::vector<int>& fds, std::string* error) {
    if (!body.is_object() || !body.contains("type") || !body["type"].is_string()) {
        set_error(error, "a message is a JSON object with a string type");
        return false;
    }
    if (fds.size() > MAX_FDS) {
        set_error(error, "more than " + std::to_string(MAX_FDS) + " descriptors");
        return false;
    }
    const std::string text = body.dump();
    if (text.size() > MAX_MESSAGE) {
        set_error(error, "message larger than " + std::to_string(MAX_MESSAGE) + " bytes");
        return false;
    }
    iovec iov{const_cast<char*>(text.data()), text.size()};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * MAX_FDS)];
    if (!fds.empty()) {
        std::memset(control, 0, sizeof control);
        msg.msg_control = control;
        msg.msg_controllen = CMSG_SPACE(sizeof(int) * fds.size());
        cmsghdr* c = CMSG_FIRSTHDR(&msg);
        c->cmsg_level = SOL_SOCKET;
        c->cmsg_type = SCM_RIGHTS;
        c->cmsg_len = CMSG_LEN(sizeof(int) * fds.size());
        std::memcpy(CMSG_DATA(c), fds.data(), sizeof(int) * fds.size());
    }
    ssize_t n;
    do n = ::sendmsg(sock, &msg, MSG_NOSIGNAL);
    while (n < 0 && errno == EINTR);
    if (n < 0) {
        set_error(error, std::string("sendmsg: ") + std::strerror(errno));
        return false;
    }
    return true;
}

Received receive(int sock, Message& out, std::string* error) {
    out = Message{};
    std::string buf(MAX_MESSAGE + 1, '\0'); // +1: a datagram that fills it was truncated
    iovec iov{buf.data(), buf.size()};
    msghdr msg{};
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * (MAX_FDS + 4))];
    msg.msg_control = control;
    msg.msg_controllen = sizeof control;
    ssize_t n;
    do n = ::recvmsg(sock, &msg, MSG_CMSG_CLOEXEC);
    while (n < 0 && errno == EINTR);
    if (n < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) return Received::WouldBlock;
        set_error(error, std::string("recvmsg: ") + std::strerror(errno));
        return Received::Error;
    }
    // Take ownership of every descriptor first, so each is closed whatever happens next.
    std::vector<Fd> fds;
    for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        const std::size_t count = (c->cmsg_len - CMSG_LEN(0)) / sizeof(int);
        for (std::size_t i = 0; i < count; i++) {
            int fd;
            std::memcpy(&fd, CMSG_DATA(c) + i * sizeof(int), sizeof(int));
            fds.emplace_back(fd);
        }
    }
    if (n == 0 && fds.empty()) return Received::Closed;
    if ((msg.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || static_cast<std::size_t>(n) > MAX_MESSAGE || fds.size() > MAX_FDS) {
        set_error(error, "a datagram too large, or with too many descriptors");
        return Received::Invalid;
    }
    auto body = nlohmann::json::parse(buf.data(), buf.data() + n, nullptr, false);
    if (body.is_discarded() || !body.is_object() || !body.contains("type") || !body["type"].is_string()) {
        set_error(error, "not a JSON object with a string type");
        return Received::Invalid;
    }
    out.body = std::move(body);
    out.fds = std::move(fds);
    return Received::Message;
}

std::optional<uid_t> peer_uid(int sock) {
    ucred cred{};
    socklen_t len = sizeof cred;
    if (::getsockopt(sock, SOL_SOCKET, SO_PEERCRED, &cred, &len) != 0) return std::nullopt;
    return cred.uid;
}

} // namespace fjarr::desktop::proto
