#pragma once
// fjarr-desktop-1: the messages between fjarr-desktop-session and backend module E. One JSON object
// per SOCK_SEQPACKET datagram, with the descriptors it names attached (SCM_RIGHTS). Compiled into
// both ends; internal, and renamed with the module seam.
// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol · ADR-0028
#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include <sys/types.h>

#include <nlohmann/json.hpp>

namespace fjarr::desktop::proto {

inline constexpr const char* PROTOCOL = "fjarr-desktop-1";
inline constexpr std::size_t MAX_MESSAGE = 64 * 1024;
inline constexpr std::size_t MAX_FDS = 4;

/// A descriptor received with a message, closed when dropped unless released.
class Fd {
  public:
    Fd() = default;
    explicit Fd(int fd) : fd_(fd) {}
    Fd(Fd&& o) noexcept : fd_(o.release()) {}
    Fd& operator=(Fd&& o) noexcept;
    Fd(const Fd&) = delete;
    Fd& operator=(const Fd&) = delete;
    ~Fd();
    int get() const { return fd_; }
    int release() {
        const int f = fd_;
        fd_ = -1;
        return f;
    }
    explicit operator bool() const { return fd_ >= 0; }

  private:
    int fd_ = -1;
};

struct Message {
    nlohmann::json body; // an object with a string "type"
    std::vector<Fd> fds;
    std::string type() const { return body.value("type", std::string{}); }
};

/// One message, with `fds` attached (they stay open on this side). False with `error` set on failure.
bool send(int sock, const nlohmann::json& body, const std::vector<int>& fds = {}, std::string* error = nullptr);

enum class Received {
    Message,    // `out` holds it
    Closed,     // the peer hung up
    WouldBlock, // a non-blocking socket had nothing
    Invalid,    // a datagram that was not a JSON object with a type, or too many descriptors: dropped, its fds closed
    Error,      // the socket failed; `error` says how
};

/// The next message. Descriptors of a message that is not accepted are closed, never leaked.
Received receive(int sock, Message& out, std::string* error = nullptr);

/// Who is on the other end of a connected Unix socket (SO_PEERCRED).
std::optional<uid_t> peer_uid(int sock);

} // namespace fjarr::desktop::proto
