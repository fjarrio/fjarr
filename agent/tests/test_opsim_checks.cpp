// fjarr-opsim's checks that need no session (agent/tools/opsim/checks.hpp): the robot's view in a
// failure message, whether desktop-control's mode-change step runs, and the input-to-photon loop
// against a simulated window on a virtual clock.
// spec: docs/15-testing-strategy.md#latency-harness · #the-desktop-test-lab
#include <gtest/gtest.h>

#include <iterator>
#include <map>

#include "opsim/checks.hpp"

using fjarr::opsim::kPhotonPresses;
using fjarr::opsim::mode_step;
using fjarr::opsim::ModeStep;
using fjarr::opsim::photon_run;
using fjarr::opsim::photon_verdict;
using fjarr::opsim::PhotonIo;
using fjarr::opsim::PhotonPress;
using fjarr::opsim::PhotonRun;
using fjarr::opsim::robot_track_stats;
using nlohmann::json;

namespace {

/// The test window seen through a capture: magenta (luma 78), green (145) while F9 is held, each key
/// event showing `delay_us` later. After `changes_shown` changes the picture stops changing: a capture
/// that stalled, or a window that stopped repainting (-1: never).
struct FakeWindow {
    int base = 78, lit = 145;
    std::int64_t delay_us = 17'000;
    int changes_shown = -1;
    std::int64_t now = 0;
    std::map<std::int64_t, int> picture; // from this time on, the centre's luma
    bool f9_down = false;
    int presses = 0;
    std::vector<int> rejected;     // the presses handed to the `rejected` hook
    std::vector<bool> rejected_f9; // F9 down at that moment

    int luma_at(std::int64_t t) const {
        auto it = picture.upper_bound(t);
        return it == picture.begin() ? base : std::prev(it)->second;
    }
    void show(int luma) {
        if (changes_shown >= 0 && static_cast<int>(picture.size()) >= changes_shown) return; // frozen
        picture[now + delay_us] = luma;
    }
    PhotonIo io() {
        PhotonIo io;
        io.wait_luma = [this](const std::function<bool(int)>& pred, int timeout_ms) {
            const std::int64_t until = now + timeout_ms * 1000LL;
            if (pred(luma_at(now))) return true;
            for (auto it = picture.upper_bound(now); it != picture.end() && it->first <= until; ++it)
                if (pred(it->second)) {
                    now = it->first;
                    return true;
                }
            now = until;
            return false;
        };
        io.luma = [this] { return luma_at(now); };
        io.key = [this](bool down) {
            f9_down = down;
            if (down) presses++;
            show(down ? lit : base);
        };
        io.now_us = [this] { return now; };
        io.sleep_ms = [this](int ms) { now += ms * 1000LL; };
        io.rejected = [this](int press) {
            rejected.push_back(press);
            rejected_f9.push_back(f9_down);
        };
        return io;
    }
};

PhotonRun run(FakeWindow& w) {
    auto io = w.io();
    return photon_run(io, w.luma_at(w.now));
}

} // namespace

TEST(OpsimRobotView, aSessionThatSendsNoRtpSaysSoAndNotSelectedPair) {
    // desktop-e2e in CI, 2026-10-14: ICE had connected, but "no frames within 10 s (robot:
    // selected_pair=""; buffered_bytes=0)" was read as ICE never selecting a pair. The agent never
    // fills that field; what the sample says is that no track was sending.
    const json s = json::parse(R"({"session_id":"c7abd691","buffered_bytes":0,
        "stats":{"selected_pair":"","twcc":{},"rtx":{"requests":0,"packets":0}}})");
    EXPECT_EQ(robot_track_stats(s), "no RTP sent on any track (the robot's last 1 s stats sample lists no sender); buffered_bytes=0");
}

TEST(OpsimRobotView, aSendingTrackIsListedWithItsCounters) {
    const json s = json::parse(R"({"buffered_bytes":12,"stats":{"selected_pair":"","twcc":{},
        "fjarr.desktop":[{"track_id":"desk-virtual-1","enabled":true,"tier":"active","bitrate_bps":92464,"frames":5,"dropped":0}]}})");
    const std::string text = robot_track_stats(s);
    EXPECT_EQ(text, "desk-virtual-1: enabled=yes tier=active bitrate=92464 bps frames=5 dropped=0; buffered_bytes=12");
    EXPECT_EQ(text.find("selected_pair"), std::string::npos);
}

TEST(OpsimRobotView, aSessionBeforeItsFirstSampleSaysSo) {
    EXPECT_EQ(robot_track_stats(json::parse(R"({"buffered_bytes":0,"stats":{}})")), "no stats sample yet; buffered_bytes=0");
}

TEST(OpsimModeStep, withoutAPlugTheStepIsNotRunByName) {
    // The mini-PC runs desktop-control without a plug service. The relative "/mode" this used to
    // request failed before it was sent (status 0), and the step ran anyway and failed every night.
    const ModeStep s = mode_step("", 0, "bad url /mode", "");
    EXPECT_EQ(s.kind, ModeStep::Kind::NotRun);
    EXPECT_NE(s.why.find("no --desktop-plug"), std::string::npos) << s.why;
}

TEST(OpsimModeStep, aFixtureWithoutModeIsNotRunByName) {
    const ModeStep s = mode_step("http://desktop-fixture:8091", 404, "", "404 /plug, /unplug, ...");
    EXPECT_EQ(s.kind, ModeStep::Kind::NotRun);
    EXPECT_EQ(s.why, "the fixture has no /mode (404)");
}

TEST(OpsimModeStep, aPlugThatChangedTheModeRunsTheStep) {
    EXPECT_EQ(mode_step("http://x11-fixture:8091", 200, "", "1600/420x900/236+0+0").kind, ModeStep::Kind::Run);
}

TEST(OpsimModeStep, aConfiguredPlugThatDoesNotAnswerFailsNamingTheError) {
    // A dead X11 plug must not pass as "this fixture cannot change a mode".
    const ModeStep s = mode_step("http://x11-fixture:8091", 0, "Could not connect to x11-fixture: Connection refused", "");
    EXPECT_EQ(s.kind, ModeStep::Kind::Fail);
    EXPECT_EQ(s.why, "GET http://x11-fixture:8091/mode: Could not connect to x11-fixture: Connection refused");
    EXPECT_EQ(mode_step("http://x11-fixture:8091", 0, "", "").why, "GET http://x11-fixture:8091/mode: no answer");
}

TEST(OpsimModeStep, aPlugThatFailsTheChangeFailsWithItsStatusAndBody) {
    const ModeStep s = mode_step("http://x11-fixture:8091", 500, "", "xrandr: BadValue");
    EXPECT_EQ(s.kind, ModeStep::Kind::Fail);
    EXPECT_EQ(s.why, "GET http://x11-fixture:8091/mode: HTTP 500: xrandr: BadValue");
}

TEST(OpsimPhoton, aWindowThatLightsAndRestoresGivesTenSamples) {
    FakeWindow w;
    const PhotonRun r = run(w);
    ASSERT_EQ(r.presses.size(), static_cast<std::size_t>(kPhotonPresses));
    for (const auto& p : r.presses) {
        EXPECT_EQ(p.kind, PhotonPress::Kind::Sample);
        EXPECT_DOUBLE_EQ(p.ms, 17.0);
    }
    const auto v = photon_verdict(r);
    EXPECT_TRUE(v.pass) << v.detail;
    EXPECT_EQ(v.detail, "10/10 samples, p50 17 ms, p95 17 ms (LAN budget 150/250; CI ceiling 500); per press (ms): 17, 17, 17, 17, 17, 17, 17, 17, 17, 17");
    EXPECT_TRUE(w.rejected.empty());
}

TEST(OpsimPhoton, aPictureFrozenOnTheLitWindowFailsOnTheBaseline) {
    // The mini-PC on 2026-10-17: the first press lit the window after 201 ms, then the picture never
    // changed again. Without the baseline, nine presses "lit" at once: 10/10, p50 0 ms, a pass.
    FakeWindow w;
    w.delay_us = 201'000;
    w.changes_shown = 1;
    const PhotonRun r = run(w);
    ASSERT_EQ(r.presses.size(), static_cast<std::size_t>(kPhotonPresses));
    EXPECT_EQ(r.presses[0].kind, PhotonPress::Kind::Sample);
    for (std::size_t i = 1; i < r.presses.size(); i++) {
        EXPECT_EQ(r.presses[i].kind, PhotonPress::Kind::NotAtBaseline) << "press " << i + 1;
        EXPECT_EQ(r.presses[i].luma, 145);
    }
    EXPECT_EQ(w.presses, 1) << "F9 is never pressed from a picture that is not back at its baseline";
    const auto v = photon_verdict(r);
    EXPECT_FALSE(v.pass);
    EXPECT_EQ(v.detail, "1/10 samples, p50 201 ms, p95 201 ms (LAN budget 150/250; CI ceiling 500); "
                        "per press (ms): 201, stuck, stuck, stuck, stuck, stuck, stuck, stuck, stuck, stuck; "
                        "press 2 stuck: the centre never returned to its baseline: frozen capture or stuck window (luma 145, baseline 78)");
    EXPECT_EQ(w.rejected, (std::vector<int>{1, 2, 3, 4, 5, 6, 7, 8, 9}));
}

TEST(OpsimPhoton, aPictureThatNeverChangesFailsEveryPressAsUnchanged) {
    // The mini-PC from 2026-10-05 to 10-14: input reached the window, the picture stayed magenta.
    FakeWindow w;
    w.changes_shown = 0;
    const PhotonRun r = run(w);
    for (const auto& p : r.presses) EXPECT_EQ(p.kind, PhotonPress::Kind::Unchanged);
    EXPECT_EQ(w.presses, kPhotonPresses);
    const auto v = photon_verdict(r);
    EXPECT_FALSE(v.pass);
    EXPECT_NE(v.detail.find("0/10 samples"), std::string::npos) << v.detail;
    EXPECT_NE(v.detail.find("; press 1 unchanged: the centre did not change within 2 s of key-down (luma 78, baseline 78, widest change 0)"), std::string::npos)
        << v.detail;
    ASSERT_EQ(w.rejected.size(), static_cast<std::size_t>(kPhotonPresses));
    EXPECT_TRUE(w.rejected_f9[0]) << "an unchanged press keeps its frame while F9 is still down";
}

TEST(OpsimPhoton, anUnchangedPressReportsItsOwnWidestChange) {
    // Press 1 lights the window (a change of 67); then the picture stops changing on magenta. Press
    // 2's "unchanged" must say how far press 2 got (0), not the widest change of the whole run.
    FakeWindow w;
    w.changes_shown = 2;
    const PhotonRun r = run(w);
    EXPECT_EQ(r.presses[0].kind, PhotonPress::Kind::Sample);
    EXPECT_EQ(r.presses[0].widest, 67);
    EXPECT_EQ(r.presses[1].kind, PhotonPress::Kind::Unchanged);
    const auto v = photon_verdict(r);
    EXPECT_FALSE(v.pass);
    EXPECT_NE(v.detail.find("; press 2 unchanged: the centre did not change within 2 s of key-down (luma 78, baseline 78, widest change 0)"), std::string::npos)
        << v.detail;
}

TEST(OpsimPhoton, aWindowThatOnceRestoresTooLateLosesThatPressOnly) {
    // One release that takes 2.5 s to show: the next press waits 2 s for the baseline and is given
    // up; the one after that starts from the restored picture and counts.
    FakeWindow w;
    auto io = w.io();
    const auto key = io.key;
    int events = 0;
    io.key = [&](bool down) {
        events++;
        if (events == 2) w.delay_us = 2'500'000; // the first release
        key(down);
        w.delay_us = 17'000;
    };
    const PhotonRun r = photon_run(io, w.base);
    EXPECT_EQ(r.presses[0].kind, PhotonPress::Kind::Sample);
    EXPECT_EQ(r.presses[1].kind, PhotonPress::Kind::NotAtBaseline);
    for (std::size_t i = 2; i < r.presses.size(); i++) EXPECT_EQ(r.presses[i].kind, PhotonPress::Kind::Sample) << "press " << i + 1;
    const auto v = photon_verdict(r);
    EXPECT_FALSE(v.pass) << "the verdict needs 10 timed presses";
    EXPECT_NE(v.detail.find("9/10 samples"), std::string::npos) << v.detail;
}

TEST(OpsimPhoton, aSlowWindowFailsOnTheCeiling) {
    FakeWindow w;
    w.delay_us = 600'000;
    const auto v = photon_verdict(run(w));
    EXPECT_EQ(v.samples.size(), static_cast<std::size_t>(kPhotonPresses));
    EXPECT_FALSE(v.pass) << v.detail;
    EXPECT_DOUBLE_EQ(v.p95, 600.0);
}

TEST(OpsimPhoton, aChangeFasterThanAFrameStillCounts) {
    // mutter captures on damage, so there is no one-frame floor: CI's samples are about 17 ms.
    FakeWindow w;
    w.delay_us = 4'000;
    const auto v = photon_verdict(run(w));
    EXPECT_TRUE(v.pass) << v.detail;
    EXPECT_DOUBLE_EQ(v.p50, 4.0);
}

TEST(OpsimPhoton, noCentreLumaPressesNothing) {
    FakeWindow w;
    auto io = w.io();
    const PhotonRun r = photon_run(io, -1);
    EXPECT_TRUE(r.presses.empty());
    EXPECT_EQ(w.presses, 0);
    const auto v = photon_verdict(r);
    EXPECT_FALSE(v.pass);
    // -1 is also a track with no decoded frame yet (it stopped after step 2b): the message says both.
    EXPECT_NE(v.detail.find("no centre luma to watch (no decoded frame on the track yet, or frames without 8-bit luma)"), std::string::npos) << v.detail;
}

// ------------------------------------------------------------ the scanout check (ADR-0041)

namespace {
/// A DRM debugfs state file, trimmed to what kms_sample reads: the screen's plane and the cursor's.
std::string dri_state(const std::string& screen_alloc, const char* screen_format = "XR24", bool on_crtc = true) {
    const std::string crtc = on_crtc ? "crtc-0" : "(null)";
    return "plane[31]: plane-0\n\tcrtc=" + crtc + "\n\tfb=" + (on_crtc ? "117" : "0") + "\n\t\tallocated by = " + screen_alloc +
           "\n\t\trefcount=2\n\t\tformat=" + screen_format + " little-endian (0x34325258)\n\t\tmodifier=0x200000000000901\n\t\tsize=1920x1080\n" +
           "\t\tlayers:\n\t\t\tsize[0]=1920x1080\n\tcrtc-pos=1920x1080+0+0\n" +
           "plane[40]: plane-5\n\tcrtc=" + crtc + "\n\tfb=" + (on_crtc ? "130" : "0") + "\n\t\tallocated by = gnome-shell\n\t\tformat=AR24 little-endian (0x34325241)\n\t\tsize=256x256\n" +
           "crtc[57]: crtc-0\n\tenable=1\n\tactive=1\n";
}

/// The window blinking every 2 s from t=1000 ms, and a capture that shows each change `delay` ms later,
/// until `frozen_after` changes (-1: never).
struct ScanoutRun {
    std::vector<fjarr::opsim::ScanoutBlink> blinks;
    std::vector<fjarr::opsim::ScanoutFrame> frames;
    std::vector<fjarr::opsim::ScanoutKms> kms;
    ScanoutRun(int changes, int delay, int frozen_after = -1, bool direct = true) {
        for (int i = 0; i < changes; i++) blinks.push_back({1000.0 + 2000.0 * i, i % 2 == 0});
        int shown = 0;
        bool green = false;
        for (double t = 0; t <= 1000.0 + 2000.0 * changes + 1500; t += 10) {
            for (const auto& b : blinks)
                if (b.t_ms + delay <= t && b.t_ms + delay > t - 10 && (frozen_after < 0 || shown < frozen_after)) {
                    green = b.green;
                    shown++;
                }
            frames.push_back({t, green ? 145 : 78});
            if (static_cast<int>(t) % 50 == 0) kms.push_back({t, direct});
        }
    }
};
} // namespace

TEST(OpsimScanout, theKmsStateTellsADirectlyScannedOutWindowFromTheCompositedScreen) {
    const auto composited = fjarr::opsim::kms_sample(dri_state("gnome-shell", "XR30"));
    EXPECT_TRUE(composited.crtc);
    EXPECT_FALSE(composited.direct) << composited.detail;
    const auto direct = fjarr::opsim::kms_sample(dri_state("KMS thread"));
    EXPECT_TRUE(direct.crtc);
    EXPECT_TRUE(direct.direct) << direct.detail;
    EXPECT_NE(direct.detail.find("plane-0 fb=117 1920x1080 XR24 by 'KMS thread'"), std::string::npos) << direct.detail;
    // The cursor plane (small, gnome-shell's) is never mistaken for the screen.
    const auto none = fjarr::opsim::kms_sample(dri_state("KMS thread", "XR24", false));
    EXPECT_FALSE(none.crtc) << "no plane on a CRTC: a virtual display";
    EXPECT_FALSE(fjarr::opsim::kms_sample("").crtc);
}

TEST(OpsimScanout, theWindowsBlinksAreReadFromItsLogSinceTheRunStarted) {
    const auto b = fjarr::opsim::parse_blinks("1760000000.100 start pid=1\n1760000001.000 blink green\n1760000003.000 blink magenta\nnoise\n", 1760000000500.0);
    ASSERT_EQ(b.size(), 2u);
    EXPECT_TRUE(b[0].green);
    EXPECT_FALSE(b[1].green);
    EXPECT_DOUBLE_EQ(b[1].t_ms, 1760000003000.0);
}

TEST(OpsimScanout, everyChangeFollowedUnderDirectScanoutPasses) {
    const ScanoutRun run(8, 150);
    const auto v = fjarr::opsim::scanout_verdict(run.blinks, run.frames, run.kms, true);
    EXPECT_EQ(v.kind, fjarr::opsim::ScanoutVerdict::Kind::Pass) << v.detail;
    EXPECT_EQ(v.followed, v.changes);
    EXPECT_GE(v.under_scanout, fjarr::opsim::kScanoutMinUnderScanout);
}

TEST(OpsimScanout, aPictureFrozenBehindTheScannedOutWindowFails) {
    // The 2026-10-17 freeze: the first frame, then nothing, while the counters stayed green.
    const ScanoutRun run(8, 150, 1);
    const auto v = fjarr::opsim::scanout_verdict(run.blinks, run.frames, run.kms, true);
    EXPECT_EQ(v.kind, fjarr::opsim::ScanoutVerdict::Kind::Fail) << v.detail;
    EXPECT_LT(v.followed, v.changes);
    EXPECT_NE(v.detail.find("while scanned out directly"), std::string::npos) << v.detail;
    // Slower than the bound is not following either.
    const ScanoutRun slow(8, fjarr::opsim::kScanoutFollowMs + 200);
    EXPECT_EQ(fjarr::opsim::scanout_verdict(slow.blinks, slow.frames, slow.kms, true).kind, fjarr::opsim::ScanoutVerdict::Kind::Fail);
}

TEST(OpsimScanout, withoutDirectScanoutOrAKmsCrtcTheCheckDidNotRun) {
    const ScanoutRun composited(8, 150, -1, false);
    const auto v = fjarr::opsim::scanout_verdict(composited.blinks, composited.frames, composited.kms, true);
    EXPECT_EQ(v.kind, fjarr::opsim::ScanoutVerdict::Kind::NotRun) << v.detail;
    const ScanoutRun ci(8, 150);
    const auto none = fjarr::opsim::scanout_verdict(ci.blinks, ci.frames, ci.kms, false);
    EXPECT_EQ(none.kind, fjarr::opsim::ScanoutVerdict::Kind::NotRun);
    EXPECT_NE(none.detail.find("no KMS CRTC"), std::string::npos) << none.detail;
    // A window that hardly changed proves nothing: a failure, so a broken window is noticed.
    const ScanoutRun few(3, 150);
    EXPECT_EQ(fjarr::opsim::scanout_verdict(few.blinks, few.frames, few.kms, true).kind, fjarr::opsim::ScanoutVerdict::Kind::Fail);
    // Composited and frozen: a failure all the same.
    const ScanoutRun frozen(8, 150, 1, false);
    EXPECT_EQ(fjarr::opsim::scanout_verdict(frozen.blinks, frozen.frames, frozen.kms, true).kind, fjarr::opsim::ScanoutVerdict::Kind::Fail);
}

namespace {
/// One CRTC's screen plane, and a connector block that names its CRTC.
std::string screen_plane(int id, const std::string& crtc, const std::string& alloc, const char* format) {
    return "plane[" + std::to_string(id) + "]: plane-" + std::to_string(id) + "\n\tcrtc=" + crtc + "\n\tfb=" + std::to_string(100 + id) +
           "\n\t\tallocated by = " + alloc + "\n\t\tformat=" + format + " little-endian (0x34325258)\n\t\tsize=1920x1080\n";
}
std::string connector_block(int id, const std::string& name, const std::string& crtc) {
    return "connector[" + std::to_string(id) + "]: " + name + "\n\tcrtc=" + crtc + "\n\tself_refresh_aware=0\n";
}
} // namespace

TEST(OpsimScanout, theScreenJudgedIsTheCrtcThatDrivesTheCapturedMonitorInEitherOrder) {
    // The mini-PC: a ghost screen composited by gnome-shell beside the monitor whose window is
    // scanned out, two CRTCs of one size. The strict largest-wins rule took the first listed.
    const std::string ghost = screen_plane(31, "crtc-0", "gnome-shell", "XR30"), window = screen_plane(47, "crtc-1", "KMS thread", "XR24");
    const std::string conns = connector_block(95, "HDMI-A-1", "crtc-0") + connector_block(103, "DP-2", "crtc-1") + connector_block(110, "DP-3", "(null)");
    for (const std::string& state : {ghost + window + "crtc[57]: crtc-0\n" + conns, window + ghost + "crtc[57]: crtc-0\n" + conns}) {
        const auto k = fjarr::opsim::kms_sample(state, "DP-2");
        EXPECT_TRUE(k.crtc);
        EXPECT_TRUE(k.matched) << k.detail;
        EXPECT_TRUE(k.direct) << k.detail;
        EXPECT_NE(k.detail.find("DP-2 crtc-1: plane-47"), std::string::npos) << k.detail;
        // The ghost, as mutter names its HDMI-A connector: composited, whichever comes first.
        const auto g = fjarr::opsim::kms_sample(state, "HDMI-1");
        EXPECT_TRUE(g.matched) << g.detail;
        EXPECT_FALSE(g.direct) << g.detail;
        EXPECT_NE(g.detail.find("HDMI-A-1 crtc-0: plane-31"), std::string::npos) << g.detail;
    }
}

TEST(OpsimScanout, aMonitorNoCrtcDrivesFallsBackToTheLargestPlaneAndSaysSo) {
    const std::string state = screen_plane(31, "crtc-0", "KMS thread", "XR24") + connector_block(95, "HDMI-A-1", "crtc-0") + connector_block(103, "DP-3", "(null)");
    const auto off = fjarr::opsim::kms_sample(state, "DP-3");
    EXPECT_TRUE(off.crtc);
    EXPECT_FALSE(off.matched);
    EXPECT_NE(off.detail.find("no CRTC drives DP-3, the largest on any: plane-31"), std::string::npos) << off.detail;
    const auto unnamed = fjarr::opsim::kms_sample(state);
    EXPECT_FALSE(unnamed.matched);
    EXPECT_EQ(unnamed.detail.find("no CRTC drives"), std::string::npos) << "no connector asked about: " << unnamed.detail;
}

TEST(OpsimScanout, mutterNamesTheKernelsHdmiAConnectorsHdmi) {
    EXPECT_TRUE(fjarr::opsim::same_connector("HDMI-A-1", "HDMI-1"));
    EXPECT_TRUE(fjarr::opsim::same_connector("DP-2", "DP-2"));
    EXPECT_TRUE(fjarr::opsim::same_connector("HDMI-A-1", "HDMI-A-1"));
    EXPECT_FALSE(fjarr::opsim::same_connector("HDMI-A-1", "HDMI-2"));
    EXPECT_FALSE(fjarr::opsim::same_connector("DP-1", "DP-10"));
    EXPECT_FALSE(fjarr::opsim::same_connector("eDP-1", "DP-1"));
}

TEST(OpsimScanout, withoutAKmsCrtcWhereOneIsRequiredTheCheckFails) {
    // The mini-PC (--require-kms): no CRTC readable means debugfs or root went missing, which a
    // NOTE would have hidden behind a green night (docs/30 principle 8).
    const auto v = fjarr::opsim::scanout_verdict({}, {}, {}, false, true);
    EXPECT_EQ(v.kind, fjarr::opsim::ScanoutVerdict::Kind::Fail);
    EXPECT_NE(v.detail.find("--require-kms"), std::string::npos) << v.detail;
    EXPECT_EQ(fjarr::opsim::scanout_verdict({}, {}, {}, false, false).kind, fjarr::opsim::ScanoutVerdict::Kind::NotRun);
}
