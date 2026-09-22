// Agent — the public entry point wiring the core: loop, config, capability
// registry, media plane, signaling client, session manager, introspection,
// supervision.
// spec: docs/09-interfaces.md#embedding · docs/23-agent-core-architecture.md#object-model
#include <fjarr/agent.hpp>

#include <atomic>
#include <csignal>

#include <nlohmann/json-schema.hpp>

#include <fjarr/errors.hpp>
#include <fjarr/version.hpp>

#include "core/log.hpp"
#include "core/loop.hpp"
#include "core/protocol.hpp"
#include "core/session_manager.hpp"
#include "core/signaling_client.hpp"
#include "capabilities/introspect_capability.hpp"
#include "introspect/introspector.hpp"
#include "introspect/memory.hpp"
#include "introspect/server.hpp"
#include "media/encoder.hpp"
#include "media/media_plane.hpp"
#include "media/sources.hpp"

namespace fjarr {

struct Agent::Impl {
    AgentConfig config;
    std::vector<std::unique_ptr<Capability>> capabilities;
    std::map<std::string, core::RegisteredCapability> registry;
    std::function<void(const SessionEvent&)> session_callback;
    bool shut_down = false;
    std::vector<glib::SourceGuard> signal_sources;
    Supervision supervision;
    CoreLoop loop;
    media::SourceRegistry sources{loop.context()}; // built-ins + register_source_type(); hot-plug callbacks land on the loop
    std::unique_ptr<media::MediaPlane> plane;
    std::unique_ptr<core::SignalingClient> signaling;
    std::unique_ptr<core::SessionManager> sessions;
    std::unique_ptr<introspect::SnapshotStore> snapshots;
    std::unique_ptr<introspect::Server> introspect_server;
    std::unique_ptr<introspect::MemoryCensus> memory;
    glib::SourceGuard watchdog_timer;
    glib::SourceGuard counters_timer;
    glib::SourceGuard snapshot_timer;
    std::atomic<int> exit_code{0};
    bool started = false;
    bool ready_notified = false;

    void emit(const SessionEvent& e) {
        if (session_callback) session_callback(e);
    }

    /// Validate `[capabilities.X]` tables against schemas and configure (docs/23#configuration).
    void configure_capabilities() {
        for (const auto& [name, table] : config.capabilities) {
            if (!registry.count(name)) throw FjarrError("config", "capabilities." + name + " names an unregistered capability");
        }
        for (auto& [name, rc] : registry) {
            nlohmann::json table = nlohmann::json::object();
            auto it = config.capabilities.find(name);
            if (it != config.capabilities.end()) table = it->second;
            rc.enabled = table.value("enabled", true);
            nlohmann::json schema = rc.manifest.config_schema.is_object() ? rc.manifest.config_schema : nlohmann::json{{"type", "object"}};
            try {
                validate_json_schema(schema, table);
            } catch (const FjarrError& e) {
                throw FjarrError("config", "capabilities." + name + ": " + e.message());
            }
            rc.capability->configure(table, sources);
        }
    }

    void take_snapshot(GstBin* bin, const std::string& trigger, const std::string& session_id) {
        if (!snapshots) return;
        introspect::SnapshotMeta meta;
        meta.pipeline_id = glib::element_name(GST_ELEMENT(bin));
        meta.kind = session_id.empty() ? "producer" : "session";
        meta.session_id = session_id;
        meta.robot_id = config.agent.robot_id;
        meta.trigger = trigger;
        if (!session_id.empty()) meta.pipeline_id = "session:" + session_id;
        snapshots->take(bin, meta);
    }

    /// GET /stats (docs/24): sessions with their last sample, hub entries, producers.
    nlohmann::json stats_json() const {
        nlohmann::json hub = nlohmann::json::array();
        nlohmann::json producers = nlohmann::json::array();
        if (plane) {
            for (const auto& key : plane->hub().keys()) {
                const auto st = plane->hub().stats(key);
                hub.push_back({{"track_id", key.track_id}, {"tier", key.tier}, {"subscribers", st.subscribers}, {"frames", st.frames},
                               {"keyframes", st.keyframes}, {"ring", st.ring}, {"has_keyframe", st.has_keyframe}});
            }
            for (auto* p : plane->producers()) {
                nlohmann::json tiers = nlohmann::json::array();
                nlohmann::json kbps = nlohmann::json::object();
                for (const char* t : {"active", "thumbnail"})
                    if (p->has_tier(t)) {
                        tiers.push_back(t);
                        kbps[t] = p->current_kbps(t); // the rate-controlled target (docs/23#rate-control-and-tier-switching)
                    }
                producers.push_back({{"name", p->name()}, {"playing", p->playing()}, {"tiers", tiers}, {"kbps", kbps}, {"error", p->error()}});
            }
        }
        return nlohmann::json{{"robot_id", config.agent.robot_id}, {"sessions", sessions ? sessions->stats() : nlohmann::json::array()},
                              {"lease", sessions ? sessions->describe().value("lease", nlohmann::json::object()) : nlohmann::json::object()},
                              {"hub", hub}, {"producers", producers}, {"encoder", plane ? plane->encoder().name : ""}};
    }

    /// What the diagnostics bundle carries besides the rings (docs/24): config with secrets redacted, versions, the check.
    nlohmann::json bundle_extra_json() const {
        const auto& a = config.agent;
        nlohmann::json cfg{{"agent",
                            {{"robot_id", a.robot_id}, {"server_url", a.server_url}, {"credential_file", a.credential_file},
                             {"dev_token", a.dev_token.empty() ? "" : "<redacted>"}, {"ice_policy", a.ice_policy}, {"log_level", a.log_level},
                             {"log_format", a.log_format}, {"dot_dir", a.dot_dir}, {"watchdog_secs", a.watchdog_secs},
                             {"allow_unsupervised", a.allow_unsupervised}}},
                           {"media",
                            {{"encoder", config.media.encoder}, {"gop_seconds", config.media.gop_seconds}, {"active_kbps", config.media.active_kbps},
                             {"active_floor_kbps", config.media.active_floor_kbps}, {"thumbnail_kbps", config.media.thumbnail_kbps}, {"tier_grace_ms", config.media.tier_grace_ms}}},
                           {"introspect",
                            {{"enabled", config.introspect.enabled}, {"bind", config.introspect.bind}, {"port", config.introspect.port},
                             {"socket", config.introspect.socket}, {"token", config.introspect.token.empty() ? "" : "<redacted>"},
                             {"history", config.introspect.history}, {"viewer_dir", config.introspect.viewer_dir}}},
                           {"capabilities", config.capabilities}};
        nlohmann::json versions{{"fjarr", version()}, {"gstreamer", gstreamer_version()}};
        // No smoke pipeline here (it would block the loop): the resolved encoder is the check's answer.
        std::string check = "encoder configured: " + config.media.encoder + "\nencoder in use: " + (plane ? plane->encoder().name : "(no plane)") +
                            "\ncapabilities:";
        for (const auto& [name, rc] : registry) check += " " + name + (rc.enabled ? "" : "(disabled)");
        check += "\n";
        return nlohmann::json{{"config", cfg}, {"versions", versions}, {"check", check}};
    }

    nlohmann::json sources_json() const {
        // Configured sources come from the capabilities (visible before any session, with the reason a
        // missing device is missing — docs/26); the plane adds negotiated caps and tier state once a
        // session has registered the track.
        std::map<std::string, nlohmann::json> by_track;
        std::vector<std::string> order;
        for (const auto& [name, rc] : registry)
            for (const auto& s : rc.capability->configured_sources()) {
                by_track[s.track_id] = {{"track_id", s.track_id}, {"cap", name}, {"label", s.label}, {"identity", s.identity},
                                        {"status", s.available ? "available" : "missing"}, {"reason", s.reason}, {"required", s.required},
                                        {"caps", ""}, {"tiers", nlohmann::json::array()}, {"playing", false}};
                order.push_back(s.track_id);
            }
        if (plane)
            for (const auto& s : plane->source_status()) {
                auto it = by_track.find(s.track_id);
                if (it == by_track.end()) {
                    by_track[s.track_id] = {{"track_id", s.track_id}, {"cap", s.cap}, {"identity", s.identity}};
                    order.push_back(s.track_id);
                    it = by_track.find(s.track_id);
                }
                it->second["status"] = s.available ? "available" : "missing";
                it->second["reason"] = s.reason;
                it->second["caps"] = s.caps;
                it->second["tiers"] = s.tiers;
                it->second["playing"] = s.playing;
            }
        nlohmann::json arr = nlohmann::json::array();
        for (const auto& id : order) arr.push_back(by_track[id]);
        return nlohmann::json{{"encoder", plane ? plane->encoder().name : ""}, {"sources", arr}};
    }

    void boot() {
        // Called on the core loop.
        auto encoder = media::resolve_encoder(config.media.encoder);
        plane = std::make_unique<media::MediaPlane>(loop, config.media, encoder, sources);
        snapshots = std::make_unique<introspect::SnapshotStore>(
            static_cast<std::size_t>(config.introspect.history), config.agent.dot_dir,
            [this](std::chrono::milliseconds delay, std::function<void()> fn) {
                // One-shot timers the store owns: destroying the store cancels what is pending.
                return loop.add_timeout(delay, [fn] {
                    fn();
                    return false;
                });
            });
        plane->on_producer_event([this](const std::string& track_id, const std::string& event) {
            if (track_id.empty()) return;
            if (auto* p = plane->producer(track_id); p && p->pipeline()) take_snapshot(GST_BIN(p->pipeline()), event, "");
        });
        plane->on_rebuild_needed([this](const std::string& reason) {
            log::error("agent", "media plane rebuild", {{"reason", reason}});
            sessions->close_all("media-restart", /*retry=*/true); // docs/08: a rung of the reconnection ladder
            plane->rebuild();
            if (plane->rebuilds_in_window() >= 3) {
                log::error("agent", "third plane rebuild within 10 minutes: exit 2 (restart me)");
                exit_code = 2;
                loop.quit();
            }
        });
        core::SessionDeps deps;
        deps.loop = &loop;
        deps.plane = plane.get();
        deps.config = &config;
        deps.send_signal = [this](nlohmann::json m) {
            if (signaling) signaling->send(m);
        };
        deps.emit = [this](const SessionEvent& e) {
            if (e.type == "ended" && snapshots) snapshots->retire("session:" + e.session_id); // docs/24: last 8 for 10 min
            emit(e);
        };
        deps.snapshot = [this](GstBin* bin, const std::string& trigger) {
            const std::string name = glib::element_name(GST_ELEMENT(bin));
            std::string sid;
            for (const auto& s : sessions->list())
                if (name == "session:" + s->sid8()) sid = s->id();
            take_snapshot(bin, trigger, sid);
        };
        auto tcfg = config.capabilities.find("fjarr.test");
        deps.test_hooks = tcfg != config.capabilities.end() && tcfg->second.value("test_hooks", false);
        deps.known_capability = [this](const std::string& name) { return registry.count(name) > 0; };
        sessions = std::make_unique<core::SessionManager>(deps, [this](const std::string& name) -> const core::RegisteredCapability* {
            auto it = registry.find(name);
            return it == registry.end() ? nullptr : &it->second;
        });
        if (config.introspect.enabled) {
            memory = std::make_unique<introspect::MemoryCensus>([this] {
                return nlohmann::json{{"hub_buffers_held", plane ? plane->hub().buffers_held() : 0},
                                      {"channel_bytes_buffered", sessions ? sessions->buffered_bytes() : 0},
                                      {"sessions_alive", sessions ? sessions->size() : 0},
                                      {"producers_alive", plane ? plane->producers().size() : 0}};
            });
            introspect::Providers providers;
            providers.sources = [this] { return sources_json(); };
            providers.snapshot_now = [this](const std::string& pid) {
                for (const auto& s : sessions->list())
                    if (s->consumer() && (pid.empty() || pid == "session:" + s->id())) snapshots->take(GST_BIN(s->consumer()->pipeline()), {"session:" + s->id(), "session", s->id(), config.agent.robot_id, static_cast<unsigned>(s->generation()), 0, 0, "on-demand", ""}, true);
                for (auto* p : plane->producers())
                    if (pid.empty() || pid == p->name()) take_snapshot(GST_BIN(p->pipeline()), "on-demand", "");
            };
            providers.stats = [this] { return stats_json(); };
            providers.bundle_extra = [this] { return bundle_extra_json(); };
            providers.memory = memory.get();
            introspect_server = std::make_unique<introspect::Server>(config.introspect, *snapshots, std::move(providers));
            introspect_server->start();
        }
        core::SignalingConfig scfg;
        scfg.url = config.agent.server_url;
        scfg.robot_id = config.agent.robot_id;
        scfg.dev_token = config.agent.dev_token;
        scfg.fjarr_version = version();
        for (const auto& [name, rc] : registry)
            if (rc.enabled) scfg.capability_names.push_back(name);
        core::SignalingHooks hooks;
        hooks.on_ready = [this] {
            if (!ready_notified && supervision.ready) {
                supervision.ready();
                ready_notified = true;
            }
        };
        hooks.on_message = [this](const protocol::SignalingMessage& m) {
            if (m.type == "session-request") sessions->on_session_request(m);
            else if (m.type == "backend-stream") log::debug("agent", "backend-stream ignored (M4)");
            else if (m.type == "error") log::warn("agent", "server error", {{"code", m.body.value("code", "")}, {"message", m.body.value("message", "")}});
            else sessions->on_signal(m);
        };
        hooks.on_closed = [this](const std::string&) { sessions->close_all("peer-gone"); };
        hooks.on_error = [this](const std::string& code, const std::string&) {
            if (code == "auth-failed") {
                log::error("agent", "device auth failed: check FJARR_DEV_DEVICE_TOKEN / the credential (exit 1)");
                exit_code = 1;
                loop.quit();
            }
        };
        signaling = std::make_unique<core::SignalingClient>(loop, scfg, hooks);
        signaling->start();
        if (supervision.watchdog && supervision.watchdog_interval_ms > 0) {
            watchdog_timer = loop.add_timeout(std::chrono::milliseconds(supervision.watchdog_interval_ms), [this] {
                supervision.watchdog();
                return true;
            });
        }
        counters_timer = loop.add_timeout(std::chrono::seconds(60), [this] {
            const auto& c = glib::ObjectCensus::instance();
            log::info("agent", "counters", {{"sessions", std::to_string(sessions->size())}, {"elements", std::to_string(c.elements.load())},
                                            {"pads", std::to_string(c.pads.load())}, {"samples", std::to_string(c.samples.load())},
                                            {"buffers", std::to_string(c.buffers.load())}, {"pipelines", std::to_string(c.pipelines.load())},
                                            {"hub_buffers", std::to_string(plane->hub().buffers_held())}});
            snapshots->expire_retired();
            return true;
        });
        log::info("agent", "core started", {{"robot_id", config.agent.robot_id}, {"encoder", encoder.name},
                                            {"capabilities", std::to_string(registry.size())}});
    }

    void shutdown() {
        // On the core loop: orderly, bounded by Supervision::stop_deadline_ms. Idempotent.
        if (shut_down) return;
        shut_down = true;
        if (sessions) sessions->close_all("agent-shutdown");
        if (signaling) {
            // Let the queued session-close frames leave before the socket goes (docs/23: operators
            // see agent-shutdown, not peer-gone). Bounded pump of our own context.
            signaling->begin_close("agent-shutdown");
            const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
            auto wake = loop.add_timeout(std::chrono::milliseconds(300), [] { return false; });
            while (!signaling->closed() && std::chrono::steady_clock::now() < deadline) loop.iterate(true);
            signaling->stop();
        }
        for (auto& c : capabilities) {
            try {
                c->shutdown();
            } catch (...) {
            }
        }
        introspect_server.reset();
        memory.reset();
        sessions.reset();
        signaling.reset();
        plane.reset();
        snapshots.reset();
        watchdog_timer.cancel();
        counters_timer.cancel();
    }
};

void validate_json_schema(const nlohmann::json& schema, const nlohmann::json& instance) {
    try {
        nlohmann::json_schema::json_validator validator;
        validator.set_root_schema(schema);
        validator.validate(instance);
    } catch (const std::exception& e) {
        throw FjarrError("config", e.what());
    }
}

Agent::Agent(AgentConfig config) : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
    if (!gst_is_initialized()) gst_init(nullptr, nullptr);
    log::set_level(impl_->config.agent.log_level);
    log::set_json(impl_->config.agent.log_format == "json");
    if (impl_->config.introspect.enabled) {
        // Built in (docs/06): the rings exist once the agent boots, so the capability resolves them lazily.
        auto cap = std::make_unique<capabilities::IntrospectCapability>([this] { return impl_->snapshots.get(); },
                                                                         [this] { return impl_->stats_json(); });
        const auto manifest = cap->manifest();
        impl_->registry[manifest.name] = core::RegisteredCapability{cap.get(), manifest, true};
        impl_->capabilities.push_back(std::move(cap));
        log::info("agent", "capability registered", {{"name", manifest.name}, {"built_in", "true"}});
    }
}

Agent::~Agent() {
    stop();
}

void Agent::register_capability(std::unique_ptr<Capability> capability) {
    if (impl_->started) throw FjarrError("config", "register_capability must precede run()/start()");
    const auto manifest = capability->manifest();
    if (!protocol::valid_cap_name(manifest.name)) throw FjarrError("config", "invalid capability name: " + manifest.name);
    if (impl_->registry.count(manifest.name)) throw FjarrError("config", "capability registered twice: " + manifest.name);
    for (const auto& dep : manifest.dependencies)
        if (!impl_->registry.count(dep)) throw FjarrError("config", manifest.name + " depends on unregistered capability " + dep);
    impl_->registry[manifest.name] = core::RegisteredCapability{capability.get(), manifest, true};
    impl_->capabilities.push_back(std::move(capability));
    log::info("agent", "capability registered", {{"name", manifest.name}});
}

void Agent::register_source_type(SourceType type) {
    if (impl_->started) throw FjarrError("config", "register_source_type must precede run()/start()");
    impl_->sources.add(std::move(type));
}

void Agent::on_session_event(std::function<void(const SessionEvent&)> callback) { impl_->session_callback = std::move(callback); }
void Agent::supervision(Supervision s) { impl_->supervision = std::move(s); }

int Agent::introspect_port() const { return impl_->introspect_server ? impl_->introspect_server->port() : 0; }

int Agent::run() {
    try {
        impl_->config.validate();
        impl_->configure_capabilities(); // throws FjarrError(config) for a bad or required-but-missing track
    } catch (const FjarrError& e) {
        log::error("agent", "configuration refused", {{"code", e.code()}, {"message", e.message()}});
        impl_->exit_code = 1;
        return 1;
    }
    impl_->started = true;
    bool boot_failed = false;
    impl_->loop.post([this, &boot_failed] {
        try {
            impl_->boot();
        } catch (const FjarrError& e) {
            log::error("agent", "startup failed", {{"code", e.code()}, {"message", e.message()}});
            boot_failed = true;
            impl_->exit_code = 1;
            impl_->loop.quit();
        } catch (const std::exception& e) {
            log::error("agent", "startup failed", {{"code", "internal"}, {"message", e.what()}});
            boot_failed = true;
            impl_->exit_code = 1;
            impl_->loop.quit();
        }
    });
    impl_->loop.run();
    if (!boot_failed) impl_->loop.call_sync([this] { impl_->shutdown(); });
    return impl_->exit_code;
}

void Agent::start() {
    impl_->config.validate();
    impl_->configure_capabilities(); // FjarrError(config) propagates to the embedder, as documented
    impl_->started = true;
    impl_->loop.start();
    impl_->loop.call_sync([this] {
        try {
            impl_->boot();
        } catch (const FjarrError& e) {
            log::error("agent", "startup failed", {{"code", e.code()}, {"message", e.message()}});
            impl_->exit_code = 1;
        } catch (const std::exception& e) {
            log::error("agent", "startup failed", {{"code", "internal"}, {"message", e.what()}});
            impl_->exit_code = 1;
        }
    });
    if (impl_->exit_code == 1) throw FjarrError("config", "agent failed to start (see log)");
}

void Agent::stop_on_signal(int signum) {
    if (impl_->started) throw FjarrError("config", "stop_on_signal must precede run()/start()");
    impl_->signal_sources.push_back(impl_->loop.add_unix_signal(signum, [this, signum] {
        // On the core loop, not in a signal handler. docs/15: input is released on the way out;
        // the deadline thread is the bound if a NULL transition wedges.
        if (impl_->shut_down) return;
        log::info("agent", "signal: stopping", {{"signal", std::to_string(signum)}});
        const int deadline = impl_->supervision.stop_deadline_ms;
        std::thread([deadline] {
            std::this_thread::sleep_for(std::chrono::milliseconds(deadline));
            std::fprintf(stderr, "fjarr: shutdown exceeded %d ms, exiting\n", deadline);
            std::_Exit(0);
        }).detach();
        stop();
    }));
}

void Agent::stop() {
    if (!impl_->started) return;
    if (impl_->loop.running() && !impl_->loop.is_owner_thread()) {
        impl_->loop.call_sync([this] { impl_->shutdown(); });
        impl_->loop.stop();
    } else if (impl_->loop.running()) {
        impl_->shutdown();
        impl_->loop.quit();
    }
    impl_->started = false;
}

} // namespace fjarr
