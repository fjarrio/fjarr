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

} // namespace fjarr::opsim
