#pragma once
// CoreLoop — the one thread that owns every piece of control-plane state:
// a private GMainContext, post_to_owner marshaling with generation guards,
// timers, and the worker pool for capability long work.
// spec: docs/23-agent-core-architecture.md#threading-model
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <thread>

#include <glib.h>

#include "glib/raii.hpp"

namespace fjarr {

using Generation = std::uint64_t;

class CoreLoop {
  public:
    CoreLoop();
    ~CoreLoop();
    CoreLoop(const CoreLoop&) = delete;
    CoreLoop& operator=(const CoreLoop&) = delete;

    GMainContext* context() const { return ctx_; }

    /// Run on the calling thread until quit(). The caller becomes the owner thread.
    void run();
    /// Run on a new thread; returns once the loop is running.
    void start();
    /// Stop the loop (from any thread); joins the thread started by start().
    void stop();
    void quit();
    bool running() const { return running_; }

    bool is_owner_thread() const;
    /// Debug assertion: core-loop-only code paths call this.
    void assert_owner(const char* what) const;

    /// Schedule `fn` on the loop from any thread.
    void post(std::function<void()> fn);
    /// Schedule `fn` unless `alive()` is false when it would run (generation guard).
    void post_guarded(std::function<bool()> alive, std::function<void()> fn);
    /// A POSIX signal delivered as an ordinary loop callback (g_unix_signal_source): the
    /// handler runs on the owner thread with no async-signal-safety constraints.
    glib::SourceGuard add_unix_signal(int signum, std::function<void()> fn);
    /// Run `fn` on the loop and wait for it (never from the owner thread).
    void call_sync(std::function<void()> fn);

    /// Timers: `fn` returns true to keep firing.
    [[nodiscard]] glib::SourceGuard add_timeout(std::chrono::milliseconds period, std::function<bool()> fn,
                                                int priority = G_PRIORITY_DEFAULT);
    [[nodiscard]] glib::SourceGuard add_idle(std::function<bool()> fn);
    /// Watch `fd` for readability (and hangup/error) on this loop; `fn` runs on the loop and
    /// returns false to remove the watch. Not `add_timeout`'s shape: GLib dispatches a unix-fd
    /// source as a `GUnixFDSourceFunc`, so it cannot share the plain `GSourceFunc` trampoline —
    /// doing so passes the fd where the closure pointer is expected.
    [[nodiscard]] glib::SourceGuard add_fd_watch(int fd, std::function<bool()> fn);

    /// Worker pool: run `job` off-loop, then `done` on the loop (if `alive`).
    void run_async(std::function<void()> job, std::function<void()> done, std::function<bool()> alive = nullptr);

    /// Drain pending sources without blocking (tests).
    void iterate(bool may_block = false);

  private:
    GMainContext* ctx_;
    GMainLoop* loop_;
    std::atomic<bool> running_{false};
    std::atomic<std::thread::id> owner_{};
    std::thread thread_;
    GThreadPool* pool_ = nullptr;
};

} // namespace fjarr
