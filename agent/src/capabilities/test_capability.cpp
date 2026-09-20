// fjarr.test — the built-in test capability.
// spec: docs/06-capabilities.md#fjarrtest--the-built-in-test-capability-slice-3
#include <fjarr/test_capability.hpp>

#include <map>
#include <memory>

#include <fjarr/errors.hpp>
#include <fjarr/session_context.hpp>

#include "core/log.hpp"
#include "core/protocol.hpp"
#include "core/session.hpp"
#include "media/sources.hpp"

namespace fjarr {

namespace {
constexpr std::chrono::milliseconds DEADMAN_BUDGET{500};

TrackSpec pattern_track() {
    TrackSpec t;
    t.track_id = "test-pattern";
    t.label = "Test pattern";
    t.kind = TrackKind::Video;
    t.source = SourceRef{std::make_shared<media::TestPatternSource>("smpte", 1280, 720, 30, "test:smpte"), "src"};
    return t;
}
TrackSpec second_track() {
    TrackSpec t;
    t.track_id = "test-second";
    t.label = "Second pattern";
    t.kind = TrackKind::Video;
    t.source = SourceRef{std::make_shared<media::TestPatternSource>("ball", 1280, 720, 30, "test:ball"), "src"};
    return t;
}
} // namespace

struct TestCapability::Impl {
    bool test_hooks = false;
    struct PerSession {
        SessionContext* ctx = nullptr;
        bool plugged = false;
        std::unique_ptr<DeadmanHandle> deadman;
        std::shared_ptr<VideoSource> pattern, second;
    };
    std::map<SessionId, PerSession> sessions;

    void emit_deadman(SessionContext& ctx, const char* state, PerSession& s) {
        const auto since = s.deadman ? s.deadman->since_feed().count() : 0;
        try {
            ctx.event("deadman", nlohmann::json{{"state", state}, {"ms_since_feed", since}});
        } catch (const std::exception&) {
            /* control channel gone: the session is ending */
        }
    }
};

TestCapability::TestCapability() : impl_(std::make_unique<Impl>()) {}
TestCapability::~TestCapability() = default;

CapabilityManifest TestCapability::manifest() const {
    CapabilityManifest m;
    m.name = "fjarr.test";
    m.version = {0, 1, 0};
    m.tracks = {{"test-pattern", "Test pattern", TrackKind::Video}, {"test-second", "Second pattern", TrackKind::Video}};
    m.channels = {{ChannelClass::Control}, {ChannelClass::Realtime}};
    m.config_schema = nlohmann::json{{"type", "object"},
                                     {"properties", {{"enabled", {{"type", "boolean"}}}, {"test_hooks", {{"type", "boolean"}}}}},
                                     {"additionalProperties", false}};
    m.consumers = {.peer = true, .backend = false};
    m.input_bearing = true; // the deadman-armed `drive` consumer takes the docs/10 lease
    return m;
}

void TestCapability::configure(const nlohmann::json& config, const SourceFactory&) { impl_->test_hooks = config.value("test_hooks", false); }

void TestCapability::session_attached(SessionContext& ctx, const nlohmann::json&) {
    auto& s = impl_->sessions[ctx.id()];
    s.ctx = &ctx;
    s.plugged = false;
    TrackSpec pattern = pattern_track();
    s.pattern = pattern.source.source;
    ctx.add_track(std::move(pattern));
}

void TestCapability::session_detached(const SessionId& id, DetachReason, std::string_view) { impl_->sessions.erase(id); }

void TestCapability::release_all_input(const SessionId& id) {
    auto it = impl_->sessions.find(id);
    if (it == impl_->sessions.end()) return;
    auto& s = it->second;
    if (s.deadman && !s.deadman->expired() && s.ctx) impl_->emit_deadman(*s.ctx, "expired", s);
    s.deadman.reset();
}

void TestCapability::on_message(SessionContext& ctx, const Envelope& msg) {
    auto& s = impl_->sessions[ctx.id()];
    s.ctx = &ctx;
    if (msg.type == "echo" && msg.kind == "request") {
        ctx.result(msg, nlohmann::json{{"ok", true}, {"echo", msg.payload}, {"t_agent", protocol::now_ms()}});
        return;
    }
    if (msg.type == "hotplug" && msg.kind == "request") {
        if (!impl_->test_hooks) {
            ctx.fail(msg, error_codes::capability_denied, "test hooks are disabled");
            return;
        }
        const bool plugged = msg.payload.value("plugged", false);
        s.plugged = plugged;
        std::vector<TrackSpec> set;
        TrackSpec p = pattern_track();
        p.source.source = s.pattern ? s.pattern : p.source.source;
        set.push_back(std::move(p));
        if (plugged) {
            TrackSpec sec = second_track();
            if (!s.second) s.second = sec.source.source;
            sec.source.source = s.second;
            set.push_back(std::move(sec));
        }
        ctx.update_tracks(std::move(set));
        ctx.result(msg, nlohmann::json{{"ok", true}, {"manifest_version", ctx.manifest_version() + 1}});
        return;
    }
    if (msg.type == "silence" && msg.kind == "request") {
        if (!impl_->test_hooks) {
            ctx.fail(msg, error_codes::capability_denied, "test hooks are disabled");
            return;
        }
        auto* impl = dynamic_cast<core::SessionContextImpl*>(&ctx);
        if (!impl) {
            ctx.fail(msg, error_codes::internal, "no test hooks on this context");
            return;
        }
        impl->test_silence(msg.payload.value("pings", false), msg.payload.value("media", false),
                           std::chrono::milliseconds(msg.payload.value("ms", 5000)));
        ctx.result(msg, nlohmann::json{{"ok", true}});
        return;
    }
    if (msg.type == "drive" && msg.kind == "event") {
        if (!s.deadman) {
            s.deadman = ctx.arm_deadman(DEADMAN_BUDGET, [this, id = ctx.id()] {
                auto it = impl_->sessions.find(id);
                if (it == impl_->sessions.end() || !it->second.ctx) return;
                impl_->emit_deadman(*it->second.ctx, "expired", it->second);
            });
            impl_->emit_deadman(ctx, "armed", s);
        } else {
            const bool was_expired = s.deadman->expired();
            s.deadman->feed();
            if (was_expired) impl_->emit_deadman(ctx, "fed", s);
        }
        return;
    }
    if (msg.kind == "request") ctx.fail(msg, error_codes::payload_invalid, "unknown fjarr.test request: " + msg.type);
}

void TestCapability::shutdown() { impl_->sessions.clear(); }

} // namespace fjarr
