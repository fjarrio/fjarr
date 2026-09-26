// post_to_owner + generation guards; timers; run_async.
#include <atomic>
#include <thread>

#include <gtest/gtest.h>

#include <unistd.h>

#include "core/glib/raii.hpp"
#include "core/loop.hpp"

TEST(CoreLoop, postRunsOnTheOwnerThreadAndGuardDropsStaleCallbacks) {
    fjarr::CoreLoop loop;
    loop.start();
    std::atomic<bool> on_owner{false};
    loop.call_sync([&] { on_owner = loop.is_owner_thread(); });
    EXPECT_TRUE(on_owner);
    auto alive = std::make_shared<int>(1);
    std::atomic<int> ran{0};
    std::weak_ptr<int> weak = alive;
    loop.post_guarded([weak] { return !weak.expired(); }, [&] { ran++; });
    loop.call_sync([] {});
    EXPECT_EQ(ran, 1);
    alive.reset();
    loop.post_guarded([weak] { return !weak.expired(); }, [&] { ran++; });
    loop.call_sync([] {});
    EXPECT_EQ(ran, 1); // dropped
    loop.stop();
}

TEST(CoreLoop, timersFireOnTheLoopAndCancelWithTheGuard) {
    fjarr::CoreLoop loop;
    loop.start();
    std::atomic<int> ticks{0};
    fjarr::glib::SourceGuard g;
    loop.call_sync([&] {
        g = loop.add_timeout(std::chrono::milliseconds(5), [&] {
            ticks++;
            return ticks < 3;
        });
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(60));
    EXPECT_EQ(ticks, 3);
    fjarr::glib::SourceGuard h;
    loop.call_sync([&] {
        h = loop.add_timeout(std::chrono::milliseconds(5), [&] {
            ticks++;
            return true;
        });
    });
    loop.call_sync([&] { h.cancel(); });
    const int after = ticks;
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    EXPECT_EQ(ticks, after);
    loop.stop();
}

TEST(CoreLoop, runAsyncCompletesOnTheLoopUnlessDead) {
    fjarr::CoreLoop loop;
    loop.start();
    std::atomic<bool> job_ran{false}, done_on_owner{false};
    auto alive = std::make_shared<bool>(true);
    std::weak_ptr<bool> weak = alive;
    loop.run_async([&] { job_ran = true; }, [&] { done_on_owner = loop.is_owner_thread(); }, [weak] { return !weak.expired(); });
    for (int i = 0; i < 100 && !done_on_owner; i++) std::this_thread::sleep_for(std::chrono::milliseconds(2));
    EXPECT_TRUE(job_ran);
    EXPECT_TRUE(done_on_owner);
    loop.stop();
}

// `add_fd_watch` is the seam `fjarr.terminal`'s pty and `fjarr.net`'s tunnel device read through,
// and nothing covered its teardown: the source was unreffed once by the loop and again by the
// guard, so every close logged `g_source_unref_internal: assertion 'old_ref > 0' failed` and
// touched freed memory. A GLib critical fails this test, and ASan catches the use-after-free.
TEST(CoreLoop, anFdWatchIsReleasedExactlyOnce) {
    // g_log_set_writer_func may be called only once per process, so this counts through the
    // default handler, which can be swapped and put back.
    struct Criticals {
        static int& count() {
            static int n = 0;
            return n;
        }
        static void handler(const gchar* domain, GLogLevelFlags level, const gchar* message, gpointer) {
            if (level & (G_LOG_LEVEL_CRITICAL | G_LOG_LEVEL_WARNING)) count()++;
            g_log_default_handler(domain, level, message, nullptr);
        }
    };
    Criticals::count() = 0;
    GLogFunc previous = g_log_set_default_handler(&Criticals::handler, nullptr);

    int fds[2] = {-1, -1};
    ASSERT_EQ(::pipe(fds), 0);
    fjarr::CoreLoop loop;
    loop.start();
    std::atomic<int> reads{0};
    {
        fjarr::glib::SourceGuard watch;
        loop.call_sync([&] {
            watch = loop.add_fd_watch(fds[0], [&] {
                char buf[8];
                reads += ::read(fds[0], buf, sizeof buf) > 0 ? 1 : 0;
                return true;
            });
        });
        ASSERT_EQ(::write(fds[1], "x", 1), 1);
        for (int i = 0; i < 200 && reads == 0; i++) loop.call_sync([] {});
        EXPECT_EQ(reads, 1);
        loop.call_sync([&] { watch.cancel(); }); // the release the criticals came from
    }
    loop.call_sync([] {});
    loop.stop();
    ::close(fds[0]);
    ::close(fds[1]);
    g_log_set_default_handler(previous, nullptr);
    EXPECT_EQ(Criticals::count(), 0) << "releasing an fd watch logged a GLib critical";
}
