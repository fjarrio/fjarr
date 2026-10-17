// fjarr-opsim: the parts of its checks that need no session (checks.hpp).
// spec: docs/15-testing-strategy.md#latency-harness · #the-desktop-test-lab
#include "checks.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <sstream>

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

// spec: docs/15-testing-strategy.md#the-desktop-test-lab (the scanout check, ADR-0041)
bool same_connector(const std::string& kernel, const std::string& mutter) {
    if (kernel == mutter) return true;
    // mutter's connector names follow the kernel's types, but for DRM_MODE_CONNECTOR_HDMIA.
    static const std::string hdmi_a = "HDMI-A-";
    return kernel.rfind(hdmi_a, 0) == 0 && mutter == "HDMI-" + kernel.substr(hdmi_a.size());
}

KmsSample kms_sample(const std::string& text, const std::string& connector, const std::string& compositor) {
    KmsSample out;
    struct Plane {
        std::string name, crtc, fb, size, format, alloc;
        long area = 0;
    };
    std::vector<Plane> planes;
    std::vector<std::pair<std::string, std::string>> connectors; // name, crtc
    std::istringstream in(text);
    std::string line;
    Plane* cur = nullptr;
    std::pair<std::string, std::string>* conn = nullptr;
    auto after = [](const std::string& l, const std::string& key) -> std::string {
        const auto p = l.find(key);
        if (p == std::string::npos) return {};
        std::string v = l.substr(p + key.size());
        const auto e = v.find_first_of(" \t");
        return key == "allocated by = " ? v : v.substr(0, e);
    };
    auto block_name = [](const std::string& l) {
        const auto c = l.find(": ");
        return c == std::string::npos ? l : l.substr(c + 2);
    };
    while (std::getline(in, line)) {
        if (line.rfind("plane[", 0) == 0) {
            planes.push_back({});
            cur = &planes.back();
            conn = nullptr;
            cur->name = block_name(line);
            continue;
        }
        if (line.rfind("connector[", 0) == 0) {
            connectors.push_back({block_name(line), {}});
            conn = &connectors.back();
            cur = nullptr;
            continue;
        }
        if (line.empty() || line[0] != '\t') {
            cur = nullptr; // a crtc[…] block, or the end
            conn = nullptr;
        }
        if (conn && line.rfind("\tcrtc=", 0) == 0) conn->second = after(line, "crtc=");
        if (!cur) continue;
        if (line.rfind("\tcrtc=", 0) == 0) cur->crtc = after(line, "crtc=");
        else if (line.rfind("\tfb=", 0) == 0) cur->fb = after(line, "fb=");
        else if (line.rfind("\t\tallocated by = ", 0) == 0) cur->alloc = after(line, "allocated by = ");
        else if (line.rfind("\t\tformat=", 0) == 0) cur->format = after(line, "format=");
        else if (line.rfind("\t\tsize=", 0) == 0) {
            cur->size = after(line, "size=");
            int w = 0, h = 0;
            if (std::sscanf(cur->size.c_str(), "%dx%d", &w, &h) == 2) cur->area = static_cast<long>(w) * h;
        }
    }
    // The CRTC driving the captured monitor, when the state names it.
    std::string want_crtc, conn_name;
    if (!connector.empty())
        for (const auto& [name, crtc] : connectors)
            if (same_connector(name, connector) && !crtc.empty() && crtc != "(null)") {
                want_crtc = crtc;
                conn_name = name;
            }
    const Plane* screen = nullptr;
    for (const auto& p : planes) {
        if (p.crtc.empty() || p.crtc == "(null)" || p.fb.empty() || p.fb == "0") continue;
        out.crtc = true;
        if (!want_crtc.empty() && p.crtc != want_crtc) continue;
        if (!screen || p.area > screen->area) screen = &p;
    }
    if (!screen) {
        out.detail = out.crtc ? "no plane on " + want_crtc + " (" + conn_name + ")" : "no plane on a CRTC";
        return out;
    }
    out.matched = !want_crtc.empty();
    out.direct = !screen->alloc.empty() && screen->alloc != compositor;
    const std::string where = out.matched ? conn_name + " " + want_crtc + ": "
                                          : (connector.empty() ? std::string() : "no CRTC drives " + connector + ", the largest on any: ");
    out.detail = where + screen->name + " fb=" + screen->fb + " " + screen->size + " " + screen->format + " by '" + screen->alloc + "'";
    return out;
}

std::vector<ScanoutBlink> parse_blinks(const std::string& log, double since_ms) {
    std::vector<ScanoutBlink> out;
    std::istringstream in(log);
    std::string line;
    while (std::getline(in, line)) {
        double t = 0;
        char colour[16] = {};
        if (std::sscanf(line.c_str(), "%lf blink %15s", &t, colour) != 2) continue;
        if (t * 1000.0 < since_ms) continue;
        out.push_back({t * 1000.0, std::string(colour) == "green"});
    }
    return out;
}

ScanoutVerdict scanout_verdict(const std::vector<ScanoutBlink>& blinks, const std::vector<ScanoutFrame>& frames, const std::vector<ScanoutKms>& kms, bool kms_crtc,
                               bool require_kms) {
    ScanoutVerdict v;
    if (!kms_crtc && require_kms) {
        // docs/30 principle 8: on the mini-PC this is a broken check, not one that cannot run there.
        v.kind = ScanoutVerdict::Kind::Fail;
        v.detail = "no KMS CRTC readable in /sys/kernel/debug/dri/*/state on a machine that has one (--require-kms): debugfs is not mounted, or the check is not root";
        return v;
    }
    if (!kms_crtc) {
        v.kind = ScanoutVerdict::Kind::NotRun;
        v.detail = "no KMS CRTC on this machine (a virtual display, as in CI): nothing can be scanned out directly";
        return v;
    }
    if (frames.empty()) {
        v.kind = ScanoutVerdict::Kind::Fail;
        v.detail = "no decoded frame to watch";
        return v;
    }
    const double last_frame = frames.back().t_ms;
    std::string missed;
    for (std::size_t i = 0; i < blinks.size(); i++) {
        const ScanoutBlink& b = blinks[i];
        if (b.t_ms < frames.front().t_ms || b.t_ms + kScanoutFollowMs > last_frame) continue; // outside what was watched
        v.changes++;
        const double until = b.t_ms + kScanoutFollowMs;
        bool shown = false;
        for (const auto& f : frames) {
            if (f.t_ms <= b.t_ms || f.t_ms > until) continue;
            if (b.green ? f.luma >= kScanoutLumaGreen : (f.luma >= 0 && f.luma <= kScanoutLumaMagenta)) {
                shown = true;
                break;
            }
        }
        int seen = 0, direct = 0;
        for (const auto& k : kms)
            if (k.t_ms >= b.t_ms && k.t_ms <= until) {
                seen++;
                direct += k.direct ? 1 : 0;
            }
        if (seen > 0 && direct == seen) v.under_scanout++;
        if (shown) v.followed++;
        else if (missed.empty()) {
            char buf[160];
            std::snprintf(buf, sizeof buf, "; the first change not followed: to %s, %s", b.green ? "green" : "magenta",
                          seen > 0 && direct == seen ? "while scanned out directly" : "while composited");
            missed = buf;
        }
    }
    char buf[200];
    std::snprintf(buf, sizeof buf, "%d of %d changes followed within %d ms; %d of them while the window was scanned out directly", v.followed, v.changes,
                  kScanoutFollowMs, v.under_scanout);
    v.detail = buf + missed;
    if (v.changes < kScanoutMinChanges) {
        v.kind = ScanoutVerdict::Kind::Fail;
        v.detail += "; the window changed fewer than " + std::to_string(kScanoutMinChanges) + " times while watched";
    } else if (v.followed < v.changes) {
        v.kind = ScanoutVerdict::Kind::Fail; // a frozen picture, scanned out or not
    } else if (v.under_scanout < kScanoutMinUnderScanout) {
        v.kind = ScanoutVerdict::Kind::NotRun;
        v.detail += "; fewer than " + std::to_string(kScanoutMinUnderScanout) + " changes under direct scanout (a banner, a menu or the overview stops it)";
    } else {
        v.kind = ScanoutVerdict::Kind::Pass;
    }
    return v;
}

} // namespace fjarr::opsim
