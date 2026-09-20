#pragma once
// The machine-readable frame stamp, painted into raw frames before encoding.
// spec: docs/25-browser-lab.md#frame-stamp-the-latency-harnesss-oracle
// Layout: 96 blocks, max(4, width/128) px wide, 16 px tall, top-left; bits
// MSB first: 8-bit sync 0xA5, 32-bit counter, 48-bit unix-ms, 8-bit XOR.
#include <cstdint>

#include <gst/gst.h>
#include <gst/video/video.h>

namespace fjarr::media {

inline constexpr int STAMP_BLOCKS = 96;
inline constexpr int STAMP_HEIGHT = 16;
inline constexpr std::uint8_t STAMP_SYNC = 0xA5;

inline int stamp_block_width(int width) { return width / 128 > 4 ? width / 128 : 4; }

/// Encode the 12 stamp bytes.
void encode_stamp(std::uint32_t counter, std::uint64_t ts_ms, std::uint8_t out[12]);

/// Paint the strip into a writable mapped raw frame (luma plane only). Returns
/// false when the format has no luma plane or the frame is too small.
bool paint_stamp(GstVideoFrame* frame, std::uint32_t counter, std::uint64_t ts_ms);

/// A pad probe that paints the stamp on every buffer passing a raw video pad.
/// Attach to the source output pad (before the tee); counts frames.
struct StampPainter {
    GstVideoInfo info{};
    bool have_info = false;
    std::uint32_t counter = 0;
    static GstPadProbeReturn probe(GstPad* pad, GstPadProbeInfo* info, gpointer user);
};

} // namespace fjarr::media
