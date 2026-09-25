/**
 * fjarr.terminal (docs/06, docs/08#terminal): availability is a deployment fact rather than a
 * fault, one pty per session, and the shell dies with the session — no orphans.
 */
#include <pwd.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <fjarr/terminal_capability.hpp>

#include "core/loop.hpp"
#include "media/sources.hpp"

#include "recording_context.hpp"

using namespace fjarr;

namespace {
/// The account this test process is actually running as, which is the only one the agent can
/// start a shell for: it runs unprivileged and cannot switch (docs/10#terminal).
std::string me() {
    const struct passwd* pw = ::getpwuid(::geteuid());
    return pw && pw->pw_name ? pw->pw_name : std::to_string(::geteuid());
}

/// The terminal ignores the source factory entirely; one still has to exist to call configure().
struct Sources {
    fjarr::CoreLoop loop;
    fjarr::media::SourceRegistry reg;
    Sources() : reg(loop.context()) {}
};

Envelope request(const std::string& type, nlohmann::json payload = nlohmann::json::object()) {
    Envelope e;
    e.cap = "fjarr.terminal";
    e.type = type;
    e.kind = "request";
    e.event_id = "ev-" + type;
    e.payload = std::move(payload);
    return e;
}
} // namespace

TEST(TerminalCapability, declaresARawBulkChannelAndTakesTheInputLease) {
    TerminalCapability cap;
    const auto m = cap.manifest();
    EXPECT_EQ(m.name, "fjarr.terminal");
    EXPECT_TRUE(m.input_bearing) << "docs/10: the terminal takes the ownership lease";
    bool raw_bulk = false;
    for (const auto& c : m.channels)
        if (c.channel == ChannelClass::Bulk && c.framing == BulkFraming::Raw) raw_bulk = true;
    EXPECT_TRUE(raw_bulk) << "docs/08#terminal: bytes ride a raw-framed bulk channel";
}

TEST(TerminalCapability, withoutAConfiguredUserItIsUnavailableRatherThanBroken) {
    // docs/06: no pty exists until fjarr.toml names the account. That is a deployment choice, so
    // it must read as `unavailable` — a UI can say "not configured" instead of showing a failure.
    TerminalCapability cap;
    Sources sources;
    cap.configure(nlohmann::json{{"enabled", true}}, sources.reg);
    fjarr::testing::RecordingContext ctx;
    cap.session_attached(ctx, nlohmann::json::object());
    cap.on_message(ctx, request("open", {{"cols", 80}, {"rows", 24}}));
    ASSERT_FALSE(ctx.sent.empty());
    const auto& r = ctx.sent.back();
    EXPECT_EQ(r.kind, "result");
    EXPECT_FALSE(r.payload.value("ok", true));
    EXPECT_EQ(r.payload["error"].value("code", ""), "unavailable");
    cap.shutdown();
}

TEST(TerminalCapability, aUserTheAgentCannotBecomeIsUnavailableAndSaysWhy) {
    // The agent runs unprivileged and cannot switch accounts, so a mismatch is reported instead
    // of silently running the shell as the wrong user.
    TerminalCapability cap;
    Sources sources;
    cap.configure(nlohmann::json{{"enabled", true}, {"user", "definitely-not-" + me()}}, sources.reg);
    fjarr::testing::RecordingContext ctx;
    cap.session_attached(ctx, nlohmann::json::object());
    cap.on_message(ctx, request("open"));
    const auto& err = ctx.sent.back().payload["error"];
    EXPECT_EQ(err.value("code", ""), "unavailable");
    EXPECT_NE(err.value("message", "").find("cannot switch accounts"), std::string::npos) << err.value("message", "");
    cap.shutdown();
}

TEST(TerminalCapability, opensOnePtyPerSessionAndRefusesASecond) {
    TerminalCapability cap;
    Sources sources;
    cap.configure(nlohmann::json{{"enabled", true}, {"user", me()}, {"shell", "/bin/sh"}}, sources.reg);
    fjarr::testing::RecordingContext ctx;
    cap.session_attached(ctx, nlohmann::json::object());
    cap.on_message(ctx, request("open", {{"cols", 100}, {"rows", 30}}));
    ASSERT_TRUE(ctx.sent.back().payload.value("ok", false)) << ctx.sent.back().payload.dump();
    EXPECT_EQ(ctx.watched.size(), 1u) << "the pty's fd is watched on the core loop";

    cap.on_message(ctx, request("open"));
    EXPECT_EQ(ctx.sent.back().payload["error"].value("code", ""), "busy");

    // docs/15: the shell dies with the session, whatever ended it.
    cap.release_all_input(ctx.id());
    cap.on_message(ctx, request("open"));
    EXPECT_TRUE(ctx.sent.back().payload.value("ok", false)) << "a released session can open again";
    cap.shutdown();
}

TEST(TerminalCapability, keystrokesForASessionWithNoPtyAreDropped) {
    // This is also what keeps a read-only operator out: `open` is denied to non-owners by the
    // lease, so they never have a pty, and their bytes reach nothing.
    TerminalCapability cap;
    Sources sources;
    cap.configure(nlohmann::json{{"enabled", true}, {"user", me()}}, sources.reg);
    fjarr::testing::RecordingContext ctx;
    cap.session_attached(ctx, nlohmann::json::object());
    const std::string keys = "rm -rf /\n";
    const std::span<const std::byte> view(reinterpret_cast<const std::byte*>(keys.data()), keys.size());
    EXPECT_NO_THROW(cap.on_binary(ctx, view));
    cap.shutdown();
}
