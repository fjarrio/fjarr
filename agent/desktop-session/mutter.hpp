#pragma once
// mutter's D-Bus interfaces, as fjarr-desktop-session uses them: the monitor layout
// (org.gnome.Mutter.DisplayConfig) and one RemoteDesktop session with a linked ScreenCast
// (org.gnome.Mutter.RemoteDesktop / .ScreenCast). Everything that needs the desktop user's rights
// lives here; nothing here touches a frame or an input event.
// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol · ADR-0028 (addendum 2026-09-30)
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gio/gio.h>

namespace fjarr::desktop::helper {

struct MonitorInfo {
    std::string connector, name, vendor, product, serial;
    int x = 0, y = 0, width = 0, height = 0;
    double scale = 1.0;
    bool primary = false;
    bool is_virtual = false;
};

/// The current layout: every monitor that is part of a logical monitor, with its current mode.
std::vector<MonitorInfo> current_monitors(GDBusConnection* bus, std::string* error);

/// Make the logical monitor holding `connector` the primary one, with every position, mode, scale
/// and transform kept, persistently (the account's monitors.xml remembers it). False with `error`.
/// spec: docs/26-robot-install-and-drivers.md#ghost-screens (a ghost is never primary beside a real monitor)
bool make_primary(GDBusConnection* bus, const std::string& connector, std::string* error);

/// Calls `changed` on every MonitorsChanged. Returns the subscription id.
guint watch_monitors(GDBusConnection* bus, std::function<void()> changed);

struct StreamInfo {
    std::uint32_t node = 0;
    int x = 0, y = 0, width = 0, height = 0;
};

/// The input session: one RemoteDesktop session with no linked ScreenCast, so its EIS absolute
/// pointer covers the whole layout and follows hot-plug (docs/23#desktop-helper-protocol).
class InputSession {
  public:
    explicit InputSession(GDBusConnection* bus);
    ~InputSession();
    InputSession(const InputSession&) = delete;
    InputSession& operator=(const InputSession&) = delete;
    /// mutter's EIS socket, starting the session first if needed: a descriptor the caller owns, or -1.
    int connect_eis(std::string* error);
    /// mutter ended the session (Closed).
    void on_closed(std::function<void()> cb) { closed_ = std::move(cb); }
    void stop();

    // The clipboard belongs to the RemoteDesktop session, and works on this unlinked one
    // (spike 2026-10-03; docs/23#desktop-helper-protocol, Clipboard).
    /// EnableClipboard, starting the session first if needed; once per session.
    bool enable_clipboard(std::string* error);
    /// SelectionOwnerChanged: who owns the clipboard now (`ours` = this session) and its mime types.
    void on_selection_owner(std::function<void(bool ours, std::vector<std::string> mime_types)> cb) { owner_ = std::move(cb); }
    /// SelectionTransfer: an application pastes what this session set; answer with selection_write.
    void on_selection_transfer(std::function<void(std::string mime_type, std::uint32_t serial)> cb) { transfer_ = std::move(cb); }
    /// SelectionRead: a descriptor to read the robot's clipboard as `mime_type` from (non-blocking), or -1.
    int selection_read(const std::string& mime_type, std::string* error);
    /// SetSelection: this session owns the clipboard, offering `mime_types`.
    bool set_selection(const std::vector<std::string>& mime_types, std::string* error);
    /// SelectionWrite: a descriptor to write transfer `serial`'s bytes to (non-blocking), or -1.
    int selection_write(std::uint32_t serial, std::string* error);
    void selection_write_done(std::uint32_t serial, bool ok);

  private:
    bool ensure_started(std::string* error);
    int call_for_fd(const char* method, GVariant* params, std::string* error);
    GDBusConnection* bus_;
    std::string path_;
    guint closed_sub_ = 0;
    std::vector<guint> clipboard_subs_;
    bool clipboard_enabled_ = false;
    std::function<void()> closed_;
    std::function<void(bool, std::vector<std::string>)> owner_;
    std::function<void(std::string, std::uint32_t)> transfer_;
};

/// One monitor's capture: a ScreenCast session of its own, so it starts and stops without touching
/// any other (docs/23#desktop-helper-protocol). Stopped when destroyed.
class Capture {
  public:
    using Recorded = std::function<void(bool ok, StreamInfo info, std::string error)>;
    /// Record `connector` ("" = the primary). `cursor`: "hidden" | "embedded" | "metadata". `done`
    /// runs once, from the main loop, when its PipeWire node is known, or at once on failure (and
    /// then null is returned).
    static std::unique_ptr<Capture> start(GDBusConnection* bus, const std::string& connector, const std::string& cursor, Recorded done);
    ~Capture();
    Capture(const Capture&) = delete;
    Capture& operator=(const Capture&) = delete;
    /// mutter ended this capture's session (Closed).
    void on_closed(std::function<void()> cb) { closed_ = std::move(cb); }

  private:
    explicit Capture(GDBusConnection* bus);
    GDBusConnection* bus_;
    std::string path_, stream_;
    Recorded done_;
    std::vector<guint> subscriptions_;
    std::function<void()> closed_;
};

} // namespace fjarr::desktop::helper
