#pragma once
// Flap-resistant reconnect backoff — pure functions, shared with tests.
// spec: docs/08-protocol.md#reconnection (0.5 s × 2, cap 30 s, ±20 %, reset after 30 s stable)
#include <chrono>
#include <cstdint>
#include <functional>

namespace fjarr {

struct BackoffPolicy {
    std::chrono::milliseconds initial{500};
    std::chrono::milliseconds max{30000};
    double factor = 2.0;
    double jitter = 0.2;
    std::chrono::milliseconds stable_reset{30000};
};

class Backoff {
  public:
    explicit Backoff(BackoffPolicy policy = {}, std::function<double()> random = nullptr);
    /// The delay before the next attempt (attempt counter advances).
    std::chrono::milliseconds next();
    void mark_connected(std::chrono::steady_clock::time_point now);
    void mark_disconnected(std::chrono::steady_clock::time_point now);
    void reset();
    int attempts() const { return attempts_; }

  private:
    BackoffPolicy policy_;
    std::function<double()> random_;
    int attempts_ = 0;
    std::chrono::steady_clock::time_point connected_at_{};
    bool connected_ = false;
};

} // namespace fjarr
