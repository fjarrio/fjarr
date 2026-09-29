// Turns a hung call into a test failure instead of a stuck suite: aborts with `what` unless
// destroyed within the budget. Guard only the call that can hang (a teardown), never a whole
// loop — under valgrind (~30x) a loop's total time is not a hang (nightly 2026-09-29).
#pragma once

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace fjarr::testing {

class Watchdog {
  public:
    explicit Watchdog(const char* what, std::chrono::seconds budget = std::chrono::seconds(60))
        : t_([this, what, budget] {
              const auto deadline = std::chrono::steady_clock::now() + budget;
              while (!done_ && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(std::chrono::milliseconds(50));
              if (!done_) {
                  std::fprintf(stderr, "%s hung\n", what);
                  std::abort();
              }
          }) {}
    ~Watchdog() {
        done_ = true;
        t_.join();
    }
    Watchdog(const Watchdog&) = delete;
    Watchdog& operator=(const Watchdog&) = delete;

  private:
    std::atomic<bool> done_{false};
    std::thread t_;
};

} // namespace fjarr::testing
