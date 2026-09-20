#include "pad_counter.hpp"

#include "core/log.hpp"

namespace fjarr::media {

namespace {
std::mutex g_registry_mutex;
std::map<GstPad*, PadCounters*> g_registry;
} // namespace

PadCounter::PadCounter(GstPad* pad, std::string label) : pad_(pad), label_(std::move(label)) {
    {
        std::lock_guard<std::mutex> lock(g_registry_mutex);
        g_registry[pad] = &counters_;
    }
    probe_ = glib::PadProbe(pad, GST_PAD_PROBE_TYPE_BUFFER, &PadCounter::probe, this);
}

PadCounter::~PadCounter() {
    probe_.remove();
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    if (pad_) g_registry.erase(pad_);
}

const PadCounters* PadCounter::lookup(GstPad* pad) {
    std::lock_guard<std::mutex> lock(g_registry_mutex);
    auto it = g_registry.find(pad);
    return it == g_registry.end() ? nullptr : it->second;
}

GstPadProbeReturn PadCounter::probe(GstPad*, GstPadProbeInfo* info, gpointer user) {
    auto* self = static_cast<PadCounter*>(user);
    GstBuffer* b = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!b) return GST_PAD_PROBE_OK;
    auto& c = self->counters_;
    c.buffers++;
    c.bytes += gst_buffer_get_size(b);
    if (GST_BUFFER_PTS_IS_VALID(b)) c.last_pts_ns = static_cast<std::int64_t>(GST_BUFFER_PTS(b));
    const std::int64_t now = g_get_monotonic_time();
    const std::int64_t last = c.last_wall_us.exchange(now);
    if (last > 0) {
        const std::int64_t gap = now - last;
        if (gap > c.max_gap_us) c.max_gap_us = gap;
        if (gap > 500000) log::debug("media", "buffer gap on pad", {{"pad", self->label_}, {"gap_ms", std::to_string(gap / 1000)}});
    }
    return GST_PAD_PROBE_OK;
}

} // namespace fjarr::media
