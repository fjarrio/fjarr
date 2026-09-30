#pragma once
// mutter's D-Bus interfaces, as fjarr-desktop-session uses them: the monitor layout
// (org.gnome.Mutter.DisplayConfig) and one RemoteDesktop session with a linked ScreenCast
// (org.gnome.Mutter.RemoteDesktop / .ScreenCast). Everything that needs the desktop user's rights
// lives here; nothing here touches a frame or an input event.
// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol · ADR-0028 (addendum 2026-09-30)
#include <cstdint>
#include <functional>
#include <map>
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

/// Calls `changed` on every MonitorsChanged. Returns the subscription id.
guint watch_monitors(GDBusConnection* bus, std::function<void()> changed);

struct StreamInfo {
    std::uint32_t node = 0;
    int x = 0, y = 0, width = 0, height = 0;
};

/// One RemoteDesktop session with a linked ScreenCast session. It starts on the first stream, and
/// mutter closes both when it stops.
class RemoteDesktop {
  public:
    explicit RemoteDesktop(GDBusConnection* bus);
    ~RemoteDesktop();
    RemoteDesktop(const RemoteDesktop&) = delete;
    RemoteDesktop& operator=(const RemoteDesktop&) = delete;

    using Recorded = std::function<void(bool ok, StreamInfo info, std::string error)>;
    /// Record a monitor ("" = the primary). `cursor`: "hidden" | "embedded" | "metadata".
    /// `done` runs once, from the main loop, when its PipeWire node is known or it failed.
    void record(const std::string& connector, const std::string& cursor, Recorded done);
    /// mutter's EIS socket for this session (it must be started): a descriptor the caller owns, or -1.
    int connect_eis(std::string* error);
    /// mutter ended the session (Closed): every stream is gone.
    void on_closed(std::function<void()> cb) { closed_ = std::move(cb); }
    bool started() const { return started_; }
    void stop();

  private:
    bool ensure_sessions(std::string* error);
    GDBusConnection* bus_;
    std::string rd_path_, sc_path_;
    bool started_ = false;
    std::vector<guint> subscriptions_;
    std::function<void()> closed_;
    struct Pending {
        std::string stream_path;
        Recorded done;
    };
    std::map<std::string, Pending> pending_; // by stream path
};

} // namespace fjarr::desktop::helper
