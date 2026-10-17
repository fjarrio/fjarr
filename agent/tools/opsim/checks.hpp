#pragma once
// The parts of fjarr-opsim's checks that need no session, apart so they are unit-tested
// (agent/tests/test_opsim_checks.cpp): whether desktop-control's mode-change step runs, what counts
// as an input-to-photon sample, and what a failure says about the robot's own view of its tracks.
// spec: docs/15-testing-strategy.md#latency-harness · #the-desktop-test-lab
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace fjarr::opsim {

// ------------------------------------------------------- the robot's view

/// One session's entry in the robot's GET /stats (docs/24) as text, for a failure about frames that
/// did not arrive: "<track>: enabled= tier= bitrate= frames= dropped=" per track, then buffered_bytes.
/// The robot lists a track only while it sends RTP (rtpbin's internal senders, consumer.cpp
/// get_stats), so a sample that lists none says that no RTP went out on any track. `selected_pair`
/// is left out: the agent never fills it (consumer.hpp), and its "" was read as ICE never selecting
/// a pair, while ICE had connected in every such session (2026-10-17).
std::string robot_track_stats(const nlohmann::json& session);

// ------------------------------------------------------------- mode change

/// What the mode-change step does, given the fixture's plug service (--desktop-plug) and its answer
/// to GET <plug>/mode.
struct ModeStep {
    enum class Kind { Run, NotRun, Fail };
    Kind kind = Kind::NotRun;
    std::string why; // NotRun and Fail: the reason, by name
};

/// Run only when a plug changed the mode (200). No plug (the mini-PC) and a fixture without /mode
/// (404: the mutter one) are "not run", by name. Anything else fails and names the error or the HTTP
/// status and body: a plug that is configured but does not answer is a broken fixture, not one that
/// cannot change a mode. `status` 0 is no HTTP answer at all (`error` says why).
ModeStep mode_step(const std::string& plug, unsigned status, const std::string& error, const std::string& body);

// --------------------------------------------------------- input-to-photon

constexpr int kPhotonPresses = 10;
constexpr int kPhotonLit = 20;           // the window is lit: the centre's mean luma moved more than this
constexpr int kPhotonBaseline = 8;       // back at the baseline: within this of the luma before the first press
constexpr int kPhotonWaitMs = 2000;      // for the baseline before a press, and for the change after it
constexpr int kPhotonGapMs = 150;        // after each release
constexpr double kPhotonCeilingMs = 500; // CI's loose ceiling on the p95 (docs/15); docs/16's LAN budget is 150/250

/// One press of F9: timed, or rejected with its reason.
struct PhotonPress {
    enum class Kind { Sample, NotAtBaseline, Unchanged };
    Kind kind = Kind::Unchanged;
    double ms = 0; // Sample: key-down to the first decoded frame whose centre changed
    int luma = -1; // rejected: the centre's luma when the press was given up
    int widest = 0; // the largest change of the centre seen while F9 was down for this press
};

/// The session the loop runs in, as hooks, so that the loop itself is tested with a fake window.
struct PhotonIo {
    /// Wait until the latest decoded centre luma satisfies `pred`; false when `timeout_ms` passes first.
    std::function<bool(const std::function<bool(int)>& pred, int timeout_ms)> wait_luma;
    std::function<int()> luma;            // the latest decoded centre luma
    std::function<void(bool down)> key;   // F9
    std::function<std::int64_t()> now_us; // monotonic
    std::function<void(int ms)> sleep_ms;
    std::function<void(int press)> rejected; // optional: called as a press is rejected (opsim keeps that frame)
};

struct PhotonRun {
    int base = -1; // the centre's luma before the first press; -1: no decoded frame with a centre to watch
    std::vector<PhotonPress> presses;
};

/// kPhotonPresses presses of F9, each from a restored baseline only.
PhotonRun photon_run(PhotonIo& io, int base);

struct PhotonVerdict {
    bool pass = false;
    std::vector<double> samples; // the timed presses, sorted
    double p50 = 0, p95 = 0;
    std::string detail; // the percentiles, every press in order, and each kind of rejection named once
};

/// The check: kPhotonPresses timed presses, and a p95 at most kPhotonCeilingMs.
PhotonVerdict photon_verdict(const PhotonRun& run);

// ------------------------------------------------------------ scanout check

/// The mini-PC's scanout check (ADR-0041, docs/15#the-desktop-test-lab): a fullscreen GPU window
/// changes colour by itself with the pointer hidden, and the decoded video must follow every change,
/// while the machine's KMS state shows the window scanned out directly.
constexpr int kScanoutFollowMs = 1000;    // a change must show in the decoded video within this
constexpr int kScanoutMinChanges = 6;     // the window must have changed at least this often
constexpr int kScanoutMinUnderScanout = 4; // ... this many of them while scanned out, or the check did not run
constexpr int kScanoutLumaGreen = 135;    // decoded centre luma at or above: the window's green
constexpr int kScanoutLumaMagenta = 115;  // ... at or below: its magenta (BT.601 and BT.709 alike)

/// One read of a DRM debugfs `state` file (/sys/kernel/debug/dri/<n>/state).
struct KmsSample {
    bool crtc = false;    // some plane is on an enabled CRTC with a framebuffer: a real display
    bool matched = false; // the screen judged is the CRTC that drives the connector asked about
    bool direct = false;  // the screen's full-screen plane scans out a buffer the compositor did not composite
    std::string detail;   // "HDMI-A-1 crtc-0: plane-3 fb=123 1920x1080 XR24 by 'KMS thread'"
};
/// The screen is the CRTC that drives `connector` (the captured monitor, as mutter names it): the
/// plane with the largest framebuffer on it (the cursor plane's is small). Without a connector, or
/// when none matches, the largest framebuffer on any CRTC, and `detail` says so: a machine with a
/// ghost screen beside a real monitor has two CRTCs of one size. It is scanned out directly when
/// that framebuffer was not made by `compositor`'s own thread: mutter 50.1 on amdgpu allocates its
/// composited onscreen buffers from `gnome-shell` and imports a client's buffer for direct scanout
/// from its `KMS thread` (kmsmon.py, spikes/capture-scanout, 2026-10-17).
KmsSample kms_sample(const std::string& dri_state, const std::string& connector = "", const std::string& compositor = "gnome-shell");
/// Whether the kernel's connector name and mutter's name one connector: the same, but for HDMI-A,
/// which mutter calls HDMI (the kernel's "HDMI-A-1" is mutter's "HDMI-1").
bool same_connector(const std::string& kernel_name, const std::string& mutter_name);

struct ScanoutBlink {
    double t_ms = 0; // wall clock, from the window's log
    bool green = false;
};
/// The window's own log ("<epoch seconds> blink green|magenta" lines), from `since_ms` on.
std::vector<ScanoutBlink> parse_blinks(const std::string& window_log, double since_ms);

struct ScanoutFrame {
    double t_ms = 0; // wall clock when the decoded centre luma was read
    int luma = -1;
};
struct ScanoutKms {
    double t_ms = 0;
    bool direct = false;
};

struct ScanoutVerdict {
    enum class Kind { Pass, Fail, NotRun };
    Kind kind = Kind::NotRun;
    int changes = 0, followed = 0, under_scanout = 0;
    std::string detail;
};
/// Every change of the window followed by the decoded video within kScanoutFollowMs; not run when
/// the machine has no KMS CRTC (CI's fixtures) or too few changes happened under direct scanout (a
/// banner, a menu or the overview stops it). A change not followed fails, scanned out or not.
/// `require_kms` (opsim --require-kms, the mini-PC's nightly): a machine that must have a KMS CRTC
/// and shows none has lost debugfs or root, which fails the check instead of leaving it not run.
ScanoutVerdict scanout_verdict(const std::vector<ScanoutBlink>& blinks, const std::vector<ScanoutFrame>& frames, const std::vector<ScanoutKms>& kms,
                               bool kms_crtc, bool require_kms = false);

} // namespace fjarr::opsim
