#pragma once
// A cursor shape as module E's reader hands it on (docs/23#desktop-helper-protocol, The cursor):
// straight-alpha RGBA named by its content. No PipeWire here, so it is unit-tested on its own.
// spec: docs/08 `cursor` · docs/22-remote-desktop-client.md#cursor-strategy
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include <fjarr/desktop_backend.hpp>

namespace fjarr::desktop::mutter {

struct CursorImage {
    std::string shape_id;      // a hash of the pixels and the hotspot: mutter's own id never changes
    bool visible = true;       // false: no cursor (mutter's empty shape, or an invalid id)
    bool invalid_id = false;   // hidden because mutter sent an invalid cursor id (0), not its empty shape
    int width = 0, height = 0; // pixels
    int hot_x = 0, hot_y = 0;
    std::vector<std::uint8_t> rgba; // straight alpha, width*height*4
};

/// Straight-alpha RGBA from mutter's bitmap: RGBA or BGRA, maybe premultiplied (a pixel whose colour
/// exceeds its alpha proves it is not).
std::vector<std::uint8_t> to_straight_rgba(const std::uint8_t* pixels, int width, int height, int stride, bool bgra);

/// The content name of a shape (FNV-1a over size, hotspot and pixels; "hidden" for none).
std::string shape_id(const CursorImage& c);

/// What one stream's reader hands on from each buffer's cursor metadata (docs/22#cursor-strategy,
/// docs/23#desktop-capture-memory). Each call returns the shape to send, or nothing when it is the
/// one sent last. Pure, so the reader's decisions are unit-tested (test_cursor_image.cpp).
class ReaderCursor {
  public:
    /// An invalid id (0): an application hid the pointer, or it is on another monitor. A hidden
    /// shape marked `invalid_id`, unless the pointer is hidden already.
    std::optional<CursorImage> invalid_id();
    /// A valid id with a bitmap: that shape (an empty bitmap is mutter's own hidden shape).
    std::optional<CursorImage> bitmap(CursorImage img);
    /// A valid id without a bitmap: mutter sends one only when the shape changes, so a pointer back
    /// from an invalid id shows the shape it had before; anything else is unchanged.
    std::optional<CursorImage> no_bitmap();

  private:
    std::optional<CursorImage> emit(CursorImage img);
    std::string last_shape_;
    std::optional<CursorImage> last_visible_;
    bool hidden_by_invalid_id_ = false; // the hidden shape in force came from an invalid id
};

/// The one shape shown for every monitor's stream (docs/22#cursor-strategy). The pointer is on one
/// monitor at a time, and mutter gives every other monitor's stream an invalid id, so a hidden
/// shape counts only from the monitor the pointer was last seen on, and the pointer arriving on a
/// monitor brings back the shape that monitor's reader sent last, unless that one came from an
/// invalid id (then it may only mean the pointer was elsewhere: the current shape stays).
class CursorArbiter {
  public:
    /// A reader's shape. The shape to show now, when that changed.
    std::optional<CursorImage> on_shape(MonitorId monitor, const CursorImage& img);
    /// The pointer seen on a monitor (a reader's position). The shape to show now, when that changed.
    std::optional<CursorImage> on_position(MonitorId monitor);
    /// The monitor's capture stopped: its shape is forgotten, and so is the pointer being there.
    void forget(MonitorId monitor);
    /// The shape shown now, if any yet.
    const std::optional<CursorImage>& current() const { return shown_; }

  private:
    std::optional<CursorImage> show(const CursorImage& img);
    std::map<MonitorId, CursorImage> by_monitor_; // each reader's latest
    std::optional<MonitorId> pointer_;            // where the pointer was last seen
    std::optional<CursorImage> shown_;
};

} // namespace fjarr::desktop::mutter
