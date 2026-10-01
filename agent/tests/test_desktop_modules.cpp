/**
 * The desktop module seam (ADR-0021): backends load at runtime from separate packages, so the
 * core carries no X11 or Wayland dependency. What is tested here is the loading and, just as
 * importantly, what a robot is told when nothing loads — "unavailable" on its own is useless to
 * whoever has to fix it.
 */
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "desktop/module_loader.hpp"

using namespace fjarr::desktop;

namespace {
struct Env {
    explicit Env(const char* k, const char* v) : key(k) {
        if (v) ::setenv(k, v, 1);
        else ::unsetenv(k);
    }
    ~Env() { ::unsetenv(key); }
    const char* key;
};

std::filesystem::path temp_dir(const std::string& tag) {
    const auto d = std::filesystem::temp_directory_path() / ("fjarr-desktop-" + tag + "-" + std::to_string(::getpid()));
    std::filesystem::remove_all(d);
    std::filesystem::create_directories(d);
    return d;
}
} // namespace

TEST(DesktopModules, aRobotWithNoModuleIsToldWhichPackageToInstall) {
    // The normal state of a robot that streams a camera and has no desktop. It is not an error,
    // and the message has to be actionable rather than a bare "unavailable".
    ModuleLoader loader;
    loader.scan((temp_dir("empty")).string());
    EXPECT_TRUE(loader.found().empty());
    EXPECT_EQ(loader.select("auto"), nullptr);
    const std::string why = loader.unavailable_reason("auto");
    EXPECT_NE(why.find("fjarr-desktop-x11"), std::string::npos) << why;
    EXPECT_NE(why.find("fjarr-desktop-wayland"), std::string::npos) << why;
}

TEST(DesktopModules, aMissingDirectoryIsNotAnError) {
    ModuleLoader loader;
    EXPECT_NO_THROW(loader.scan("/definitely/not/here"));
    EXPECT_TRUE(loader.found().empty());
}

TEST(DesktopModules, loadsAModuleFromItsOwnSharedObjectAndMakesABackend) {
    ModuleLoader loader;
    loader.scan(FJARR_STUB_MODULE_DIR);
    ASSERT_EQ(loader.found().size(), 1u) << "the stub module was not discovered";
    const Found& f = loader.found().front();
    EXPECT_EQ(f.display_server, "stub");
    EXPECT_EQ(f.package, "fjarr-desktop-stub");
    EXPECT_TRUE(f.usable) << f.reason;

    const Found* chosen = loader.select("auto");
    ASSERT_NE(chosen, nullptr);
    std::string error;
    const fjarr::desktop::ModuleHost host{"{}", nullptr, [](int, const char*, const char*) {}};
    auto backend = loader.create(*chosen, host, &error);
    EXPECT_NE(backend, nullptr) << error;
    EXPECT_TRUE(backend->monitors().empty());
}

TEST(DesktopModules, anInstalledModuleThatCannotRunHereSaysWhy) {
    // "Installed" and "usable" are different questions: a Wayland module on an X11 session is
    // present and useless, and the operator needs the second answer, not the first.
    Env unusable("FJARR_STUB_UNUSABLE", "no DISPLAY in this session");
    ModuleLoader loader;
    loader.scan(FJARR_STUB_MODULE_DIR);
    ASSERT_EQ(loader.found().size(), 1u);
    EXPECT_FALSE(loader.found().front().usable);
    EXPECT_EQ(loader.select("auto"), nullptr);
    EXPECT_NE(loader.unavailable_reason("auto").find("no DISPLAY in this session"), std::string::npos)
        << loader.unavailable_reason("auto");
}

TEST(DesktopModules, aConfiguredBackendThatIsNotInstalledIsNotSubstituted) {
    // `backend = "wayland"` with only an X11 module installed must fail, not quietly use X11:
    // the operator asked for something specific, probably for a reason.
    ModuleLoader loader;
    loader.scan(FJARR_STUB_MODULE_DIR);
    ASSERT_FALSE(loader.found().empty());
    EXPECT_NE(loader.select("stub"), nullptr);
    EXPECT_EQ(loader.select("wayland"), nullptr);
    EXPECT_NE(loader.unavailable_reason("wayland").find("backend = \"wayland\""), std::string::npos)
        << loader.unavailable_reason("wayland");
}

TEST(DesktopModules, somethingThatIsNotOurModuleIsIgnoredWithAReasonRatherThanLoaded) {
    // A stray .so in the directory — another project's, or one built against an older seam. The
    // version is in the symbol name, so the wrong one is simply not found.
    const auto dir = temp_dir("stray");
    std::ofstream(dir / "libnot-a-module.so") << "this is not an ELF file";
    ModuleLoader loader;
    loader.scan(dir.string());
    ASSERT_EQ(loader.found().size(), 1u);
    EXPECT_FALSE(loader.found().front().usable);
    EXPECT_FALSE(loader.found().front().reason.empty());
    EXPECT_EQ(loader.select("auto"), nullptr);
}

#include <fjarr/desktop_capability.hpp>

#include "core/loop.hpp"
#include "media/sources.hpp"
#include "recording_context.hpp"

namespace {
/// The capability ignores the source factory; one still has to exist to call configure().
struct Sources {
    fjarr::CoreLoop loop;
    fjarr::media::SourceRegistry reg;
    Sources() : reg(loop.context()) {}
};
} // namespace

TEST(DesktopCapability, aRobotWithNoDesktopIsNormalAndSaysWhatToInstall) {
    // ADR-0021: the capability is always present, so "can this robot share its screen?" has an
    // answer everywhere — and when the answer is no, it names the package rather than shrugging.
    Sources sources;
    fjarr::DesktopCapability cap;
    cap.configure(nlohmann::json{{"module_dir", "/definitely/not/here"}}, sources.reg);
    const auto rows = cap.configured_sources();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].available);
    EXPECT_NE(rows[0].reason.find("fjarr-desktop-x11"), std::string::npos) << rows[0].reason;
    EXPECT_FALSE(rows[0].required) << "a missing desktop must never be a startup error";
}

TEST(DesktopCapability, withAModuleInstalledItReportsWhichBackendServesIt) {
    Sources sources;
    fjarr::DesktopCapability cap;
    cap.configure(nlohmann::json{{"module_dir", FJARR_STUB_MODULE_DIR}}, sources.reg);
    const auto rows = cap.configured_sources();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].available) << rows[0].reason;
    EXPECT_EQ(rows[0].identity, "stub");
}

TEST(DesktopCapability, disabledIsADeploymentChoiceAndSaysSo) {
    Sources sources;
    fjarr::DesktopCapability cap;
    cap.configure(nlohmann::json{{"enabled", false}, {"module_dir", FJARR_STUB_MODULE_DIR}}, sources.reg);
    const auto rows = cap.configured_sources();
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_FALSE(rows[0].available);
    EXPECT_NE(rows[0].reason.find("disabled"), std::string::npos) << rows[0].reason;
}

// --- input (M3 3.2) ------------------------------------------------------------------------------
// spec: docs/08-protocol.md#input-events-fjarrdesktop · docs/15-testing-strategy.md#safety-behaviors
namespace {
struct InputRig {
    std::filesystem::path log = temp_dir("input") / "calls.txt";
    Env record{"FJARR_STUB_RECORD", log.c_str()};
    Sources sources;
    fjarr::DesktopCapability cap;
    fjarr::testing::RecordingContext a, b;
    InputRig() {
        cap.configure(nlohmann::json{{"module_dir", FJARR_STUB_MODULE_DIR}}, sources.reg);
        a.sid = "session-a";
        b.sid = "session-b";
        cap.session_attached(a, nlohmann::json::object());
        cap.session_attached(b, nlohmann::json::object());
    }
    void send(fjarr::testing::RecordingContext& ctx, const std::string& type, nlohmann::json payload, const std::string& kind = "event") {
        cap.on_message(ctx, fjarr::Envelope{"fjarr.desktop", type, "e-" + type, kind, std::move(payload)});
    }
    std::vector<std::string> calls() const {
        std::vector<std::string> out;
        std::ifstream in(log);
        for (std::string line; std::getline(in, line);) out.push_back(line);
        return out;
    }
};
} // namespace

TEST(DesktopInput, eachMessageReachesTheBackendAsItsEvdevCall) {
    InputRig r;
    r.send(r.a, "pointer", {{"track_id", "desk-virtual-1"}, {"x", 0.5}, {"y", 0.25}, {"seq", 1}});
    r.send(r.a, "button", {{"button", "right"}, {"down", true}});
    r.send(r.a, "button", {{"button", "right"}, {"down", false}});
    r.send(r.a, "wheel", {{"dx", 0}, {"dy", 48}});
    r.send(r.a, "key", {{"code", "KeyA"}, {"down", true}});
    r.send(r.a, "key", {{"code", "KeyA"}, {"down", false}});
    r.send(r.a, "key-combo", {{"codes", {"ControlLeft", "AltLeft", "Delete"}}}, "request");
    EXPECT_EQ(r.calls(), (std::vector<std::string>{"pointer 7 0.500000 0.250000", "button 2 down", "button 2 up", "wheel 0.000000 48.000000",
                                                  "key 30 down", "key 30 up", "key 29 down", "key 56 down", "key 111 down", "key 111 up",
                                                  "key 56 up", "key 29 up"}));
    ASSERT_EQ(r.a.sent.size(), 1u);
    EXPECT_TRUE(r.a.sent[0].payload["ok"].get<bool>()) << "key-combo is answered";
}

TEST(DesktopInput, staleOrOffScreenPointerMotionIsDropped) {
    InputRig r;
    r.send(r.a, "pointer", {{"track_id", "desk-virtual-1"}, {"x", 0.1}, {"y", 0.1}, {"seq", 5}});
    r.send(r.a, "pointer", {{"track_id", "desk-virtual-1"}, {"x", 0.9}, {"y", 0.9}, {"seq", 4}}); // older: realtime is unordered
    r.send(r.a, "pointer", {{"track_id", "desk-virtual-1"}, {"x", 1.5}, {"y", 0.1}, {"seq", 6}}); // outside the monitor
    r.send(r.a, "pointer", {{"track_id", "desk-nope"}, {"x", 0.2}, {"y", 0.2}, {"seq", 7}});      // no such monitor
    EXPECT_EQ(r.calls(), (std::vector<std::string>{"pointer 7 0.100000 0.100000"}));
}

TEST(DesktopInput, textIsTypedWholeOrRefusedNamingWhatTheLayoutLacks) {
    InputRig r;
    r.send(r.a, "text", {{"text", "hej"}}, "request");
    r.send(r.a, "text", {{"text", "hå"}}, "request");
    EXPECT_EQ(r.calls(), (std::vector<std::string>{"text hej"}));
    ASSERT_EQ(r.a.sent.size(), 2u);
    EXPECT_TRUE(r.a.sent[0].payload["ok"].get<bool>());
    EXPECT_EQ(r.a.sent[1].payload["error"]["code"], "unavailable");
    EXPECT_EQ(r.a.sent[1].payload["error"]["data"]["untypable"], nlohmann::json::array({"å"}));
}

TEST(DesktopInput, unknownKeysAndButtonsAreRefusedAndNeverInjected) {
    InputRig r;
    r.send(r.a, "key", {{"code", "NotAKey"}, {"down", true}}, "request");
    r.send(r.a, "button", {{"button", "sideways"}, {"down", true}}, "request");
    r.send(r.a, "key-combo", {{"codes", {"ControlLeft", "Nope"}}}, "request");
    EXPECT_TRUE(r.calls().empty()) << "a combo with an unknown key presses nothing, not half of it";
    ASSERT_EQ(r.a.sent.size(), 3u);
    for (const auto& s : r.a.sent) EXPECT_EQ(s.payload["error"]["code"], "payload-invalid");
}

TEST(DesktopInput, safetyTheEndOfTheSessionWhoseInputIsOnTheDesktopReleasesItAndNoOtherDoes) {
    // docs/15 safety: release_all_input on session end. But a viewer leaving (or its window losing
    // focus, `release-all`) must not let go of keys the holder is pressing.
    InputRig r;
    r.send(r.a, "key", {{"code", "ShiftLeft"}, {"down", true}});
    r.cap.release_all_input(r.b.sid);
    r.send(r.b, "release-all", nlohmann::json::object());
    EXPECT_EQ(r.calls(), (std::vector<std::string>{"key 42 down"}));
    r.cap.release_all_input(r.a.sid); // the core, as session-a ends
    EXPECT_EQ(r.calls().back(), "release-all");
}

TEST(DesktopInput, safetyWhenInputMovesToAnotherSessionThePreviousHoldersKeysGoUpFirst) {
    InputRig r;
    r.send(r.a, "key", {{"code", "ControlLeft"}, {"down", true}});
    r.send(r.b, "key", {{"code", "KeyC"}, {"down", true}}); // b took control: a's Ctrl must not make this Ctrl+C
    EXPECT_EQ(r.calls(), (std::vector<std::string>{"key 29 down", "release-all", "key 46 down"}));
}
