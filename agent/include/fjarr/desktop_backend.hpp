#pragma once
// Desktop capture/injection backends behind one interface, designed
// Wayland-first (normalized per-monitor coordinates — the mapping_id model;
// X11 implements INTO this shape).
// spec: docs/09-interfaces.md#the-desktop-backend-interface-wayland-first-shape
// spec: docs/07-desktop-backends.md — implementations chosen by ADR-0006.
#include <cstdint>
#include <string>
#include <vector>

namespace fjarr {

using MonitorId = std::uint32_t;
using LinuxKeycode = std::uint16_t; // KEY_* from <linux/input-event-codes.h>

struct Monitor {
    MonitorId id = 0;
    std::string label;
    int width = 0, height = 0;
    double scale = 1.0;
};

enum class MouseButton : std::uint8_t { Left, Middle, Right, Back, Forward };

/// Opaque handle to a capture source; yields a GStreamer element/bin in the
/// M1 core. Kept opaque here so public headers stay GStreamer-free.
class CaptureSource;
class ClipboardHandle;

class DesktopBackend {
  public:
    virtual ~DesktopBackend() = default;

    virtual std::vector<Monitor> monitors() = 0;

    virtual CaptureSource* start_capture(MonitorId monitor) = 0;
    virtual void stop_capture(MonitorId monitor) = 0;

    /// Absolute pointer position, normalized [0,1] within one monitor.
    /// spec: docs/08-protocol.md#input-events-fjarrdesktop
    virtual void pointer_motion(MonitorId monitor, double nx, double ny) = 0;
    virtual void pointer_button(MouseButton button, bool down) = 0;
    virtual void pointer_wheel(double dx, double dy) = 0;

    virtual void key(LinuxKeycode code, bool down) = 0;

    /// MUST be called on every session end — no stuck modifiers, ever.
    /// spec: docs/15-testing-strategy.md#safety-behaviors
    virtual void release_all_input() = 0;

    virtual ClipboardHandle* clipboard() = 0;
};

} // namespace fjarr
