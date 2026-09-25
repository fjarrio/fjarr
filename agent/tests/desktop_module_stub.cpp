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
    std::vector<Monitor> monitors() override { return {}; }
    void on_monitors_changed(std::function<void(std::vector<Monitor>)>) override {}
    CaptureSource* start_capture(MonitorId) override { return nullptr; }
    void stop_capture(MonitorId) override {}
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
