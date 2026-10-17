// fjarr-opsim: the parts of its checks that need no session (checks.hpp).
// spec: docs/15-testing-strategy.md#latency-harness · #the-desktop-test-lab
#include "checks.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace fjarr::opsim {

using nlohmann::json;

std::string robot_track_stats(const json& session) {
    std::string out;
    const json st = session.value("stats", json::object());
    for (const auto& [cap, tracks] : st.items()) {
        if (!tracks.is_array()) continue;
        for (const auto& t : tracks)
            out += t.value("track_id", "?") + ": enabled=" + (t.value("enabled", false) ? "yes" : "no") + " tier=" + t.value("tier", "?") + " bitrate=" +
                   t.value("bitrate_bps", json()).dump() + " bps frames=" + t.value("frames", json()).dump() + " dropped=" + t.value("dropped", json()).dump() + "; ";
    }
    if (out.empty()) out = st.empty() ? "no stats sample yet; " : "no RTP sent on any track (the robot's last 1 s stats sample lists no sender); ";
    return out + "buffered_bytes=" + session.value("buffered_bytes", json()).dump();
}

ModeStep mode_step(const std::string& plug, unsigned status, const std::string& error, const std::string& body) {
    if (plug.empty()) return {ModeStep::Kind::NotRun, "no --desktop-plug: nothing here can change a monitor's mode"};
    if (status == 200) return {ModeStep::Kind::Run, ""};
    if (status == 404) return {ModeStep::Kind::NotRun, "the fixture has no /mode (404)"};
    // Never "cannot stage": that would hide a dead X11 fixture plug, whose step must run in CI.
    const std::string what = "GET " + plug + "/mode: ";
    if (status == 0) return {ModeStep::Kind::Fail, what + (error.empty() ? "no answer" : error)};
    return {ModeStep::Kind::Fail, what + "HTTP " + std::to_string(status) + (body.empty() ? "" : ": " + body.substr(0, 200))};
}

PhotonRun photon_run(PhotonIo& io, int base) {
    PhotonRun run;
    run.base = base;
    if (base < 0) return run;
    for (int i = 0; i < kPhotonPresses; i++) {
        // A press counts only from a restored baseline. A picture frozen on the lit window (a stalled
        // capture, or a window that stopped repainting) is otherwise "lit" at once and times nothing:
        // nine 0 ms samples passed on the mini-PC on 2026-10-17.
        // spec: docs/15-testing-strategy.md#latency-harness
        if (!io.wait_luma([&](int l) { return std::abs(l - base) <= kPhotonBaseline; }, kPhotonWaitMs)) {
            run.presses.push_back({PhotonPress::Kind::NotAtBaseline, 0, io.luma(), 0});
            if (io.rejected) io.rejected(i);
            continue;
        }
        const std::int64_t t0 = io.now_us();
        io.key(true);
        int widest = 0; // this press's own: a failure says how far its picture got
        const bool lit = io.wait_luma(
            [&](int l) {
                widest = std::max(widest, std::abs(l - base));
                return std::abs(l - base) > kPhotonLit;
            },
            kPhotonWaitMs);
        if (lit) {
            run.presses.push_back({PhotonPress::Kind::Sample, static_cast<double>(io.now_us() - t0) / 1000.0, -1, widest});
        } else {
            run.presses.push_back({PhotonPress::Kind::Unchanged, 0, io.luma(), widest});
            if (io.rejected) io.rejected(i); // while F9 is still down: the frame that should have been lit
        }
        io.key(false);
        io.sleep_ms(kPhotonGapMs);
    }
    return run;
}

PhotonVerdict photon_verdict(const PhotonRun& run) {
    PhotonVerdict v;
    for (const auto& p : run.presses)
        if (p.kind == PhotonPress::Kind::Sample) v.samples.push_back(p.ms);
    std::sort(v.samples.begin(), v.samples.end());
    auto pct = [&](double q) {
        return v.samples.empty() ? 0.0 : v.samples[std::min(v.samples.size() - 1, static_cast<std::size_t>(q * static_cast<double>(v.samples.size())))];
    };
    v.p50 = pct(0.5);
    v.p95 = pct(0.95);
    v.pass = v.samples.size() == static_cast<std::size_t>(kPhotonPresses) && v.p95 <= kPhotonCeilingMs;
    char buf[200];
    std::snprintf(buf, sizeof buf, "%zu/%d samples, p50 %.0f ms, p95 %.0f ms (LAN budget 150/250; CI ceiling %.0f)", v.samples.size(), kPhotonPresses, v.p50,
                  v.p95, kPhotonCeilingMs);
    v.detail = buf;
    if (run.base < 0) {
        v.detail += "; no centre luma to watch (no decoded frame on the track yet, or frames without 8-bit luma)";
        return v;
    }
    std::string per;
    for (const auto& p : run.presses) {
        if (!per.empty()) per += ", ";
        if (p.kind == PhotonPress::Kind::Sample) {
            std::snprintf(buf, sizeof buf, "%.0f", p.ms);
            per += buf;
        } else {
            per += p.kind == PhotonPress::Kind::NotAtBaseline ? "stuck" : "unchanged";
        }
    }
    v.detail += "; per press (ms): " + per;
    // Each kind of rejection, named once, at its first press.
    for (const auto kind : {PhotonPress::Kind::NotAtBaseline, PhotonPress::Kind::Unchanged}) {
        const auto it = std::find_if(run.presses.begin(), run.presses.end(), [&](const PhotonPress& p) { return p.kind == kind; });
        if (it == run.presses.end()) continue;
        const std::string seen = "luma " + std::to_string(it->luma) + ", baseline " + std::to_string(run.base);
        v.detail += "; press " + std::to_string(it - run.presses.begin() + 1) +
                    (kind == PhotonPress::Kind::NotAtBaseline
                         ? " stuck: the centre never returned to its baseline: frozen capture or stuck window (" + seen + ")"
                         : " unchanged: the centre did not change within " + std::to_string(kPhotonWaitMs / 1000) + " s of key-down (" + seen + ", widest change " +
                               std::to_string(it->widest) + ")");
    }
    return v;
}

} // namespace fjarr::opsim
