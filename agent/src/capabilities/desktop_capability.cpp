// spec: docs/06-capabilities.md · docs/adr/0021-desktop-backends-as-runtime-modules.md
#include <fjarr/desktop_capability.hpp>

#include <fjarr/errors.hpp>
#include <fjarr/session_context.hpp>

#include "core/log.hpp"
#include "desktop/module_loader.hpp"

namespace fjarr {
namespace {
/// Where packages install their modules (ADR-0021). Overridable so the dev stack and the tests
/// can point somewhere writable without pretending to be an installed system.
constexpr const char* DEFAULT_MODULE_DIR = "/usr/lib/fjarr/desktop";
} // namespace

struct DesktopCapability::Impl {
    bool enabled = true;
    std::string backend = "auto";
    std::string module_dir = DEFAULT_MODULE_DIR;
    desktop::ModuleLoader loader;
    std::string chosen;      // the display server serving us, empty when none
    std::string unavailable; // why not, when chosen is empty
};

DesktopCapability::DesktopCapability() : impl_(std::make_unique<Impl>()) {}
DesktopCapability::~DesktopCapability() = default;

CapabilityManifest DesktopCapability::manifest() const {
    CapabilityManifest m;
    m.name = "fjarr.desktop";
    m.version = {0, 1, 0};
    m.channels = {{ChannelClass::Control}, {ChannelClass::Realtime}};
    m.consumers.peer = true;
    m.input_bearing = true; // docs/10: pointer and keyboard take the ownership lease
    m.config_schema = nlohmann::json{
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"enabled", {{"type", "boolean"}}},
          {"backend", {{"type", "string"}, {"enum", {"auto", "x11", "wayland"}}}},
          {"module_dir", {{"type", "string"}, {"description", "where backend modules are installed (ADR-0021)"}}}}}};
    return m;
}

void DesktopCapability::configure(const nlohmann::json& config, const SourceFactory&) {
    impl_->enabled = config.value("enabled", true);
    impl_->backend = config.value("backend", std::string{"auto"});
    impl_->module_dir = config.value("module_dir", std::string{DEFAULT_MODULE_DIR});
    if (const char* env = std::getenv("FJARR_DESKTOP_MODULE_DIR"); env && *env) impl_->module_dir = env;
    impl_->chosen.clear();
    impl_->unavailable.clear();
    if (!impl_->enabled) {
        impl_->unavailable = "disabled in configuration";
        return;
    }
    impl_->loader.scan(impl_->module_dir);
    const desktop::Found* f = impl_->loader.select(impl_->backend);
    if (!f) {
        // Not a startup error: a robot with no desktop is the normal case, and every other
        // capability must keep running (ADR-0021).
        impl_->unavailable = impl_->loader.unavailable_reason(impl_->backend);
        log::info("desktop", "no backend", {{"backend", impl_->backend}, {"reason", impl_->unavailable}, {"module_dir", impl_->module_dir}});
        return;
    }
    impl_->chosen = f->display_server;
    log::info("desktop", "backend available", {{"display_server", f->display_server}, {"package", f->package}, {"path", f->path}});
}

void DesktopCapability::session_attached(SessionContext&, const nlohmann::json&) {}
void DesktopCapability::session_detached(const SessionId&, DetachReason, std::string_view) {}

void DesktopCapability::on_message(SessionContext& ctx, const Envelope& msg) {
    if (msg.kind != "request") return;
    // M3 brings capture and input. Until then the honest answer to any request is the same one
    // `/sources` gives, with the reason — never a silent no-op that looks like it worked.
    if (!impl_->chosen.empty()) {
        ctx.fail(msg, error_codes::unavailable, "remote desktop arrives in M3; the " + impl_->chosen + " backend is loaded and idle");
        return;
    }
    ctx.fail(msg, error_codes::unavailable, impl_->unavailable);
}

std::vector<Capability::ConfiguredSource> DesktopCapability::configured_sources() const {
    ConfiguredSource s;
    s.track_id = "desktop";
    s.label = "Remote desktop";
    s.identity = impl_->chosen.empty() ? "none" : impl_->chosen;
    s.available = !impl_->chosen.empty();
    s.reason = impl_->unavailable;
    return {s};
}

void DesktopCapability::shutdown() {}

} // namespace fjarr
