// spec: docs/23-agent-core-architecture.md#desktop-x11 (Monitors)
#include "desktop/edid.hpp"

#include <cstdio>

namespace fjarr::desktop {

MonitorKey edid_key(const std::uint8_t* e, std::size_t len, const std::string& connector) {
    MonitorKey k;
    k.connector = connector;
    if (!e || len < 128 || e[0] != 0x00 || e[1] != 0xff || e[7] != 0x00) return k;
    // Bytes 8-9: three 5-bit letters, 'A' = 1.
    const unsigned v = (e[8] << 8) | e[9];
    k.vendor = {char('A' - 1 + ((v >> 10) & 31)), char('A' - 1 + ((v >> 5) & 31)), char('A' - 1 + (v & 31))};
    // Four 18-byte descriptors from byte 54; a display descriptor starts 00 00 00 <tag> 00.
    for (std::size_t d = 54; d + 18 <= 126; d += 18) {
        if (e[d] || e[d + 1] || e[d + 2]) continue; // a detailed timing, not a descriptor
        std::string text(reinterpret_cast<const char*>(e + d + 5), 13);
        text = text.substr(0, text.find('\n'));
        while (!text.empty() && text.back() == ' ') text.pop_back();
        if (e[d + 3] == 0xfc) k.model = text;
        if (e[d + 3] == 0xff) k.serial = text;
    }
    if (k.serial.empty()) {
        const unsigned long n = e[12] | (e[13] << 8) | (e[14] << 16) | (static_cast<unsigned long>(e[15]) << 24);
        if (n) {
            char b[16];
            std::snprintf(b, sizeof b, "0x%08lx", n);
            k.serial = b;
        }
    }
    return k;
}

} // namespace fjarr::desktop
