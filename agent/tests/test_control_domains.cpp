// The docs/10 control-domain rules as pure bookkeeping, in virtual time: claim on
// first input, one holder per domain, the 5 s desktop idle, motion never idling
// out, take-control, release, fail-open after 30 s without a heartbeat, and who
// must have their input released on each change.
// spec: docs/10-security.md#session-ownership · docs/15-testing-strategy.md#safety-behaviors
#include <gtest/gtest.h>

#include "core/control_domains.hpp"

using namespace fjarr;
using namespace fjarr::core;
using namespace std::chrono_literals;

namespace {
const OperatorInfo anna{"anna@example.com", "Anna"};
const OperatorInfo bob{"bob@example.com", "Bob"};
const ControlDomains::Clock::time_point t0{};
} // namespace

TEST(ControlDomains, nothingIsHeldUntilTheFirstInputWhichClaims) {
    ControlDomains d;
    EXPECT_EQ(d.holder("desktop"), nullptr);
    EXPECT_FALSE(d.any_held());
    auto r = d.input("desktop", anna, t0, 1000);
    EXPECT_EQ(r.outcome, ControlDomains::Outcome::Allowed);
    ASSERT_TRUE(r.change);
    EXPECT_EQ(r.change->domain, "desktop");
    EXPECT_EQ(r.change->why, "claim");
    EXPECT_FALSE(r.change->release) << "a free domain has nobody to release";
    ASSERT_NE(d.holder("desktop"), nullptr);
    EXPECT_EQ(d.holder("desktop")->op.id, anna.id);
    EXPECT_EQ(d.holder("desktop")->since_ms, 1000);
    // The holder's next input is allowed and changes nothing.
    auto again = d.input("desktop", anna, t0 + 1s, 2000);
    EXPECT_EQ(again.outcome, ControlDomains::Outcome::Allowed);
    EXPECT_FALSE(again.change);
    EXPECT_EQ(d.holder("desktop")->since_ms, 1000) << "since is when the claim began";
}

TEST(ControlDomains, aNonHolderIsRefusedAndTheDomainsAreIndependent) {
    ControlDomains d;
    d.input("desktop", anna, t0, 1);
    EXPECT_EQ(d.input("desktop", bob, t0 + 1s, 2).outcome, ControlDomains::Outcome::Held);
    EXPECT_EQ(d.holder("desktop")->op.id, anna.id) << "a refused input claims nothing";
    // Someone on the desktop and someone driving work at once.
    auto drive = d.input("motion", bob, t0 + 1s, 2);
    EXPECT_EQ(drive.outcome, ControlDomains::Outcome::Allowed);
    ASSERT_TRUE(drive.change);
    EXPECT_EQ(d.holder("motion")->op.id, bob.id);
    EXPECT_EQ(d.holder("desktop")->op.id, anna.id);
}

TEST(ControlDomains, desktopFreesFiveSecondsAfterTheHoldersLastInput) {
    ControlDomains d;
    d.input("desktop", anna, t0, 1);
    d.input("desktop", anna, t0 + 4s, 2); // keeps it
    d.heartbeat(anna.id, t0 + 8s);
    EXPECT_TRUE(d.expire(t0 + 8s).empty()) << "4 s after the last input: still held";
    EXPECT_EQ(d.input("desktop", bob, t0 + 8s, 3).outcome, ControlDomains::Outcome::Held);
    auto changes = d.expire(t0 + 9s + 1ms);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].why, "idle");
    EXPECT_FALSE(changes[0].release) << "an idle holder is not released until someone else takes over";
    EXPECT_EQ(d.holder("desktop"), nullptr);
    // The next to move the pointer takes it, and the idle holder's input is released then.
    auto r = d.input("desktop", bob, t0 + 10s, 4);
    EXPECT_EQ(r.outcome, ControlDomains::Outcome::Allowed);
    ASSERT_TRUE(r.change);
    ASSERT_TRUE(r.change->release);
    EXPECT_EQ(*r.change->release, anna.id);
}

TEST(ControlDomains, anIdleDesktopHolderWhoComesBackFirstKeepsTheirInput) {
    ControlDomains d;
    d.input("desktop", anna, t0, 1);
    d.expire(t0 + 6s);
    auto r = d.input("desktop", anna, t0 + 7s, 2);
    ASSERT_TRUE(r.change);
    EXPECT_FALSE(r.change->release) << "nothing to release for a reclaim by the same operator";
    // And the remembered idle holder is gone: a later takeover releases Anna once, as the holder.
    auto t = d.take("desktop", bob, t0 + 8s, 3);
    ASSERT_TRUE(t && t->release);
    EXPECT_EQ(*t->release, anna.id);
}

TEST(ControlDomains, motionNeverFreesOnIdle) {
    ControlDomains d;
    d.input("motion", anna, t0, 1);
    for (auto t = 1s; t < 29s; t += 1s) d.heartbeat(anna.id, t0 + t);
    EXPECT_TRUE(d.expire(t0 + 29s).empty()) << "docs/10: motion never hands over on idle";
    EXPECT_EQ(d.input("motion", bob, t0 + 29s, 2).outcome, ControlDomains::Outcome::Held);
}

TEST(ControlDomains, takeControlMovesTheClaimAndNamesWhoseInputToReleaseFirst) {
    ControlDomains d;
    d.input("motion", anna, t0, 1);
    auto c = d.take("motion", bob, t0 + 1s, 2);
    ASSERT_TRUE(c);
    EXPECT_EQ(c->why, "take");
    ASSERT_TRUE(c->release);
    EXPECT_EQ(*c->release, anna.id) << "the previous holder's robot stops before Bob's first command";
    EXPECT_EQ(d.holder("motion")->op.id, bob.id);
    EXPECT_EQ(d.holder("motion")->since_ms, 2);
    EXPECT_EQ(d.input("motion", anna, t0 + 2s, 3).outcome, ControlDomains::Outcome::Held);
    // Taking what you hold changes nothing; taking a free domain releases nobody.
    EXPECT_FALSE(d.take("motion", bob, t0 + 2s, 4));
    auto free = d.take("desktop", anna, t0 + 2s, 5);
    ASSERT_TRUE(free);
    EXPECT_FALSE(free->release);
}

TEST(ControlDomains, releaseIsIdempotentAndOnlyTheHolderCanRelease) {
    ControlDomains d;
    d.input("motion", anna, t0, 1);
    EXPECT_FALSE(d.release("motion", bob.id)) << "Bob cannot release Anna's claim";
    EXPECT_EQ(d.holder("motion")->op.id, anna.id);
    auto c = d.release("motion", anna.id);
    ASSERT_TRUE(c);
    ASSERT_TRUE(c->release);
    EXPECT_EQ(*c->release, anna.id) << "giving up motion stops what you were driving";
    EXPECT_EQ(d.holder("motion"), nullptr);
    EXPECT_FALSE(d.release("motion", anna.id));
    EXPECT_FALSE(d.release("desktop", anna.id));
}

TEST(ControlDomains, failsOpenAfterThirtySecondsWithoutHeartbeatAndReleasesAtOnce) {
    ControlDomains d;
    d.input("motion", anna, t0, 1);
    d.input("desktop", anna, t0, 1);
    d.heartbeat(anna.id, t0 + 10s);
    for (auto t = 10s; t < 40s; t += 1s) d.input("desktop", anna, t0 + t, 2); // input is not a heartbeat
    EXPECT_TRUE(d.expire(t0 + 40s).empty()) << "exactly 30 s: not yet stale";
    auto changes = d.expire(t0 + 40s + 1ms);
    ASSERT_EQ(changes.size(), 2u);
    for (const auto& c : changes) {
        EXPECT_EQ(c.why, "stale");
        ASSERT_TRUE(c.release) << "a stale holder's input is released now (docs/15)";
        EXPECT_EQ(*c.release, anna.id);
    }
    EXPECT_FALSE(d.any_held());
    EXPECT_EQ(d.input("motion", bob, t0 + 41s, 3).outcome, ControlDomains::Outcome::Allowed);
}

TEST(ControlDomains, aHeartbeatKeepsOnlyItsOwnOperatorsClaims) {
    ControlDomains d;
    d.input("motion", anna, t0, 1);
    d.input("desktop", bob, t0, 1);
    d.heartbeat(anna.id, t0 + 25s);
    d.input("desktop", bob, t0 + 29s, 2);
    auto changes = d.expire(t0 + 31s);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].domain, "desktop");
    EXPECT_EQ(changes[0].why, "stale");
    EXPECT_EQ(d.holder("motion")->op.id, anna.id);
}

TEST(ControlDomains, goneEndsTheClaimWithoutAReleaseTheCloseAlreadyDid) {
    ControlDomains d;
    d.input("motion", anna, t0, 1);
    EXPECT_FALSE(d.gone("motion", bob.id));
    auto c = d.gone("motion", anna.id);
    ASSERT_TRUE(c);
    EXPECT_EQ(c->why, "gone");
    EXPECT_FALSE(c->release);
    EXPECT_EQ(d.holder("motion"), nullptr);
    // A gone idle desktop holder is forgotten too: nobody to release later.
    d.input("desktop", anna, t0, 2);
    d.expire(t0 + 6s);
    d.gone("desktop", anna.id);
    auto r = d.input("desktop", bob, t0 + 7s, 3);
    ASSERT_TRUE(r.change);
    EXPECT_FALSE(r.change->release);
}

TEST(ControlDomains, onlyDesktopAndMotionAreDomains) {
    EXPECT_TRUE(ControlDomains::known("desktop"));
    EXPECT_TRUE(ControlDomains::known("motion"));
    EXPECT_FALSE(ControlDomains::known(""));
    EXPECT_FALSE(ControlDomains::known("terminal"));
}
