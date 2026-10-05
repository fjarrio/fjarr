#pragma once
// The X11 kiosk's output layout (docs/23#desktop-x11, fjarr-x11-session; ADR-0006): a bare X kiosk lays
// out nothing and never reacts to a hot-plug, so the session program does. Pure, so it is unit-tested
// without an X server: outputs in, what to switch off and where each connected output goes, out.
#include <string>
#include <vector>

namespace fjarr::desktop::x11 {

struct Output {
    std::string name;      // "DisplayPort-4", "HDMI-1"
    bool connected = false;
    bool has_crtc = false; // it holds a CRTC now (switched on, or left on after an unplug)
    int x = 0, y = 0;      // where it is now, when it holds a CRTC
    int width = 0, height = 0;           // its current mode, when it holds a CRTC
    int preferred_w = 0, preferred_h = 0; // its preferred mode (0 when the server knows none)
    bool ghost = false;   // a ghost screen: its EDID's vendor is FJR (docs/26#ghost-screens)
    bool primary = false; // RandR's primary output now
};

struct Placement {
    std::string name;
    int x = 0, y = 0, width = 0, height = 0;
};

struct Plan {
    std::vector<std::string> off;  // disconnected outputs that still hold a CRTC
    std::vector<Placement> on;     // connected outputs, left to right
    int screen_w = 0, screen_h = 0; // the root that fits them
    bool changes = false;          // false: the outputs are laid out already
    std::string primary;           // the output to make primary; empty: the primary stays as it is
};

/// Connected outputs at their preferred mode, left to right in connector order (a number in a name is
/// compared as a number: DisplayPort-10 after DisplayPort-4), each position explicit; disconnected
/// outputs holding a CRTC switched off. A ghost is never primary while a real monitor is connected
/// (docs/26#ghost-screens): with no primary, or a ghost or unplugged one, the leftmost real monitor
/// becomes primary; a real monitor already primary stays so.
Plan plan(const std::vector<Output>& outputs);

} // namespace fjarr::desktop::x11
