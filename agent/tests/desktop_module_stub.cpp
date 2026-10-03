/**
 * A desktop backend module that exists only to prove the seam (ADR-0021): built as a separate
 * .so, discovered and dlopened like a real one, with no X11 or Wayland anywhere near it. Its
 * behaviour is controlled by environment variables so one binary can play every case the loader
 * has to handle.
 */
#include <map>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <optional>

#include "desktop/module.hpp"

using namespace fjarr;

namespace {

/// FJARR_STUB_RECORD=<file>: the stub has one monitor, virtual-1, and writes every input call
/// to the file, one line each — how the capability's routing is tested without a desktop.
void record(const std::string& line) {
    if (const char* f = std::getenv("FJARR_STUB_RECORD"); f && *f) std::ofstream(f, std::ios::app) << line << "\n";
}

/// A capture, available until the robot stops sharing (fjarr_stub_stop_sharing, below).
class StubSource final : public VideoSource {
  public:
    SourceInfo describe() const override { return {}; }
    GstBin* create_bin() override { return nullptr; }
    bool available() const override { return available_; }
    void on_availability_changed(std::function<void(bool)> cb) override { cb_ = std::move(cb); }
    void stop() {
        available_ = false;
        if (cb_) cb_(false);
    }

  private:
    bool available_ = true;
    std::function<void(bool)> cb_;
};
/// Every capture started, by monitor, and who hears that one ended.
std::map<MonitorId, std::weak_ptr<StubSource>> g_sources;
std::function<void(MonitorId, CaptureLost)> g_lost_cb;

/// Monitors the test plugs and unplugs through fjarr_stub_plug (below).
std::vector<Monitor> g_monitors;
std::function<void(std::vector<Monitor>)> g_monitors_cb;
/// The robot's cursor, which the test moves and reshapes through fjarr_stub_cursor (below).
std::function<void(const CursorShape&)> g_shape_cb;
std::function<void(MonitorId, double, double)> g_position_cb;
/// The robot's clipboard, which the test fills through fjarr_stub_copy (below).
std::string g_clipboard;
std::function<void(std::vector<std::string>)> g_clipboard_cb;

class StubBackend final : public DesktopBackend, public ClipboardHandle {
  public:
    Features features() override {
        Features f;
        f.clipboard = std::getenv("FJARR_STUB_RECORD") != nullptr;
        f.virtual_monitors = f.clipboard;
        return f;
    }
    static Monitor default_monitor() {
        Monitor m;
        m.id = 7;
        m.wire_id = "virtual-1";
        m.width = 1280;
        m.height = 720;
        m.primary = true;
        return m;
    }
    std::vector<Monitor> monitors() override {
        if (!std::getenv("FJARR_STUB_RECORD")) return {};
        if (!g_monitors.empty()) return g_monitors;
        return {default_monitor()};
    }
    void on_monitors_changed(std::function<void(std::vector<Monitor>)> cb) override { g_monitors_cb = std::move(cb); }
    void on_capture_lost(std::function<void(MonitorId, CaptureLost)> cb) override { g_lost_cb = std::move(cb); }
    std::shared_ptr<VideoSource> start_capture(MonitorId m, CaptureOptions) override {
        if (!std::getenv("FJARR_STUB_RECORD")) return nullptr;
        record("capture " + std::to_string(m));
        auto s = std::make_shared<StubSource>();
        g_sources[m] = s;
        return s;
    }
    void stop_capture(MonitorId m) override { record("stop " + std::to_string(m)); }
    /// A virtual monitor joins the layout at once, as mutter's does once its stream has a consumer.
    MonitorId create_virtual_monitor(int width, int height) override {
        if (!std::getenv("FJARR_STUB_RECORD")) return INVALID_MONITOR;
        record("virtual " + std::to_string(width) + "x" + std::to_string(height));
        if (g_monitors.empty()) g_monitors.push_back(default_monitor());
        Monitor m;
        m.id = next_virtual_++;
        m.wire_id = "virtual-" + std::to_string(m.id);
        m.kind = MonitorKind::Virtual;
        m.x = 1280;
        m.width = width;
        m.height = height;
        g_monitors.push_back(m);
        if (g_monitors_cb) g_monitors_cb(g_monitors);
        return m.id;
    }
    void destroy_virtual_monitor(MonitorId id) override {
        record("destroy " + std::to_string(id));
        std::erase_if(g_monitors, [id](const Monitor& m) { return m.id == id; });
        if (g_monitors_cb) g_monitors_cb(g_monitors);
    }
    MonitorId next_virtual_ = 100;
    std::shared_ptr<VideoSource> start_audio_capture() override { return nullptr; }
    void stop_audio_capture() override {}
    void on_cursor_shape(std::function<void(const CursorShape&)> cb) override { g_shape_cb = std::move(cb); }
    void on_cursor_position(std::function<void(MonitorId, double, double)> cb) override { g_position_cb = std::move(cb); }
    void pointer_motion(MonitorId m, double x, double y) override { record("pointer " + std::to_string(m) + " " + std::to_string(x) + " " + std::to_string(y)); }
    void pointer_button(MouseButton b, bool down) override { record("button " + std::to_string(static_cast<int>(b)) + (down ? " down" : " up")); }
    void pointer_wheel(double dx, double dy) override { record("wheel " + std::to_string(dx) + " " + std::to_string(dy)); }
    void key(LinuxKeycode code, bool down) override { record("key " + std::to_string(code) + (down ? " down" : " up")); }
    /// Types ASCII only, as a US keymap would type letters; anything else is untypable.
    std::vector<std::string> type_text(const std::string& utf8, std::string& error) override {
        if (!std::getenv("FJARR_STUB_RECORD")) {
            error = "the stub types nothing";
            return {};
        }
        for (const char c : utf8)
            if (static_cast<unsigned char>(c) >= 0x80) return {"å"};
        record("text " + utf8);
        return {};
    }
    ClipboardHandle* clipboard() override { return std::getenv("FJARR_STUB_RECORD") ? this : nullptr; }
    void release_all_input() override { record("release-all"); }

    // The clipboard: reads answer what fjarr_stub_copy put there; writes are recorded.
    void on_changed(std::function<void(std::vector<std::string>)> cb) override { g_clipboard_cb = std::move(cb); }
    void read(const std::string& type, std::size_t max_bytes, std::function<void(std::optional<std::string>, std::string)> done) override {
        if (type != "text/plain") return done(std::nullopt, "no " + type);
        if (g_clipboard.size() > max_bytes) return done(std::nullopt, "too-large");
        done(g_clipboard, {});
    }
    void write(const std::string& type, std::string bytes, std::function<void(bool, std::string)> done) override {
        record("clipboard " + type + " " + bytes);
        done(true, {});
    }
};

const char* probe() {
    const char* why = std::getenv("FJARR_STUB_UNUSABLE");
    return (why && *why) ? why : nullptr;
}

DesktopBackend* create(const desktop::ModuleHost*) {
    if (std::getenv("FJARR_STUB_NO_BACKEND")) return nullptr;
    return new StubBackend();
}

const desktop::ModuleV1 MODULE{
    /*display_server=*/"stub",
    /*package=*/"fjarr-desktop-stub",
    /*probe=*/&probe,
    /*create=*/&create,
};

} // namespace

extern "C" const desktop::ModuleV1* fjarr_desktop_module_v1() { return &MODULE; }

/// The test's hand on the monitors: "wire:id:x:width:primary;…" (empty: none), then the change is
/// reported as a helper's `monitors` message would be.
/// The test's hand on the robot's cursor: a shape `id` (a 2x1 image; "" = no change, "hidden" = no
/// cursor), then the pointer at (x, y) on monitor 7 when x >= 0.
extern "C" void fjarr_stub_cursor(const char* id, double x, double y) {
    const std::string sid = id ? id : "";
    if (!sid.empty() && g_shape_cb) {
        CursorShape c;
        c.shape_id = sid;
        c.visible = sid != "hidden";
        if (c.visible) {
            c.width = 2;
            c.height = 1;
            c.hot_x = 1;
            c.rgba = {255, 0, 0, 255, 0, 0, 255, 128};
        }
        g_shape_cb(c);
    }
    if (x >= 0 && g_position_cb) g_position_cb(7, x, y);
}

/// The test's hand on GNOME's stop button: every capture ends, as mutter ends them (docs/23#desktop-sharing-stopped).
extern "C" void fjarr_stub_stop_sharing() {
    for (auto& [m, w] : g_sources)
        if (auto s = w.lock()) {
            s->stop();
            if (g_lost_cb) g_lost_cb(m, CaptureLost::SharingStopped);
        }
}

/// The test's hand on one capture ending by itself, as an unplugged monitor's does before the layout says so.
extern "C" void fjarr_stub_lose(unsigned monitor) {
    if (auto s = g_sources[static_cast<MonitorId>(monitor)].lock()) s->stop();
    if (g_lost_cb) g_lost_cb(static_cast<MonitorId>(monitor), CaptureLost::SourceStopped);
}

/// The test's hand on the robot's clipboard: the robot copies `text` ("" clears it: no types).
extern "C" void fjarr_stub_copy(const char* text) {
    g_clipboard = text ? text : "";
    if (g_clipboard_cb) g_clipboard_cb(g_clipboard.empty() ? std::vector<std::string>{} : std::vector<std::string>{"text/plain"});
}

extern "C" void fjarr_stub_plug(const char* spec) {
    g_monitors.clear();
    std::string s = spec ? spec : "";
    for (std::size_t at = 0; at < s.size();) {
        const auto end = s.find(';', at) == std::string::npos ? s.size() : s.find(';', at);
        const std::string item = s.substr(at, end - at);
        at = end + 1;
        if (item.empty()) continue;
        Monitor m;
        std::size_t p = 0;
        auto next = [&] {
            const auto q = item.find(':', p);
            std::string f = item.substr(p, q == std::string::npos ? std::string::npos : q - p);
            p = q == std::string::npos ? item.size() : q + 1;
            return f;
        };
        m.wire_id = next();
        m.id = static_cast<MonitorId>(std::stoul(next()));
        m.x = std::stoi(next());
        m.width = std::stoi(next());
        m.height = 720;
        m.primary = next() == "1";
        m.cursor_in_video = next() == "e"; // an optional sixth field: this capture fell back to embedded
        m.connector = "Meta-" + std::to_string(m.id);
        g_monitors.push_back(m);
    }
    if (g_monitors_cb) g_monitors_cb(g_monitors);
}
