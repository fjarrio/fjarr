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

} // namespace fjarr::desktop::mutter
