#include "loop.hpp"

#include <cassert>
#include <condition_variable>
#include <mutex>

#include <glib-unix.h>

#include "log.hpp"

namespace fjarr {

CoreLoop::CoreLoop() : ctx_(g_main_context_new()), loop_(g_main_loop_new(ctx_, FALSE)) {
    pool_ = g_thread_pool_new(
        [](gpointer data, gpointer) {
            auto* job = static_cast<std::function<void()>*>(data);
            (*job)();
            delete job;
        },
        nullptr, 4, FALSE, nullptr);
}

CoreLoop::~CoreLoop() {
    stop();
    if (pool_) g_thread_pool_free(pool_, FALSE, TRUE); // run what is queued: each job frees itself
    g_main_loop_unref(loop_);
    g_main_context_unref(ctx_);
}

void CoreLoop::run() {
    owner_ = std::this_thread::get_id();
    running_ = true;
    g_main_context_push_thread_default(ctx_);
    g_main_loop_run(loop_);
    g_main_context_pop_thread_default(ctx_);
    running_ = false;
}

void CoreLoop::start() {
    if (running_) return;
    std::mutex m;
    std::condition_variable cv;
    bool started = false;
    thread_ = std::thread([&] {
        owner_ = std::this_thread::get_id();
        running_ = true;
        g_main_context_push_thread_default(ctx_);
        glib::invoke_once(ctx_, [&] {
            std::lock_guard<std::mutex> lock(m);
            started = true;
            cv.notify_all();
        });
        g_main_loop_run(loop_);
        g_main_context_pop_thread_default(ctx_);
        running_ = false;
    });
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&] { return started; });
}

void CoreLoop::quit() { g_main_loop_quit(loop_); }

void CoreLoop::stop() {
    if (running_) quit();
    if (thread_.joinable()) {
        if (thread_.get_id() == std::this_thread::get_id()) thread_.detach();
        else thread_.join();
    }
}

bool CoreLoop::is_owner_thread() const { return owner_.load() == std::this_thread::get_id(); }

void CoreLoop::assert_owner(const char* what) const {
#ifndef NDEBUG
    if (running_ && !is_owner_thread()) {
        log::error("core", "called off the core loop", {{"what", what}});
        assert(false && "core-loop-only method called from another thread");
    }
#else
    (void)what;
#endif
}

void CoreLoop::post(std::function<void()> fn) { glib::invoke_once(ctx_, std::move(fn)); }

void CoreLoop::post_guarded(std::function<bool()> alive, std::function<void()> fn) {
    glib::invoke_once(ctx_, [alive = std::move(alive), fn = std::move(fn)] {
        if (alive && !alive()) return;
        fn();
    });
}

void CoreLoop::call_sync(std::function<void()> fn) {
    if (is_owner_thread() || !running_) {
        fn();
        return;
    }
    std::mutex m;
    std::condition_variable cv;
    bool done = false;
    post([&] {
        fn();
        std::lock_guard<std::mutex> lock(m);
        done = true;
        cv.notify_all();
    });
    std::unique_lock<std::mutex> lock(m);
    cv.wait(lock, [&] { return done; });
}

glib::SourceGuard CoreLoop::add_timeout(std::chrono::milliseconds period, std::function<bool()> fn, int priority) {
    GSource* s = g_timeout_source_new(static_cast<guint>(period.count()));
    g_source_set_priority(s, priority);
    return glib::SourceGuard(s, ctx_, std::move(fn));
}

glib::SourceGuard CoreLoop::add_unix_signal(int signum, std::function<void()> fn) {
    return glib::SourceGuard(g_unix_signal_source_new(signum), ctx_, [fn = std::move(fn)] {
        fn();
        return true;
    });
}

glib::SourceGuard CoreLoop::add_idle(std::function<bool()> fn) {
    return glib::SourceGuard(g_idle_source_new(), ctx_, std::move(fn));
}

void CoreLoop::run_async(std::function<void()> job, std::function<void()> done, std::function<bool()> alive) {
    auto* boxed = new std::function<void()>([this, job = std::move(job), done = std::move(done), alive = std::move(alive)] {
        job();
        if (done) post_guarded(alive, done);
    });
    GError* err = nullptr;
    if (!g_thread_pool_push(pool_, boxed, &err)) {
        glib::GErrorPtr guard(err);
        log::error("core", "thread pool push failed", {{"error", err ? err->message : "?"}});
        delete boxed;
    }
}

void CoreLoop::iterate(bool may_block) {
    while (g_main_context_iteration(ctx_, may_block ? TRUE : FALSE)) {
        may_block = false;
    }
}

} // namespace fjarr
