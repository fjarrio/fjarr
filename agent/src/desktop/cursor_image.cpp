// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol (The cursor)
#include "cursor_image.hpp"

#include <algorithm>
#include <cstdio>

namespace fjarr::desktop::mutter {

std::vector<std::uint8_t> to_straight_rgba(const std::uint8_t* pixels, int width, int height, int stride, bool bgra) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(width) * height * 4);
    bool premultiplied = true;
    for (int y = 0; y < height && premultiplied; y++)
        for (int x = 0; x < width; x++) {
            const std::uint8_t* p = pixels + static_cast<std::size_t>(y) * stride + x * 4;
            if (p[0] > p[3] || p[1] > p[3] || p[2] > p[3]) {
                premultiplied = false;
                break;
            }
        }
    for (int y = 0; y < height; y++)
        for (int x = 0; x < width; x++) {
            const std::uint8_t* p = pixels + static_cast<std::size_t>(y) * stride + x * 4;
            std::uint8_t* o = out.data() + (static_cast<std::size_t>(y) * width + x) * 4;
            const std::uint8_t r = bgra ? p[2] : p[0], g = p[1], b = bgra ? p[0] : p[2], a = p[3];
            auto un = [&](std::uint8_t c) { return premultiplied && a > 0 ? static_cast<std::uint8_t>(std::min(255, (c * 255 + a / 2) / a)) : c; };
            o[0] = un(r);
            o[1] = un(g);
            o[2] = un(b);
            o[3] = a;
        }
    return out;
}

std::string shape_id(const CursorImage& c) {
    if (!c.visible) return "hidden";
    std::uint64_t h = 1469598103934665603ULL;
    auto mix = [&](std::uint64_t v) {
        for (int i = 0; i < 8; i++) {
            h ^= (v >> (i * 8)) & 0xFF;
            h *= 1099511628211ULL;
        }
    };
    mix(static_cast<std::uint64_t>(c.width) << 32 | static_cast<std::uint32_t>(c.height));
    mix(static_cast<std::uint64_t>(static_cast<std::uint32_t>(c.hot_x)) << 32 | static_cast<std::uint32_t>(c.hot_y));
    for (const auto b : c.rgba) {
        h ^= b;
        h *= 1099511628211ULL;
    }
    char buf[17];
    std::snprintf(buf, sizeof buf, "%016llx", static_cast<unsigned long long>(h));
    return buf;
}

// spec: docs/22-remote-desktop-client.md#cursor-strategy (a hidden pointer is a hidden shape)
std::optional<CursorImage> ReaderCursor::emit(CursorImage img) {
    img.shape_id = shape_id(img);
    if (img.shape_id == last_shape_) return std::nullopt;
    last_shape_ = img.shape_id;
    if (img.visible) last_visible_ = img;
    return img;
}

std::optional<CursorImage> ReaderCursor::invalid_id() {
    static const std::string hidden = [] {
        CursorImage none;
        none.visible = false;
        return shape_id(none);
    }();
    if (last_shape_ == hidden) return std::nullopt; // hidden already, by whatever
    hidden_by_invalid_id_ = true;
    CursorImage img;
    img.visible = false;
    img.invalid_id = true;
    return emit(std::move(img));
}

std::optional<CursorImage> ReaderCursor::bitmap(CursorImage img) {
    hidden_by_invalid_id_ = false; // what mutter draws now, an empty shape included
    img.invalid_id = false;
    return emit(std::move(img));
}

std::optional<CursorImage> ReaderCursor::no_bitmap() {
    // Only after an invalid id: mutter's own empty shape stays in force until it sends another.
    if (!hidden_by_invalid_id_ || !last_visible_) return std::nullopt;
    hidden_by_invalid_id_ = false;
    return emit(*last_visible_);
}

std::optional<CursorImage> CursorArbiter::show(const CursorImage& img) {
    if (shown_ && shown_->shape_id == img.shape_id) return std::nullopt; // another monitor's reader, same shape
    shown_ = img;
    return img;
}

std::optional<CursorImage> CursorArbiter::on_shape(MonitorId monitor, const CursorImage& img) {
    by_monitor_[monitor] = img;
    // An invalid id from a monitor the pointer is not on says only that it is elsewhere.
    if (!img.visible && img.invalid_id && pointer_ && *pointer_ != monitor) return std::nullopt;
    return show(img);
}

std::optional<CursorImage> CursorArbiter::on_position(MonitorId monitor) {
    const bool arrived = !pointer_ || *pointer_ != monitor;
    pointer_ = monitor;
    if (!arrived) return std::nullopt;
    auto it = by_monitor_.find(monitor);
    // A hidden shape from an invalid id may have been sent while the pointer was elsewhere: the
    // pointer is visibly here now, so the current shape stays until this reader sends another.
    if (it == by_monitor_.end() || it->second.invalid_id) return std::nullopt;
    return show(it->second);
}

void CursorArbiter::forget(MonitorId monitor) {
    by_monitor_.erase(monitor);
    if (pointer_ && *pointer_ == monitor) pointer_.reset();
}

} // namespace fjarr::desktop::mutter
