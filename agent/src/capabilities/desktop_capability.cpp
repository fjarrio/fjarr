// spec: docs/06-capabilities.md · docs/adr/0021-desktop-backends-as-runtime-modules.md
#include <fjarr/desktop_capability.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <tuple>

#include <fjarr/blob.hpp>
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
constexpr std::size_t CLIPBOARD_MAX = 1024 * 1024; // docs/08: 1 MiB in either direction

std::string random_uuid() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::array<std::uint8_t, 16> b{};
    for (auto& x : b) x = static_cast<std::uint8_t>(rng());
    b[6] = static_cast<std::uint8_t>((b[6] & 0x0F) | 0x40); // version 4
    b[8] = static_cast<std::uint8_t>((b[8] & 0x3F) | 0x80);
    return blob::uuid_text(b);
}

bool view_only(SessionContext& ctx) {
    const auto& p = ctx.granted_params("fjarr.desktop");
    return p.is_object() && p.value("view_only", false);
}
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

    // The clipboard (docs/08 clipboard-*, M3 3.5). The robot's current content, as offered.
    std::string offer_id;
    std::vector<std::string> offer_types;
    // clipboard-write: its request and its bytes travel on different channels, so either can come
    // first; they meet here by blob id.
    struct Incoming {
        SessionId session;
        std::optional<Envelope> request;
        std::string type;
        std::uint64_t len = 0;
        std::string bytes;
        bool complete = false;
        std::chrono::steady_clock::time_point first = std::chrono::steady_clock::now();
    };
    std::map<std::string, Incoming> incoming; // by blob id

    // Virtual monitors a session added (docs/08 add-monitor), and the adds still waiting for the
    // monitor to appear in the layout.
    std::map<SessionId, std::set<MonitorId>> virtuals;
    struct PendingAdd {
        SessionId session;
        Envelope request;
        std::unique_ptr<Timer> timeout;
    };
    std::map<MonitorId, PendingAdd> pending_adds;
    static constexpr std::size_t MAX_VIRTUAL_PER_SESSION = 2;

    /// An add whose monitor is in the layout now: answered with its id.
    void answer_adds(const std::vector<Monitor>& monitors) {
        for (const auto& m : monitors) {
            auto p = pending_adds.find(m.id);
            if (p == pending_adds.end()) continue;
            auto s = sessions.find(p->second.session);
            if (s != sessions.end()) s->second->result(p->second.request, {{"ok", true}, {"monitor", m.wire_id}});
            pending_adds.erase(p);
        }
    }
    void remove_virtuals(const SessionId& id) {
        auto v = virtuals.find(id);
        if (v == virtuals.end()) return;
        for (const MonitorId m : v->second) {
            pending_adds.erase(m);
            if (driver) driver->destroy_virtual_monitor(m);
        }
        virtuals.erase(v);
    }

    // The cursor (docs/08 `cursor`, `cursor-position`; docs/22#cursor-strategy).
    std::optional<CursorShape> cursor_shape;
    struct CursorSession {
        std::set<std::string> images;       // shape ids whose image this session has been sent
        std::optional<nlohmann::json> pending; // the newest position not yet sent
        std::chrono::steady_clock::time_point last{};
        std::unique_ptr<Timer> timer;
    };
    std::map<SessionId, CursorSession> cursors;
    static constexpr std::chrono::milliseconds POSITION_INTERVAL{33}; // ≤ 30 per second per session

    void send_shape(SessionContext& ctx) {
        if (!cursor_shape) return;
        const CursorShape& s = *cursor_shape;
        auto& cs = cursors[ctx.id()];
        nlohmann::json p{{"shape_id", s.shape_id}, {"hotspot", {{"x", s.hot_x}, {"y", s.hot_y}}}};
        if (!s.visible) {
            p["hidden"] = true;
        } else if (!cs.images.count(s.shape_id) && !s.rgba.empty()) {
            // The pixels as a blob: an envelope is at most 16 KiB (docs/08 `cursor`).
            const auto ref = ctx.send_blob(std::string(s.rgba.begin(), s.rgba.end()), "application/x-fjarr-rgba");
            p["image"] = {{"w", s.width}, {"h", s.height}, {"blob", ref.to_json()}};
            cs.images.insert(s.shape_id);
        }
        ctx.event("cursor", std::move(p));
    }
    void on_cursor_shape(const CursorShape& shape) {
        log::debug("desktop", "cursor shape", {{"shape", shape.shape_id}, {"size", std::to_string(shape.width) + "x" + std::to_string(shape.height)}});
        cursor_shape = shape;
        for (auto& [_, ctx] : sessions) send_shape(*ctx);
    }
    void flush_position(const SessionId& id) {
        auto s = sessions.find(id);
        auto c = cursors.find(id);
        if (s == sessions.end() || c == cursors.end() || !c->second.pending) return;
        s->second->realtime().send(protocol::make_envelope("fjarr.desktop", "cursor-position", "event", std::move(*c->second.pending)));
        c->second.pending.reset();
        c->second.last = std::chrono::steady_clock::now();
    }
    void on_cursor_position(MonitorId monitor, double nx, double ny) {
        std::string track; // the monitor's track, as pointer input names it (monitor_for)
        if (driver)
            for (const auto& m : driver->monitors())
                if (m.id == monitor) track = "desk-" + m.wire_id;
        if (track.empty()) return;
        const auto now = std::chrono::steady_clock::now();
        for (auto& [id, ctx] : sessions) {
            auto& cs = cursors[id];
            cs.pending = nlohmann::json{{"track_id", track}, {"x", nx}, {"y", ny}};
            if (now - cs.last >= POSITION_INTERVAL) {
                flush_position(id);
            } else if (!cs.timer) {
                // Newest wins: one deferred send carries whatever is pending when it fires.
                const SessionId sid = id;
                cs.timer = ctx->every(POSITION_INTERVAL, [this, sid] {
                    flush_position(sid);
                    auto c = cursors.find(sid);
                    if (c != cursors.end()) c->second.timer.reset();
                    return false;
                });
            }
        }
    }

    void offer(SessionContext& ctx) {
        if (view_only(ctx)) return; // docs/10: a viewer is given the screen, not the clipboard
        ctx.event("clipboard-offer", {{"offer_id", offer_id}, {"types", offer_types}});
    }
    void on_clipboard_changed(std::vector<std::string> types) {
        offer_id = random_uuid();
        offer_types = std::move(types);
        log::info("desktop", "the robot's clipboard changed", {{"types", nlohmann::json(offer_types).dump()}});
        for (auto& [_, ctx] : sessions) offer(*ctx);
    }
    /// A clipboard-write whose request and bytes have both arrived: onto the robot's clipboard.
    void try_write(const std::string& blob_id) {
        auto it = incoming.find(blob_id);
        if (it == incoming.end() || !it->second.request || !it->second.complete) return;
        Incoming in = std::move(it->second);
        incoming.erase(it);
        auto* clip = driver ? driver->clipboard() : nullptr;
        auto s = sessions.find(in.session);
        if (s == sessions.end()) return;
        if (!clip) return s->second->fail(*in.request, error_codes::unavailable, "this robot's desktop has no clipboard");
        const SessionId sid = in.session;
        const Envelope req = *in.request;
        clip->write(in.type, std::move(in.bytes), [this, sid, req](bool ok, std::string error) {
            // The robot's clipboard is the operator's paste now: the last offer no longer describes
            // it, and offering it to a later session made that session read our own selection
            // (refused by mutter, 2026-10-03). Our own write is never offered (docs/08).
            if (ok) {
                offer_id.clear();
                offer_types.clear();
            }
            auto s2 = sessions.find(sid);
            if (s2 == sessions.end()) return;
            if (ok) s2->second->result(req, {{"ok", true}});
            else s2->second->fail(req, error_codes::unavailable, "the robot's clipboard was not set: " + error);
        });
    }
    /// Writes whose other half never came (a client that stopped mid-write) are dropped after 10 s.
    void expire_incoming() {
        const auto now = std::chrono::steady_clock::now();
        for (auto it = incoming.begin(); it != incoming.end();) {
            if (now - it->second.first < std::chrono::seconds(10)) {
                ++it;
                continue;
            }
            auto s = sessions.find(it->second.session);
            if (s != sessions.end() && it->second.request) s->second->fail(*it->second.request, error_codes::payload_invalid, "the clipboard's bytes did not all arrive");
            it = incoming.erase(it);
        }
    }

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
        if (m.cursor_in_video) mi.cursor = "embedded"; // docs/23: this capture fell back
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
            // Local cursor where the backend has one (docs/22#cursor-strategy): the video leaves it out,
            // and the shape and position arrive beside it.
            CaptureOptions options;
            options.cursor_in_video = !driver->features().local_cursor;
            Screen sc{m, driver->start_capture(m.id, options)};
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
        answer_adds(monitors); // after the monitors event: the client hears of the monitor first
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
    // Bulk carries the clipboard's bytes as blob frames, both ways (docs/08 clipboard-read, -write).
    m.channels = {{ChannelClass::Control}, {ChannelClass::Realtime}, {ChannelClass::Bulk, BulkFraming::Blob}};
    m.consumers.peer = true;
    m.input_bearing = true;
    m.control_domain = "desktop"; // docs/10: one pointer, one keyboard — one holder at a time
    // docs/08#input-events-fjarrdesktop. Not `release-all`: a viewer's window losing focus sends
    // it, and that must never claim a free desktop.
    // clipboard-write is input too (docs/10): a paste lands where the holder is typing. clipboard-read is not.
    m.control_inputs = {"pointer", "button", "wheel", "key", "key-combo", "text", "clipboard-write", "add-monitor", "remove-monitor"};
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
    if (auto* clip = impl_->driver->clipboard()) clip->on_changed([this](std::vector<std::string> types) { impl_->on_clipboard_changed(std::move(types)); });
    impl_->driver->on_cursor_shape([this](const CursorShape& shape) { impl_->on_cursor_shape(shape); });
    impl_->driver->on_cursor_position([this](MonitorId m, double x, double y) { impl_->on_cursor_position(m, x, y); });
}

void DesktopCapability::session_attached(SessionContext& ctx, const nlohmann::json&) {
    impl_->sessions[ctx.id()] = &ctx;
    for (auto& spec : impl_->specs()) ctx.add_track(std::move(spec)); // unavailable until the helper hands it over: held back by the core
    if (!impl_->offer_id.empty()) impl_->offer(ctx); // what the robot holds now, so a late viewer can paste it too
    impl_->send_shape(ctx);                          // the cursor as it is now (docs/08 `cursor`)
}

void DesktopCapability::session_detached(const SessionId& id, DetachReason, std::string_view) {
    impl_->sessions.erase(id);
    impl_->pointer_seq.erase(id);
    impl_->cursors.erase(id); // its timer goes with it
    impl_->remove_virtuals(id); // docs/08 add-monitor: a session's virtual monitors go with it
    for (auto it = impl_->incoming.begin(); it != impl_->incoming.end();) it = it->second.session == id ? impl_->incoming.erase(it) : std::next(it);
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
    if (msg.type == "clipboard-read") {
        // Reading claims no control (docs/10), so it comes before the input bookkeeping below.
        if (view_only(ctx)) return refuse(error_codes::capability_denied, "a view-only session sees no clipboard");
        auto* clip = d.clipboard();
        if (!clip) return refuse(error_codes::unavailable, "this robot's desktop has no clipboard");
        const std::string type = p.value("type", std::string{});
        if (p.value("offer_id", std::string{}) != impl_->offer_id || impl_->offer_id.empty())
            return refuse(error_codes::payload_invalid, "the robot's clipboard changed since that offer");
        if (std::find(impl_->offer_types.begin(), impl_->offer_types.end(), type) == impl_->offer_types.end())
            return refuse(error_codes::payload_invalid, "the offer has no '" + type + "'");
        const SessionId sid = ctx.id();
        const Envelope req = msg;
        clip->read(type, CLIPBOARD_MAX, [this, sid, req, type](std::optional<std::string> bytes, std::string error) {
            auto s = impl_->sessions.find(sid);
            if (s == impl_->sessions.end()) return;
            SessionContext& c = *s->second;
            if (!bytes) {
                if (error == "too-large") return c.fail(req, error_codes::unavailable, "larger than 1 MiB", {{"reason", "too-large"}, {"limit", CLIPBOARD_MAX}});
                return c.fail(req, error_codes::unavailable, "the robot's clipboard could not be read: " + error);
            }
            const auto ref = c.send_blob(std::move(*bytes), type);
            c.result(req, {{"ok", true}, {"blob", ref.to_json()}});
        });
        return;
    }
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
    } else if (msg.type == "clipboard-write") {
        // Input in the desktop domain (control_inputs): the core let it through only for the holder.
        if (!d.clipboard()) return refuse(error_codes::unavailable, "this robot's desktop has no clipboard");
        const std::string type = p.value("type", std::string{});
        if (type != "text/plain") return refuse(error_codes::payload_invalid, "the clipboard takes text/plain");
        const auto ref = blob::BlobRef::from_json(p.value("blob", nlohmann::json{}));
        if (!ref) return refuse(error_codes::payload_invalid, "clipboard-write needs a blob reference");
        if (ref->len > CLIPBOARD_MAX) return ctx.fail(msg, error_codes::unavailable, "larger than 1 MiB", {{"reason", "too-large"}, {"limit", CLIPBOARD_MAX}});
        impl_->expire_incoming();
        auto& in = impl_->incoming[ref->id];
        if (!in.session.empty() && in.session != ctx.id()) return refuse(error_codes::payload_invalid, "that blob belongs to another session");
        in.session = ctx.id();
        in.request = msg;
        in.type = type;
        in.len = ref->len;
        if (ref->len == 0) in.complete = true; // an empty clipboard: no frames will come
        impl_->try_write(ref->id);
    } else if (msg.type == "add-monitor") {
        // docs/08 add-monitor: input in the desktop domain (control_inputs), so only the holder is here.
        const int w = p.value("width", 0), h = p.value("height", 0);
        if (w < 320 || w > 3840 || h < 240 || h > 2160) return refuse(error_codes::payload_invalid, "a virtual monitor is 320-3840 wide and 240-2160 high");
        if (!d.features().virtual_monitors) return refuse(error_codes::unavailable, "this robot's desktop cannot add a monitor");
        auto& mine = impl_->virtuals[ctx.id()];
        if (mine.size() >= Impl::MAX_VIRTUAL_PER_SESSION) return refuse(error_codes::unavailable, "a session may add at most 2 monitors");
        const MonitorId id = d.create_virtual_monitor(w, h);
        if (id == INVALID_MONITOR) return refuse(error_codes::unavailable, "the robot's desktop would not add a monitor now");
        mine.insert(id);
        const SessionId sid = ctx.id();
        auto& pending = impl_->pending_adds[id];
        pending.session = sid;
        pending.request = msg;
        // The monitor joins the layout once its stream has a consumer; answered then (Impl::answer_adds).
        pending.timeout = ctx.every(std::chrono::seconds(5), [this, id, sid] {
            auto pa = impl_->pending_adds.find(id);
            if (pa == impl_->pending_adds.end()) return false;
            auto s = impl_->sessions.find(sid);
            if (s != impl_->sessions.end()) s->second->fail(pa->second.request, error_codes::unavailable, "the monitor did not appear within 5 s");
            impl_->virtuals[sid].erase(id);
            if (impl_->driver) impl_->driver->destroy_virtual_monitor(id);
            impl_->pending_adds.erase(pa);
            return false;
        });
        impl_->answer_adds(d.monitors()); // a backend that adds it at once has already reported it
        return;
    } else if (msg.type == "remove-monitor") {
        const std::string wire = p.value("id", std::string{});
        auto& mine = impl_->virtuals[ctx.id()];
        for (const MonitorId m : mine) {
            const auto mon = impl_->monitor_for("desk-" + wire);
            if (mon && mon->id == m) {
                mine.erase(m);
                impl_->pending_adds.erase(m);
                d.destroy_virtual_monitor(m);
                if (request) ctx.result(msg, {{"ok", true}});
                return;
            }
        }
        return refuse(error_codes::payload_invalid, "no virtual monitor '" + wire + "' was added by this session");
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

void DesktopCapability::on_blob_chunk(SessionContext& ctx, const BlobChunk& chunk) {
    // spec: docs/08 clipboard-write — the only blobs fjarr.desktop receives.
    if (chunk.blob_len > CLIPBOARD_MAX) return; // refused when its request arrives
    impl_->expire_incoming();
    auto& in = impl_->incoming[chunk.blob_id];
    if (!in.session.empty() && in.session != ctx.id()) return;
    in.session = ctx.id();
    if (chunk.offset == 0) in.bytes.clear();
    in.bytes.append(reinterpret_cast<const char*>(chunk.payload.data()), chunk.payload.size());
    if (in.bytes.size() >= chunk.blob_len) {
        in.complete = true;
        impl_->try_write(chunk.blob_id);
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
