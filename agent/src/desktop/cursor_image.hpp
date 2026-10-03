#pragma once
// A cursor shape as module E's reader hands it on (docs/23#desktop-helper-protocol, The cursor):
// straight-alpha RGBA named by its content. No PipeWire here, so it is unit-tested on its own.
// spec: docs/08 `cursor` · docs/22-remote-desktop-client.md#cursor-strategy
#include <cstdint>
#include <string>
#include <vector>

namespace fjarr::desktop::mutter {

struct CursorImage {
    std::string shape_id;      // a hash of the pixels and the hotspot: mutter's own id never changes
    bool visible = true;       // false: mutter sent an empty shape (no cursor)
    int width = 0, height = 0; // pixels
    int hot_x = 0, hot_y = 0;
    std::vector<std::uint8_t> rgba; // straight alpha, width*height*4
};

/// Straight-alpha RGBA from mutter's bitmap: RGBA or BGRA, maybe premultiplied (a pixel whose colour
/// exceeds its alpha proves it is not).
std::vector<std::uint8_t> to_straight_rgba(const std::uint8_t* pixels, int width, int height, int stride, bool bgra);

/// The content name of a shape (FNV-1a over size, hotspot and pixels; "hidden" for none).
std::string shape_id(const CursorImage& c);

} // namespace fjarr::desktop::mutter
