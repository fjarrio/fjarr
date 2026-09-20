// post_to_owner + generation guards; timers; run_async.
#include <atomic>
#include <thread>

#include <gtest/gtest.h>

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
