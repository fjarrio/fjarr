// spec: docs/06-capabilities.md · docs/adr/0021-desktop-backends-as-runtime-modules.md
#include <fjarr/desktop_capability.hpp>

#include <map>
#include <optional>

#include <fjarr/errors.hpp>
#include <fjarr/session_context.hpp>

#include "core/log.hpp"
#include "desktop/module_loader.hpp"
#include "media/sources.hpp"

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
    nlohmann::json helper = nlohmann::json::object(); // the backend's own config (the session helper's socket and account)
    std::unique_ptr<DesktopBackend> driver; // the live backend from the module
    std::map<SessionId, SessionContext*> sessions;
    // The primary monitor's track (M3 3.1; every monitor from 3.4).
    std::optional<Monitor> primary;
    std::shared_ptr<VideoSource> source;

    std::vector<TrackSpec> specs(bool available_only = false) const {
        if (!primary || !source || (available_only && !source->available())) return {};
        TrackSpec t;
        t.track_id = "desk-" + primary->wire_id; // docs/08#track-manifest: from the identity, never the connector
        t.label = primary->label.empty() ? primary->connector : primary->label;
        t.kind = TrackKind::Video;
        t.source = SourceRef{source, "src"};
        MonitorInfo mi;
        mi.id = primary->wire_id;
        mi.primary = primary->primary;
        mi.x = primary->x;
        mi.y = primary->y;
        mi.w = primary->width;
        mi.h = primary->height;
        mi.scale = primary->scale;
        mi.name = primary->label;
        t.monitor = mi;
        return {t};
    }
    void reoffer() {
        for (auto& [_, ctx] : sessions) ctx->update_tracks(specs(true));
    }
    /// The monitor set changed: capture the primary one, and re-offer if its track changed.
    void on_monitors(const std::vector<Monitor>& monitors) {
        const Monitor* p = nullptr;
        for (const auto& m : monitors)
            if (m.primary) p = &m;
        if (!p && !monitors.empty()) p = &monitors.front();
        if (primary && (!p || p->wire_id != primary->wire_id)) {
            driver->stop_capture(primary->id);
            source.reset();
            primary.reset();
        }
        if (p && !primary) {
            primary = *p;
            source = driver->start_capture(p->id, CaptureOptions{});
            if (source) {
                const std::string id = "desk-" + p->wire_id;
                source->on_availability_changed([this, id](bool now) {
                    log::info("desktop", now ? "track available" : "track unavailable", {{"track", id}});
                    reoffer();
                });
            }
        } else if (p && primary) {
            primary = *p; // same monitor, new geometry
        }
        reoffer();
    }
};

namespace {
/// Module log lines into the agent's structured log.
void module_log(int level, const char* component, const char* message) {
    switch (level) {
    case 0: log::debug(component, message); break;
    case 1: log::info(component, message); break;
    case 2: log::warn(component, message); break;
    default: log::error(component, message); break;
    }
}
} // namespace

DesktopCapability::DesktopCapability() : impl_(std::make_unique<Impl>()) {}
DesktopCapability::~DesktopCapability() = default;

CapabilityManifest DesktopCapability::manifest() const {
    CapabilityManifest m;
    m.name = "fjarr.desktop";
    m.version = {0, 1, 0};
    m.channels = {{ChannelClass::Control}, {ChannelClass::Realtime}};
    m.consumers.peer = true;
    m.input_bearing = true;
    m.control_domain = "desktop"; // docs/10: one pointer, one keyboard — one holder at a time
    // docs/08#input-events-fjarrdesktop. Not `release-all`: a viewer's window losing focus sends
    // it, and that must never claim a free desktop.
    m.control_inputs = {"pointer", "button", "wheel", "key", "key-combo", "text"};
    m.config_schema = nlohmann::json{
        {"type", "object"},
        {"additionalProperties", false},
        {"properties",
         {{"enabled", {{"type", "boolean"}}},
          {"backend", {{"type", "string"}, {"enum", {"auto", "x11", "wayland"}}}},
          {"module_dir", {{"type", "string"}, {"description", "where backend modules are installed (ADR-0021)"}}},
          {"helper",
           {{"type", "object"},
            {"description", "the session helper (ADR-0028): its socket, the desktop account it runs as, the socket's group"},
            {"additionalProperties", false},
            {"properties",
             {{"socket", {{"type", "string"}}},
              {"user", {{"type", "string"}}},
              {"uid", {{"type", "integer"}}},
              {"group", {{"type", "string"}}},
              {"keepalive_ms", {{"type", "integer"}, {"minimum", 20}, {"maximum", 1000}}}}}}}}}};
    return m;
}

void DesktopCapability::configure(const nlohmann::json& config, const SourceFactory& sources) {
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
    impl_->helper = config.value("helper", nlohmann::json::object());
    log::info("desktop", "backend available", {{"display_server", f->display_server}, {"package", f->package}, {"path", f->path}});
    // A running agent gets a live backend; tools that only check config (no core loop) do not.
    const auto* registry = dynamic_cast<const media::SourceRegistry*>(&sources);
    GMainContext* loop = registry ? registry->context() : nullptr;
    if (!loop) return;
    const std::string helper_config = impl_->helper.dump();
    const desktop::ModuleHost host{helper_config.c_str(), loop, &module_log};
    std::string error;
    impl_->driver = impl_->loader.create(*f, host, &error);
    if (!impl_->driver) {
        impl_->unavailable = "the " + impl_->chosen + " backend would not start: " + (error.empty() ? "see the log above" : error);
        log::warn("desktop", "no backend", {{"reason", impl_->unavailable}});
        impl_->chosen.clear();
        return;
    }
    impl_->driver->on_monitors_changed([this](std::vector<Monitor> monitors) { impl_->on_monitors(monitors); });
}

void DesktopCapability::session_attached(SessionContext& ctx, const nlohmann::json&) {
    impl_->sessions[ctx.id()] = &ctx;
    for (auto& spec : impl_->specs()) ctx.add_track(std::move(spec)); // unavailable until the helper hands it over: held back by the core
}

void DesktopCapability::session_detached(const SessionId& id, DetachReason, std::string_view) { impl_->sessions.erase(id); }

void DesktopCapability::release_all_input(const SessionId&) {
    if (impl_->driver) impl_->driver->release_all_input();
}

void DesktopCapability::on_message(SessionContext& ctx, const Envelope& msg) {
    if (msg.kind != "request") return;
    // M3 brings capture and input. Until then the honest answer to any request is the same one
    // `/sources` gives, with the reason — never a silent no-op that looks like it worked.
    if (!impl_->chosen.empty()) {
        ctx.fail(msg, error_codes::unavailable, "desktop input arrives in M3 slice 3.2; the " + impl_->chosen + " backend streams the screen only");
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

void DesktopCapability::shutdown() {
    impl_->sessions.clear();
    impl_->source.reset();
    impl_->driver.reset();
}

} // namespace fjarr
