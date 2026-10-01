// spec: docs/06-capabilities.md · docs/adr/0021-desktop-backends-as-runtime-modules.md
#include <fjarr/desktop_capability.hpp>

#include <algorithm>
#include <map>
#include <optional>
#include <tuple>
#include <set>

#include <fjarr/errors.hpp>
#include <fjarr/session_context.hpp>

#include "core/log.hpp"
#include "core/protocol.hpp"
#include "desktop/keycodes.hpp"
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
    // One track per monitor, keyed by wire id (docs/23#desktop-monitors).
    struct Screen {
        Monitor monitor;
        std::shared_ptr<VideoSource> source;
    };
    std::map<std::string, Screen> screens; // by wire id
    bool had_monitors = false;             // the first set is `initial`
    // Input (M3 3.2). The core lets only the desktop domain's holder through (docs/10); this is
    // the session whose input is on the desktop now, so only its end releases what it holds.
    std::optional<SessionId> input_session;
    std::map<SessionId, std::uint64_t> pointer_seq; // docs/08: receivers drop stale motion
    std::set<std::string> unknown_codes;            // logged once each

    /// The monitor a desktop track shows, by its wire id.
    std::optional<Monitor> monitor_for(const std::string& track_id) const {
        if (!driver || track_id.rfind("desk-", 0) != 0) return std::nullopt;
        for (const auto& m : driver->monitors())
            if (m.wire_id == track_id.substr(5)) return m;
        return std::nullopt;
    }
    std::optional<LinuxKeycode> keycode(const std::string& code) {
        if (auto k = desktop::evdev_for_code(code)) return *k;
        if (unknown_codes.insert(code).second) log::warn("desktop", "unknown key code; dropped", {{"code", code}});
        return std::nullopt;
    }

    /// Display order: left to right, then top to bottom (docs/22: `index` is order only).
    std::vector<const Screen*> ordered() const {
        std::vector<const Screen*> out;
        for (const auto& [_, sc] : screens) out.push_back(&sc);
        std::sort(out.begin(), out.end(), [](const Screen* a, const Screen* b) {
            return std::tie(a->monitor.x, a->monitor.y, a->monitor.wire_id) < std::tie(b->monitor.x, b->monitor.y, b->monitor.wire_id);
        });
        return out;
    }
    static MonitorInfo info(const Monitor& m, int index) {
        MonitorInfo mi;
        mi.id = m.wire_id;
        mi.index = index;
        mi.primary = m.primary;
        mi.x = m.x;
        mi.y = m.y;
        mi.w = m.width;
        mi.h = m.height;
        mi.scale = m.scale;
        mi.name = m.label;
        mi.connector = m.connector;
        return mi;
    }
    std::vector<TrackSpec> specs(bool available_only = false) const {
        std::vector<TrackSpec> out;
        int index = 0;
        for (const Screen* sc : ordered()) {
            const int i = index++;
            if (!sc->source || (available_only && !sc->source->available())) continue;
            TrackSpec t;
            t.track_id = "desk-" + sc->monitor.wire_id; // docs/08#track-manifest: from the identity, never the connector
            t.label = sc->monitor.label.empty() ? sc->monitor.connector : sc->monitor.label;
            t.kind = TrackKind::Video;
            t.source = SourceRef{sc->source, "src"};
            t.monitor = info(sc->monitor, i);
            out.push_back(std::move(t));
        }
        return out;
    }
    void reoffer() {
        for (auto& [_, ctx] : sessions) ctx->update_tracks(specs(true));
    }
    /// docs/08 `monitors`: the full current set, sent before the re-offer.
    void announce(const char* reason) {
        nlohmann::json list = nlohmann::json::array();
        int index = 0;
        for (const Screen* sc : ordered()) list.push_back(protocol::monitor_to_json(info(sc->monitor, index++)));
        for (auto& [_, ctx] : sessions) ctx->event("monitors", {{"monitors", list}, {"reason", reason}});
    }
    /// The monitor set changed (docs/23#desktop-monitors): diff by wire id.
    void on_monitors(const std::vector<Monitor>& monitors) {
        bool set_changed = false;
        std::set<std::string> present;
        for (const auto& m : monitors) present.insert(m.wire_id);
        for (auto it = screens.begin(); it != screens.end();) {
            if (present.count(it->first)) {
                ++it;
                continue;
            }
            log::info("desktop", "monitor gone", {{"monitor", it->first}});
            driver->stop_capture(it->second.monitor.id);
            it = screens.erase(it);
            set_changed = true;
        }
        for (const auto& m : monitors) {
            auto it = screens.find(m.wire_id);
            if (it != screens.end()) {
                it->second.monitor = m; // same monitor: new geometry, mode or primary flag, same track
                continue;
            }
            Screen sc{m, driver->start_capture(m.id, CaptureOptions{})};
            if (sc.source) {
                const std::string id = "desk-" + m.wire_id;
                sc.source->on_availability_changed([this, id](bool now) {
                    log::info("desktop", now ? "track available" : "track unavailable", {{"track", id}});
                    reoffer();
                });
            }
            log::info("desktop", "monitor present", {{"monitor", m.wire_id}, {"connector", m.connector}, {"primary", m.primary ? "yes" : "no"}});
            screens.emplace(m.wire_id, std::move(sc));
            set_changed = true;
        }
        announce(!had_monitors ? "initial" : set_changed ? "hotplug" : "mode-change");
        had_monitors = had_monitors || !monitors.empty();
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

void DesktopCapability::session_detached(const SessionId& id, DetachReason, std::string_view) {
    impl_->sessions.erase(id);
    impl_->pointer_seq.erase(id);
    if (impl_->input_session == id) impl_->input_session.reset();
}

void DesktopCapability::release_all_input(const SessionId& id) {
    // spec: docs/15-testing-strategy.md#safety-behaviors — the core calls this on every session end
    // and when control moves. Another session's held keys are not ours to release.
    if (!impl_->driver || (impl_->input_session && *impl_->input_session != id)) return;
    impl_->driver->release_all_input();
    impl_->input_session.reset();
}

void DesktopCapability::on_message(SessionContext& ctx, const Envelope& msg) {
    // spec: docs/08-protocol.md#input-events-fjarrdesktop. The core has already dropped input from a
    // session that does not hold the desktop domain, and from a view-only grant (docs/10).
    const bool request = msg.kind == "request";
    auto refuse = [&](std::string_view code, const std::string& why) {
        if (request) ctx.fail(msg, code, why);
    };
    if (!impl_->driver) return refuse(error_codes::unavailable, impl_->unavailable.empty() ? "no desktop backend is running" : impl_->unavailable);
    auto& d = *impl_->driver;
    const auto& p = msg.payload;
    if (msg.type == "release-all") {
        // Never claims: only the session whose input is on the desktop can let go of it.
        release_all_input(ctx.id());
        if (request) ctx.result(msg, {{"ok", true}});
        return;
    }
    if (impl_->input_session && *impl_->input_session != ctx.id()) d.release_all_input(); // control moved without an end
    impl_->input_session = ctx.id();
    if (msg.type == "pointer") {
        const auto seq = p.value("seq", std::uint64_t{0});
        auto& last = impl_->pointer_seq[ctx.id()];
        if (seq != 0 && seq <= last) return; // stale: realtime is unordered
        last = seq;
        const double x = p.value("x", -1.0), y = p.value("y", -1.0);
        const auto m = impl_->monitor_for(p.value("track_id", std::string{}));
        if (!m || x < 0 || x > 1 || y < 0 || y > 1) return;
        d.pointer_motion(m->id, x, y);
    } else if (msg.type == "button") {
        static const std::map<std::string, MouseButton> buttons{
            {"left", MouseButton::Left}, {"middle", MouseButton::Middle}, {"right", MouseButton::Right}, {"back", MouseButton::Back}, {"forward", MouseButton::Forward}};
        const auto it = buttons.find(p.value("button", std::string{}));
        if (it == buttons.end()) return refuse(error_codes::payload_invalid, "button is left, middle, right, back or forward");
        d.pointer_button(it->second, p.value("down", false));
    } else if (msg.type == "wheel") {
        d.pointer_wheel(p.value("dx", 0.0), p.value("dy", 0.0));
    } else if (msg.type == "key") {
        if (const auto k = impl_->keycode(p.value("code", std::string{}))) d.key(*k, p.value("down", false));
        else return refuse(error_codes::payload_invalid, "unknown key code");
    } else if (msg.type == "key-combo") {
        std::vector<LinuxKeycode> keys;
        for (const auto& c : p.value("codes", nlohmann::json::array())) {
            const auto k = c.is_string() ? impl_->keycode(c.get<std::string>()) : std::nullopt;
            if (!k) return refuse(error_codes::payload_invalid, "unknown key code in the combo");
            keys.push_back(*k);
        }
        for (const auto k : keys) d.key(k, true);
        for (auto it = keys.rbegin(); it != keys.rend(); ++it) d.key(*it, false);
        if (request) ctx.result(msg, {{"ok", true}});
    } else if (msg.type == "text") {
        // Typed through the robot's keymap, whole or not at all (docs/08 `text`).
        std::string error;
        const auto untypable = d.type_text(p.value("text", std::string{}), error);
        if (!error.empty()) return refuse(error_codes::unavailable, error);
        if (!untypable.empty()) {
            if (request) ctx.fail(msg, error_codes::unavailable, "the robot's keyboard layout cannot type these characters", {{"untypable", untypable}});
            return;
        }
        if (request) ctx.result(msg, {{"ok", true}});
    } else {
        refuse(error_codes::payload_invalid, "fjarr.desktop has no message '" + msg.type + "'");
    }
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
    impl_->screens.clear();
    impl_->driver.reset();
}

} // namespace fjarr
