/**
 * A desktop backend module that exists only to prove the seam (ADR-0021): built as a separate
 * .so, discovered and dlopened like a real one, with no X11 or Wayland anywhere near it. Its
 * behaviour is controlled by environment variables so one binary can play every case the loader
 * has to handle.
 */
#include <cstdlib>
#include <cstring>
#include <fstream>

#include "desktop/module.hpp"

using namespace fjarr;

namespace {

/// FJARR_STUB_RECORD=<file>: the stub has one monitor, virtual-1, and writes every input call
/// to the file, one line each — how the capability's routing is tested without a desktop.
void record(const std::string& line) {
    if (const char* f = std::getenv("FJARR_STUB_RECORD"); f && *f) std::ofstream(f, std::ios::app) << line << "\n";
}

/// A capture that is always available: what the capability's diffing needs, nothing more.
class StubSource final : public VideoSource {
  public:
    SourceInfo describe() const override { return {}; }
    GstBin* create_bin() override { return nullptr; }
    bool available() const override { return true; }
    void on_availability_changed(std::function<void(bool)>) override {}
};

/// Monitors the test plugs and unplugs through fjarr_stub_plug (below).
std::vector<Monitor> g_monitors;
std::function<void(std::vector<Monitor>)> g_monitors_cb;

class StubBackend final : public DesktopBackend {
  public:
    Features features() override { return {}; }
    std::vector<Monitor> monitors() override {
        if (!std::getenv("FJARR_STUB_RECORD")) return {};
        if (!g_monitors.empty()) return g_monitors;
        Monitor m;
        m.id = 7;
        m.wire_id = "virtual-1";
        m.width = 1280;
        m.height = 720;
        m.primary = true;
        return {m};
    }
    void on_monitors_changed(std::function<void(std::vector<Monitor>)> cb) override { g_monitors_cb = std::move(cb); }
    void on_capture_lost(std::function<void(MonitorId, CaptureLost)>) override {}
    std::shared_ptr<VideoSource> start_capture(MonitorId m, CaptureOptions) override {
        if (!std::getenv("FJARR_STUB_RECORD")) return nullptr;
        record("capture " + std::to_string(m));
        return std::make_shared<StubSource>();
    }
    void stop_capture(MonitorId m) override { record("stop " + std::to_string(m)); }
    MonitorId create_virtual_monitor(int, int) override { return INVALID_MONITOR; }
    void destroy_virtual_monitor(MonitorId) override {}
    std::shared_ptr<VideoSource> start_audio_capture() override { return nullptr; }
    void stop_audio_capture() override {}
    void on_cursor_shape(std::function<void(const CursorShape&)>) override {}
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
    ClipboardHandle* clipboard() override { return nullptr; }
    void release_all_input() override { record("release-all"); }
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
        m.connector = "Meta-" + std::to_string(m.id);
        g_monitors.push_back(m);
    }
    if (g_monitors_cb) g_monitors_cb(g_monitors);
}
