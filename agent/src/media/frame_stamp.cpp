#include "frame_stamp.hpp"

#include <cstring>

#include "core/glib/raii.hpp"

namespace fjarr::media {

void encode_stamp(std::uint32_t counter, std::uint64_t ts_ms, std::uint8_t out[12]) {
    out[0] = STAMP_SYNC;
    out[1] = static_cast<std::uint8_t>(counter >> 24);
    out[2] = static_cast<std::uint8_t>(counter >> 16);
    out[3] = static_cast<std::uint8_t>(counter >> 8);
    out[4] = static_cast<std::uint8_t>(counter);
    for (int i = 10; i >= 5; i--) {
        out[i] = static_cast<std::uint8_t>(ts_ms & 0xff);
        ts_ms >>= 8;
    }
    std::uint8_t x = 0;
    for (int i = 0; i < 11; i++) x ^= out[i];
    out[11] = x;
}

bool paint_stamp(GstVideoFrame* frame, std::uint32_t counter, std::uint64_t ts_ms) {
    const GstVideoFormatInfo* finfo = frame->info.finfo;
    if (!finfo || GST_VIDEO_FORMAT_INFO_IS_RGB(finfo) || GST_VIDEO_FRAME_N_PLANES(frame) < 1) return false;
    const int width = GST_VIDEO_FRAME_WIDTH(frame);
    const int height = GST_VIDEO_FRAME_HEIGHT(frame);
    const int bw = stamp_block_width(width);
    if (width < bw * STAMP_BLOCKS || height < STAMP_HEIGHT) return false;
    if (GST_VIDEO_FRAME_COMP_DEPTH(frame, 0) != 8) return false; // 8-bit luma only (I420/NV12/YUY2…)
    std::uint8_t bytes[12];
    encode_stamp(counter, ts_ms, bytes);
    auto* data = static_cast<std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(frame, 0));
    const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(frame, 0);
    const int pstride = GST_VIDEO_FRAME_COMP_PSTRIDE(frame, 0); // 1 for planar, 2 for YUY2
    const int offset = GST_VIDEO_FRAME_COMP_OFFSET(frame, 0);
    for (int i = 0; i < STAMP_BLOCKS; i++) {
        const bool bit = (bytes[i >> 3] >> (7 - (i & 7))) & 1;
        const std::uint8_t y = bit ? 235 : 16; // video-range white / black
        for (int row = 0; row < STAMP_HEIGHT; row++) {
            std::uint8_t* p = data + row * stride + offset + i * bw * pstride;
            for (int x = 0; x < bw; x++) p[x * pstride] = y;
        }
    }
    return true;
}

GstPadProbeReturn StampPainter::probe(GstPad* pad, GstPadProbeInfo* info, gpointer user) {
    auto* self = static_cast<StampPainter*>(user);
    if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
        GstEvent* ev = GST_PAD_PROBE_INFO_EVENT(info);
        if (GST_EVENT_TYPE(ev) == GST_EVENT_CAPS) {
            GstCaps* caps = nullptr;
            gst_event_parse_caps(ev, &caps);
            self->have_info = caps && gst_video_info_from_caps(&self->info, caps);
        }
        return GST_PAD_PROBE_OK;
    }
    if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) return GST_PAD_PROBE_OK;
    if (!self->have_info) {
        glib::GstCapsPtr caps(gst_pad_get_current_caps(pad));
        self->have_info = caps && gst_video_info_from_caps(&self->info, caps.get());
        if (!self->have_info) return GST_PAD_PROBE_OK;
    }
    GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
    // The probe may replace the buffer with a writable one (probe contract).
    buf = gst_buffer_make_writable(buf);
    if (!buf) return GST_PAD_PROBE_OK; // the probe contract: a failed copy leaves the original in place
    GST_PAD_PROBE_INFO_DATA(info) = buf;
    GstVideoFrame frame;
    if (!gst_video_frame_map(&frame, &self->info, buf, GST_MAP_WRITE)) return GST_PAD_PROBE_OK;
    const std::uint64_t ts = static_cast<std::uint64_t>(g_get_real_time() / 1000);
    paint_stamp(&frame, ++self->counter, ts);
    gst_video_frame_unmap(&frame);
    return GST_PAD_PROBE_OK;
}

} // namespace fjarr::media
