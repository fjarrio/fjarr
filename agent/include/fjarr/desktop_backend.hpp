#pragma once
// Desktop capture/injection backends behind one interface, designed
// Wayland-first (normalized per-monitor coordinates — the mapping_id model;
// X11 implements INTO this shape).
// spec: docs/09-interfaces.md#the-desktop-backend-interface-wayland-first-shape
// spec: docs/07-desktop-backends.md — implementations chosen by ADR-0006.
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fjarr/video_source.hpp>

namespace fjarr {

using MonitorId = std::uint32_t;
inline constexpr MonitorId INVALID_MONITOR = 0;
using LinuxKeycode = std::uint16_t; // KEY_* from <linux/input-event-codes.h>

/// Who a monitor is, from its EDID. Connector names are NOT identities: a replug of a DP MST
/// chain renamed DP-4/6/8 to DP-5/9/11 on the spike machine (ADR-0006).
struct MonitorIdentity {
    std::string vendor;  // EDID manufacturer id, "DEL"
    std::string model;   // EDID product name, "DELL U2422H"
    std::string serial;  // EDID serial string; empty when the EDID has none
};

enum class MonitorKind : std::uint8_t {
    Physical, // a connector, including one forced on from the kernel command line
    Virtual,  // created by the backend (create_virtual_monitor); lives for the session only
};

struct Monitor {
    MonitorId id = INVALID_MONITOR; // backend handle, stable while `identity` is present
    /// The wire identity, docs/08#track-manifest: the vendor-model-serial slug; the connector
    /// name when there is no serial or the identity collides; virtual-<n> for a virtual monitor.
    /// Desktop track_ids derive from it; never from index or connector.
    std::string wire_id;
    MonitorIdentity identity;
    MonitorKind kind = MonitorKind::Physical;
    std::string connector; // informational only: the current connector name, changes on replug
    std::string label;     // EDID model when known
    bool primary = false;
    int x = 0, y = 0;      // placement in the virtual screen
    int width = 0, height = 0;
    double scale = 1.0;
};

enum class MouseButton : std::uint8_t { Left, Middle, Right, Back, Forward };

/// What this backend can do beyond capture and input. The capability reports each to the client.
struct Features {
    bool local_cursor = false;     // on_cursor_shape delivers; captures can leave the cursor out
    bool virtual_monitors = false; // create_virtual_monitor works
    bool desktop_audio = false;    // start_audio_capture works
    bool clipboard = false;        // clipboard() is usable
};

/// Why a capture ended without being asked to.
enum class CaptureLost : std::uint8_t {
    MonitorGone,   // its monitor was unplugged (mutter says nothing: the backend must detect it)
    SourceStopped, // the display server or portal ended the stream
    SessionEnded,  // the desktop session itself ended
};

struct CaptureOptions {
    /// false = local-cursor mode: the cursor is left out of the video and its shape arrives
    /// through on_cursor_shape. spec: docs/22-remote-desktop-client.md#cursor-strategy
    bool cursor_in_video = true;
};

/// One cursor image, delivered whenever the shape changes.
struct CursorShape {
    std::string shape_id;  // names the shape by its content; the client caches images by it (docs/08 `cursor`)
    bool visible = true;   // false: no cursor is shown
    int width = 0, height = 0;
    int hot_x = 0, hot_y = 0;
    std::vector<std::uint8_t> rgba; // width*height*4, straight alpha
};

/// The robot's clipboard (docs/08 clipboard-*, M3 3.5). Types are Fjarr's ("text/plain" = UTF-8
/// text); the backend maps them to the compositor's names. Every callback runs on the core loop.
/// spec: docs/09-interfaces.md (ClipboardHandle) · docs/23-agent-core-architecture.md#desktop-helper-protocol
class ClipboardHandle {
  public:
    virtual ~ClipboardHandle() = default;
    /// The robot copied something (not what write() put there): what it can be read as. Empty = cleared.
    virtual void on_changed(std::function<void(std::vector<std::string> types)> callback) = 0;
    /// The current content as `type`, at most `max_bytes`; nullopt and a reason otherwise.
    virtual void read(const std::string& type, std::size_t max_bytes, std::function<void(std::optional<std::string> bytes, std::string error)> done) = 0;
    /// The robot's clipboard holds `bytes` as `type` once done(true) runs.
    virtual void write(const std::string& type, std::string bytes, std::function<void(bool ok, std::string error)> done) = 0;
};

class DesktopBackend {
  public:
    virtual ~DesktopBackend() = default;

    virtual Features features() = 0;

    virtual std::vector<Monitor> monitors() = 0;

    /// Hot-plug: called with the full current set on connect/disconnect/mode
    /// change. The desktop capability diffs it BY IDENTITY and updates its track
    /// set through the session context. // spec: docs/08-protocol.md#renegotiation
    virtual void on_monitors_changed(
        std::function<void(std::vector<Monitor>)> callback) = 0;

    /// A capture ended without being asked to. The backend detects silent ends itself.
    virtual void on_capture_lost(std::function<void(MonitorId, CaptureLost)> callback) = 0;

    /// Returns at once and never waits for a first frame: capture may be variable-rate, and a
    /// still screen produces no frames at all. The backend provokes a first frame where it can;
    /// the media plane repeats the last frame to the encoder.
    /// The capture as a track's source: unavailable until the display server has handed the stream
    /// over, then available (on_availability_changed), so the capability declares it at once and the
    /// core adds it when it is ready. Null when the monitor is unknown.
    virtual std::shared_ptr<VideoSource> start_capture(MonitorId monitor, CaptureOptions options) = 0;
    /// MUST NOT disturb captures of other monitors.
    virtual void stop_capture(MonitorId monitor) = 0;

    /// A monitor that exists only for this session: headless robots, or sized to an operator's
    /// window. The size must be given (unasked, mutter made one 1x1). INVALID_MONITOR when
    /// unsupported (features().virtual_monitors).
    virtual MonitorId create_virtual_monitor(int width, int height) = 0;
    virtual void destroy_virtual_monitor(MonitorId monitor) = 0;

    /// What the robot's speakers play: the default sink's monitor. Null when unsupported.
    virtual std::shared_ptr<VideoSource> start_audio_capture() = 0;
    virtual void stop_audio_capture() = 0;

    /// Local-cursor mode: every shape change, for captures started with cursor_in_video=false.
    virtual void on_cursor_shape(std::function<void(const CursorShape&)> callback) = 0;
    /// Where the pointer is, normalized within one monitor, while it is on a captured one
    /// (docs/08 `cursor-position`). spec: docs/22-remote-desktop-client.md#cursor-strategy
    virtual void on_cursor_position(std::function<void(MonitorId, double nx, double ny)> callback) = 0;

    /// Absolute pointer position, normalized [0,1] within one monitor.
    /// spec: docs/08-protocol.md#input-events-fjarrdesktop
    virtual void pointer_motion(MonitorId monitor, double nx, double ny) = 0;
    virtual void pointer_button(MouseButton button, bool down) = 0;
    virtual void pointer_wheel(double dx, double dy) = 0;

    virtual void key(LinuxKeycode code, bool down) = 0;
    /// Types `utf8` through the robot's active keymap: each character as the key and modifiers that
    /// produce it there (docs/08 `text`; no backend has a Unicode path). Returns the characters no
    /// key produces, and then types nothing. Sets `error` when there is no input to type with.
    /// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol
    virtual std::vector<std::string> type_text(const std::string& utf8, std::string& error) = 0;

    /// MUST be called on every session end — no stuck modifiers, ever.
    /// spec: docs/15-testing-strategy.md#safety-behaviors
    virtual void release_all_input() = 0;

    /// Null when features().clipboard is false.
    virtual ClipboardHandle* clipboard() = 0;
};

} // namespace fjarr
