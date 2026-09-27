/**
 * A desktop backend module that exists only to prove the seam (ADR-0021): built as a separate
 * .so, discovered and dlopened like a real one, with no X11 or Wayland anywhere near it. Its
 * behaviour is controlled by environment variables so one binary can play every case the loader
 * has to handle.
 */
#include <cstdlib>
#include <cstring>

#include "desktop/module.hpp"

using namespace fjarr;

namespace {

class StubBackend final : public DesktopBackend {
  public:
    Features features() override { return {}; }
    std::vector<Monitor> monitors() override { return {}; }
    void on_monitors_changed(std::function<void(std::vector<Monitor>)>) override {}
    void on_capture_lost(std::function<void(MonitorId, CaptureLost)>) override {}
    CaptureSource* start_capture(MonitorId, CaptureOptions) override { return nullptr; }
    void stop_capture(MonitorId) override {}
    MonitorId create_virtual_monitor(int, int) override { return INVALID_MONITOR; }
    void destroy_virtual_monitor(MonitorId) override {}
    CaptureSource* start_audio_capture() override { return nullptr; }
    void stop_audio_capture() override {}
    void on_cursor_shape(std::function<void(const CursorShape&)>) override {}
    void pointer_motion(MonitorId, double, double) override {}
    void pointer_button(MouseButton, bool) override {}
    void pointer_wheel(double, double) override {}
    void key(LinuxKeycode, bool) override {}
    ClipboardHandle* clipboard() override { return nullptr; }
    void release_all_input() override {}
};

const char* probe() {
    const char* why = std::getenv("FJARR_STUB_UNUSABLE");
    return (why && *why) ? why : nullptr;
}

DesktopBackend* create() {
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
