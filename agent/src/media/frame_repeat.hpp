#pragma once
// FjarrFrameRepeat: the custom GStreamer meta a source adds to a buffer that repeats content it
// already sent (module E's keepalive on a still screen, and the frame it gives a newly built
// pipeline), so new frames and repeats are counted apart (docs/24#capture-frame-counts).
// Header-only on purpose: libfjarr counts it and module E, a separate shared object, adds it; the
// meta is registered once per process, by whichever side comes first.
// spec: docs/23-agent-core-architecture.md#desktop-capture-memory
#include <atomic>
#include <mutex>

#include <gst/gst.h>

namespace fjarr::media {

inline constexpr const char* FRAME_REPEAT_META = "FjarrFrameRepeat";

/// Registers the meta if no one in this process has yet (gst_meta_register_custom_simple).
inline const GstMetaInfo* frame_repeat_meta_info() {
    static std::atomic<const GstMetaInfo*> cached{nullptr}; // per shared object: the agent and the module each have one
    if (const GstMetaInfo* info = cached.load(std::memory_order_acquire)) return info;
    static std::mutex mu;
    std::lock_guard<std::mutex> lk(mu);
    const GstMetaInfo* info = gst_meta_get_info(FRAME_REPEAT_META);
    if (!info) info = gst_meta_register_custom_simple(FRAME_REPEAT_META);
    cached.store(info, std::memory_order_release);
    return info;
}

/// Marks `buffer` (writable) as a repeat.
inline void mark_repeat(GstBuffer* buffer) { gst_buffer_add_meta(buffer, frame_repeat_meta_info(), nullptr); }

/// Whether `buffer` repeats content its source sent before.
inline bool is_repeat(GstBuffer* buffer) { return gst_buffer_get_meta(buffer, frame_repeat_meta_info()->api) != nullptr; }

} // namespace fjarr::media
