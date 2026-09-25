#pragma once
// spec: docs/08-protocol.md#errors — the stable error codes, as one exception type.
#include <stdexcept>
#include <string>
#include <string_view>

namespace fjarr {

class FjarrError : public std::runtime_error {
  public:
    FjarrError(std::string code, std::string message)
        : std::runtime_error(code + ": " + message), code_(std::move(code)),
          message_(std::move(message)) {}
    const std::string& code() const noexcept { return code_; }
    const std::string& message() const noexcept { return message_; }

  private:
    std::string code_;
    std::string message_;
};

namespace error_codes {
inline constexpr std::string_view auth_failed = "auth-failed";
inline constexpr std::string_view grant_expired = "grant-expired";
inline constexpr std::string_view capability_unknown = "capability-unknown";
inline constexpr std::string_view capability_denied = "capability-denied";
inline constexpr std::string_view session_unknown = "session-unknown";
inline constexpr std::string_view robot_offline = "robot-offline";
inline constexpr std::string_view rate_limited = "rate-limited";
inline constexpr std::string_view payload_invalid = "payload-invalid";
inline constexpr std::string_view internal = "internal";
/// The robot is not configured for it — a deployment choice, not a fault.
inline constexpr std::string_view unavailable = "unavailable";
/// The resource is already in use on this session (one pty per session).
inline constexpr std::string_view busy = "busy";
} // namespace error_codes

} // namespace fjarr
