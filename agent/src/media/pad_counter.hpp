#pragma once
// Lightweight per-pad buffer counters (buffers, bytes, last PTS, last wall
// time) for introspection (docs/24 pad counters) and stall diagnostics.
#include <atomic>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>

#include <gst/gst.h>

#include "core/glib/raii.hpp"

namespace fjarr::media {

struct PadCounters {
    std::atomic<std::uint64_t> buffers{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<std::int64_t> last_pts_ns{-1};
    std::atomic<std::int64_t> last_wall_us{0};
    std::atomic<std::int64_t> max_gap_us{0};
};

/// Install a counting probe on a pad; the counters are readable by pad pointer
/// while the probe lives (`lookup`).
class PadCounter {
  public:
    PadCounter() = default;
    PadCounter(GstPad* pad, std::string label);
    ~PadCounter();
    PadCounter(PadCounter&&) = delete;
    PadCounter& operator=(PadCounter&&) = delete;
    const PadCounters& counters() const { return counters_; }
    static const PadCounters* lookup(GstPad* pad);

  private:
    static GstPadProbeReturn probe(GstPad*, GstPadProbeInfo* info, gpointer user);
    GstPad* pad_ = nullptr;
    std::string label_;
    PadCounters counters_;
    glib::PadProbe probe_;
};

} // namespace fjarr::media
