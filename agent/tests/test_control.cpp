// The docs/10 control domains through the real SessionManager and Session router:
// several operators' sessions, envelopes routed as fjarr:control delivers them,
// replies and control-state read back from a recording control channel. No peer
// is needed — the router is what decides.
// spec: docs/10-security.md#session-ownership · docs/08-protocol.md#fjarr-core · #errors
//       docs/15-testing-strategy.md#safety-behaviors
#include <algorithm>
#include <thread>

#include <gtest/gtest.h>

#include <fjarr/agent.hpp>
#include <fjarr/errors.hpp>

#include "core/loop.hpp"
#include "core/session.hpp"
#include "core/session_manager.hpp"
#include "media/encoder.hpp"
#include "media/media_plane.hpp"
#include "media/sources.hpp"

using namespace fjarr;
using namespace fjarr::core;
using namespace std::chrono_literals;

namespace fjarr::core {
/// The test's way past the DataChannel: open a recording fjarr:control and route envelopes.
struct SessionTestAccess {
    struct Recording final : ChannelSender {
        std::vector<Envelope>* out;
        explicit Recording(std::vector<Envelope>* o) : out(o) {}
        void send(const Envelope& e) override { out->push_back(e); }
        bool send_binary(std::span<const std::byte>) override { return true; }
        std::size_t buffered_amount() const override { return 0; }
        void on_drain(std::function<void()>) override {}
    };
    static void open_control(Session& s, std::vector<Envelope>* out) {
        s.senders_["fjarr:control"] = std::make_unique<Recording>(out);
        s.control_open_ = true;
    }
    static void deliver(Session& s, const Envelope& e) { s.route(e, "fjarr:control"); }
    static void deliver_binary(Session& s, const std::string& label, const std::string& bytes) { s.on_channel_data(label, bytes); }
    static unsigned long dropped_control(const Session& s) { return s.dropped_control_; }
};
} // namespace fjarr::core

namespace {

/// A capability that records, per session, every message it is handed and every release.
struct Probe final : Capability {
    std::string name, domain;
    std::vector<std::string> inputs;
    std::vector<std::string>* log;
    Probe(std::string n, std::string d, std::vector<std::string> in, std::vector<std::string>* l)
        : name(std::move(n)), domain(std::move(d)), inputs(std::move(in)), log(l) {}
    CapabilityManifest manifest() const override {
        CapabilityManifest m;
        m.name = name;
        m.channels = {{ChannelClass::Control}, {ChannelClass::Realtime}, {ChannelClass::Bulk, BulkFraming::Raw}};
        m.input_bearing = true;
        m.control_domain = domain;
        m.control_inputs = inputs;
        return m;
    }
    void configure(const nlohmann::json&, const fjarr::SourceFactory&) override {}
    void session_attached(SessionContext&, const nlohmann::json&) override {}
    void session_detached(const SessionId&, DetachReason, std::string_view) override {}
    std::function<void()> on_release;
    void release_all_input(const SessionId& id) override {
        log->push_back(name + ":release:" + id);
        if (on_release) on_release();
    }
    void on_message(SessionContext& ctx, const Envelope& msg) override {
        log->push_back(name + ":" + msg.type + ":" + ctx.id());
        if (msg.kind == "request") ctx.result(msg, nlohmann::json{{"ok", true}});
    }
    void on_binary(SessionContext& ctx, std::span<const std::byte>) override { log->push_back(name + ":binary:" + ctx.id()); }
    void shutdown() override {}
};

struct Rig {
    CoreLoop loop;
    AgentConfig config;
    media::SourceRegistry sources{loop.context()};
    std::unique_ptr<media::MediaPlane> plane;
    std::vector<std::string> log; // capability calls, in order, across every session
    Probe desk{"com.test.desk", "desktop", {}, &log};
    Probe drive{"com.test.drive", "motion", {"drive"}, &log};
    Probe shell{"com.test.shell", "", {}, &log};
    std::map<std::string, RegisteredCapability> registry;
    std::unique_ptr<SessionManager> mgr;
    std::map<std::string, std::vector<Envelope>> out; // session id → what its fjarr:control carried
    int seq = 0;

    Rig() {
        config.agent.robot_id = "t";
        config.media.encoder = "software";
        loop.start();
        for (Probe* p : {&desk, &drive, &shell}) registry[p->name] = RegisteredCapability{p, p->manifest(), true};
        loop.call_sync([&] {
            plane = std::make_unique<media::MediaPlane>(loop, config.media, media::EncoderChoice{media::EncoderKind::Software, "software"}, sources);
            SessionDeps deps;
            deps.loop = &loop;
            deps.plane = plane.get();
            deps.config = &config;
            deps.send_signal = [](nlohmann::json) {};
            deps.known_capability = [this](const std::string& n) { return registry.count(n) > 0; };
            mgr = std::make_unique<SessionManager>(deps, [this](const std::string& n) -> const RegisteredCapability* {
                auto it = registry.find(n);
                return it == registry.end() ? nullptr : &it->second;
            });
        });
    }
    ~Rig() {
        loop.call_sync([&] {
            mgr->close_all("test-teardown");
            mgr.reset();
            plane.reset();
        });
        loop.stop();
    }

    /// A session of `op` granted `caps` (name → params), with fjarr:control open.
    std::string open(const OperatorInfo& op, std::vector<std::pair<std::string, nlohmann::json>> caps) {
        const std::string id = "01a0ctrl-0000-7000-8000-00000000000" + std::to_string(++seq);
        nlohmann::json grants = nlohmann::json::array();
        for (auto& [n, p] : caps) grants.push_back({{"name", n}, {"params", p}});
        protocol::SignalingMessage m;
        m.type = "session-request";
        m.event_id = "e" + id;
        m.session_id = id;
        m.body = {{"session_id", id}, {"capabilities", grants}, {"operator", {{"id", op.id}, {"label", op.label}}}};
        loop.call_sync([&] {
            mgr->on_session_request(m);
            SessionTestAccess::open_control(*mgr->get(id), &out[id]);
        });
        return id;
    }
    void send(const std::string& sid, const std::string& cap, const std::string& type, const std::string& kind,
              nlohmann::json payload = nlohmann::json::object()) {
        loop.call_sync([&] {
            SessionTestAccess::deliver(*mgr->get(sid), Envelope{cap, type, "ev" + std::to_string(++seq), kind, std::move(payload)});
        });
    }
    /// The last result this session was sent, or null.
    nlohmann::json last_result(const std::string& sid) {
        nlohmann::json r;
        loop.call_sync([&] {
            for (auto it = out[sid].rbegin(); it != out[sid].rend(); ++it)
                if (it->kind == "result") {
                    r = it->payload;
                    break;
                }
        });
        return r;
    }
    /// The last control-state this session was sent, or null.
    nlohmann::json last_state(const std::string& sid) {
        nlohmann::json r;
        loop.call_sync([&] {
            for (auto it = out[sid].rbegin(); it != out[sid].rend(); ++it)
                if (it->type == "control-state") {
                    r = it->payload;
                    break;
                }
        });
        return r;
    }
    int count(const std::string& entry) {
        int n = 0;
        loop.call_sync([&] { n = static_cast<int>(std::count(log.begin(), log.end(), entry)); });
        return n;
    }
    std::vector<std::string> log_copy() {
        std::vector<std::string> l;
        loop.call_sync([&] { l = log; });
        return l;
    }
    void tick(ControlDomains::Clock::duration ahead) {
        loop.call_sync([&] { mgr->control_tick(ControlDomains::Clock::now() + ahead); });
    }
    void close(const std::string& sid, const std::string& reason = "operator-closed", bool retry = false) {
        loop.call_sync([&] { mgr->get(sid)->close(reason, retry, reason == "peer-gone"); });
        loop.call_sync([] {}); // the posted on_closed runs
        for (int i = 0; i < 200; i++) {
            bool gone = false;
            loop.call_sync([&] { gone = mgr->get(sid) == nullptr; });
            if (gone) return;
            std::this_thread::sleep_for(5ms);
        }
    }
    std::string holder(const std::string& domain) {
        std::string h;
        loop.call_sync([&] {
            if (const auto* x = mgr->control().holder(domain)) h = x->op.id;
        });
        return h;
    }
};

const OperatorInfo anna{"anna@example.com", "Anna"};
const OperatorInfo bob{"bob@example.com", "Bob"};
const nlohmann::json none = nlohmann::json::object();

} // namespace

TEST(ControlDomainsRouting, openingASessionClaimsNothingTheFirstInputDoes) {
    Rig r;
    const auto a = r.open(anna, {{"com.test.desk", none}});
    EXPECT_EQ(r.holder("desktop"), "") << "docs/10: claimed on engagement, not on session open";
    r.send(a, "com.test.desk", "pointer", "event");
    EXPECT_EQ(r.holder("desktop"), anna.id);
    EXPECT_EQ(r.count("com.test.desk:pointer:" + a), 1) << "the claiming input itself reaches the capability";
    auto st = r.last_state(a);
    ASSERT_TRUE(st.is_object());
    EXPECT_EQ(st["domains"]["desktop"]["holder"]["id"], anna.id);
    EXPECT_TRUE(st["domains"]["desktop"]["you"].get<bool>());
    EXPECT_TRUE(st["domains"]["desktop"]["since"].is_number_integer());
    EXPECT_FALSE(st["domains"].contains("motion")) << "only the domains this grant has a capability in";
}

TEST(ControlDomainsRouting, aNonHoldersEventsAreDroppedAndCountedAndRequestsAnsweredControlHeld) {
    Rig r;
    const auto a = r.open(anna, {{"com.test.desk", none}});
    const auto b = r.open(bob, {{"com.test.desk", none}});
    r.send(a, "com.test.desk", "pointer", "event");
    r.send(b, "com.test.desk", "pointer", "event");
    EXPECT_EQ(r.count("com.test.desk:pointer:" + b), 0) << "a non-holder's input never reaches the capability";
    unsigned long dropped = 0;
    r.loop.call_sync([&] { dropped = SessionTestAccess::dropped_control(*r.mgr->get(b)); });
    EXPECT_EQ(dropped, 1u);
    r.send(b, "com.test.desk", "type-text", "request", {{"text", "hi"}});
    auto res = r.last_result(b);
    EXPECT_FALSE(res.value("ok", true));
    EXPECT_EQ(res["error"]["code"], "control-held");
    EXPECT_EQ(res["error"]["data"]["domain"], "desktop");
    EXPECT_EQ(res["error"]["data"]["holder"]["id"], anna.id);
    EXPECT_EQ(res["error"]["data"]["holder"]["label"], "Anna");
    EXPECT_TRUE(res["error"]["data"]["since"].is_number_integer());
    // Bob was told who holds it, and that it is not him.
    auto st = r.last_state(b);
    EXPECT_EQ(st["domains"]["desktop"]["holder"]["label"], "Anna");
    EXPECT_FALSE(st["domains"]["desktop"]["you"].get<bool>());
}

TEST(ControlDomainsRouting, domainsAreIndependentAndCapabilitiesWithoutOneAreNeverGated) {
    Rig r;
    const auto a = r.open(anna, {{"com.test.desk", none}, {"com.test.drive", none}, {"com.test.shell", none}});
    const auto b = r.open(bob, {{"com.test.desk", none}, {"com.test.drive", none}, {"com.test.shell", none}});
    r.send(a, "com.test.desk", "key", "event");
    r.send(b, "com.test.drive", "drive", "event");
    EXPECT_EQ(r.holder("desktop"), anna.id);
    EXPECT_EQ(r.holder("motion"), bob.id);
    // A terminal each, side by side, whoever holds what.
    r.send(a, "com.test.shell", "open", "request");
    r.send(b, "com.test.shell", "open", "request");
    EXPECT_TRUE(r.last_result(a).value("ok", false));
    EXPECT_TRUE(r.last_result(b).value("ok", false));
    r.loop.call_sync([&] { SessionTestAccess::deliver_binary(*r.mgr->get(b), "fjarr:bulk:com.test.shell", "ls\n"); });
    EXPECT_EQ(r.count("com.test.shell:binary:" + b), 1);
}

TEST(ControlDomainsRouting, onlyTheDeclaredInputTypesClaimOrAreGated) {
    Rig r;
    const auto a = r.open(anna, {{"com.test.drive", none}});
    const auto b = r.open(bob, {{"com.test.drive", none}});
    r.send(b, "com.test.drive", "echo", "request"); // fjarr.test's echo: not input, claims nothing
    EXPECT_EQ(r.holder("motion"), "");
    EXPECT_TRUE(r.last_result(b).value("ok", false));
    r.send(a, "com.test.drive", "drive", "event");
    EXPECT_EQ(r.holder("motion"), anna.id);
    r.send(b, "com.test.drive", "echo", "request");
    EXPECT_TRUE(r.last_result(b).value("ok", false)) << "a non-holder may still use what is not input";
    // A binary message is not in {"drive"}: not gated either.
    r.loop.call_sync([&] { SessionTestAccess::deliver_binary(*r.mgr->get(b), "fjarr:bulk:com.test.drive", "x"); });
    EXPECT_EQ(r.count("com.test.drive:binary:" + b), 1);
    // Where every message is input (desktop), binary is gated too.
    const auto c = r.open(anna, {{"com.test.desk", none}});
    const auto d = r.open(bob, {{"com.test.desk", none}});
    r.send(c, "com.test.desk", "pointer", "event");
    r.loop.call_sync([&] { SessionTestAccess::deliver_binary(*r.mgr->get(d), "fjarr:bulk:com.test.desk", "x"); });
    EXPECT_EQ(r.count("com.test.desk:binary:" + d), 0);
}

TEST(ControlDomainsRouting, theSameOperatorsSessionsShareOneClaim) {
    Rig r;
    const auto a1 = r.open(anna, {{"com.test.desk", none}});
    const auto a2 = r.open(anna, {{"com.test.desk", none}}); // presentation mode: a window per monitor
    const auto b = r.open(bob, {{"com.test.desk", none}});
    r.send(a1, "com.test.desk", "pointer", "event");
    r.send(a2, "com.test.desk", "key", "event");
    EXPECT_EQ(r.count("com.test.desk:key:" + a2), 1) << "docs/10: all of the holder's sessions may send input";
    EXPECT_TRUE(r.last_state(a2)["domains"]["desktop"]["you"].get<bool>());
    EXPECT_FALSE(r.last_state(b)["domains"]["desktop"]["you"].get<bool>());
    // Closing one of Anna's windows keeps the claim; closing the last frees it at once (no 30 s wait).
    r.close(a1);
    EXPECT_EQ(r.holder("desktop"), anna.id);
    r.close(a2);
    EXPECT_EQ(r.holder("desktop"), "");
    auto st = r.last_state(b);
    EXPECT_TRUE(st["domains"]["desktop"]["holder"].is_null()) << "control-state goes out when the holder leaves";
    r.send(b, "com.test.desk", "pointer", "event");
    EXPECT_EQ(r.holder("desktop"), bob.id);
}

TEST(ControlDomainsRouting, desktopFreesAfterFiveIdleSecondsAndTheNextToTypeTakesIt) {
    Rig r;
    const auto a = r.open(anna, {{"com.test.desk", none}});
    const auto b = r.open(bob, {{"com.test.desk", none}});
    r.send(a, "com.test.desk", "pointer", "event");
    r.tick(4s);
    EXPECT_EQ(r.holder("desktop"), anna.id);
    r.tick(6s);
    EXPECT_EQ(r.holder("desktop"), "");
    EXPECT_TRUE(r.last_state(b)["domains"]["desktop"]["holder"].is_null()) << "everyone is told it is free";
    EXPECT_EQ(r.count("com.test.desk:release:" + a), 0) << "an idle holder is not released until someone takes over";
    r.send(b, "com.test.desk", "pointer", "event");
    EXPECT_EQ(r.holder("desktop"), bob.id);
    auto l = r.log_copy();
    auto rel = std::find(l.begin(), l.end(), "com.test.desk:release:" + a);
    auto ptr = std::find(l.begin(), l.end(), "com.test.desk:pointer:" + b);
    ASSERT_NE(rel, l.end()) << "Anna's held keys are released when Bob takes the desktop";
    EXPECT_LT(rel, ptr) << "before Bob's first input";
}

TEST(ControlDomainsRouting, motionNeverIdlesOut) {
    Rig r;
    const auto a = r.open(anna, {{"com.test.drive", none}});
    const auto b = r.open(bob, {{"com.test.drive", none}});
    r.send(a, "com.test.drive", "drive", "event");
    r.send(a, "fjarr.core", "ping", "request", {{"t0", 1}});
    r.tick(20s);
    EXPECT_EQ(r.holder("motion"), anna.id);
    r.send(b, "com.test.drive", "drive", "event");
    EXPECT_EQ(r.count("com.test.drive:drive:" + b), 0);
}

TEST(ControlDomainsRouting, takeControlOfMotionStopsTheRobotBeforeTheNewHolderHearsOk) {
    Rig r;
    const auto a = r.open(anna, {{"com.test.drive", none}});
    const auto b = r.open(bob, {{"com.test.drive", none}});
    r.send(a, "com.test.drive", "drive", "event");
    bool answered_before_release = false;
    r.loop.call_sync([&] {
        r.drive.on_release = [&] {
            for (const auto& e : r.out[b])
                if (e.type == "take-control") answered_before_release = true;
        };
    });
    r.send(b, "fjarr.core", "take-control", "request", {{"domain", "motion"}});
    r.loop.call_sync([&] { r.drive.on_release = nullptr; }); // it captures this test's locals; teardown releases too
    EXPECT_TRUE(r.last_result(b).value("ok", false));
    EXPECT_EQ(r.holder("motion"), bob.id);
    EXPECT_EQ(r.count("com.test.drive:release:" + a), 1) << "docs/10: nobody inherits a robot in motion";
    EXPECT_FALSE(answered_before_release) << "docs/08: the old holder's input is released before the result is sent";
    r.send(b, "com.test.drive", "drive", "event");
    auto l = r.log_copy();
    EXPECT_LT(std::find(l.begin(), l.end(), "com.test.drive:release:" + a), std::find(l.begin(), l.end(), "com.test.drive:drive:" + b));
    // Anna is told, and her next command is refused.
    auto st = r.last_state(a);
    EXPECT_EQ(st["domains"]["motion"]["holder"]["label"], "Bob");
    EXPECT_FALSE(st["domains"]["motion"]["you"].get<bool>());
    r.send(a, "com.test.drive", "drive", "event");
    EXPECT_EQ(r.count("com.test.drive:drive:" + a), 1) << "only the one before the takeover";
}

TEST(ControlDomainsRouting, takeControlOfTheDesktopFromAnActiveHolder) {
    Rig r;
    const auto a = r.open(anna, {{"com.test.desk", none}});
    const auto b = r.open(bob, {{"com.test.desk", none}});
    r.send(a, "com.test.desk", "pointer", "event");
    r.send(b, "fjarr.core", "take-control", "request", {{"domain", "desktop"}});
    EXPECT_TRUE(r.last_result(b).value("ok", false));
    EXPECT_EQ(r.holder("desktop"), bob.id);
    EXPECT_EQ(r.count("com.test.desk:release:" + a), 1);
    EXPECT_EQ(r.last_state(a)["domains"]["desktop"]["holder"]["label"], "Bob") << "the holder sees who took control";
}

TEST(ControlDomainsRouting, takeControlIsDeniedToViewOnlyAndToGrantsWithoutTheDomain) {
    Rig r;
    const auto v = r.open(bob, {{"com.test.desk", {{"view_only", true}}}});
    r.send(v, "fjarr.core", "take-control", "request", {{"domain", "desktop"}});
    EXPECT_EQ(r.last_result(v)["error"]["code"], "capability-denied");
    r.send(v, "fjarr.core", "take-control", "request", {{"domain", "motion"}});
    EXPECT_EQ(r.last_result(v)["error"]["code"], "capability-denied") << "no capability in motion";
    r.send(v, "fjarr.core", "take-control", "request", {{"domain", "terminal"}});
    EXPECT_EQ(r.last_result(v)["error"]["code"], "payload-invalid");
    r.send(v, "fjarr.core", "take-control", "request", nlohmann::json::object());
    EXPECT_EQ(r.last_result(v)["error"]["code"], "payload-invalid");
    EXPECT_EQ(r.holder("desktop"), "");
}

TEST(ControlDomainsRouting, aViewOnlyGrantNeverClaimsWhateverItsClientSends) {
    Rig r;
    const auto v = r.open(bob, {{"com.test.desk", {{"view_only", true}}}});
    r.send(v, "com.test.desk", "pointer", "event");
    r.send(v, "com.test.desk", "type-text", "request");
    EXPECT_EQ(r.holder("desktop"), "") << "docs/10: view_only never claims, even a free domain";
    EXPECT_EQ(r.count("com.test.desk:pointer:" + v), 0);
    EXPECT_EQ(r.last_result(v)["error"]["code"], "capability-denied");
    // It is still told who holds the desktop.
    const auto a = r.open(anna, {{"com.test.desk", none}});
    r.send(a, "com.test.desk", "pointer", "event");
    EXPECT_EQ(r.last_state(v)["domains"]["desktop"]["holder"]["id"], anna.id);
    EXPECT_TRUE(r.last_state(v)["domains"]["desktop"]["view_only"].get<bool>()) << "so the client shows no take-control button";
    EXPECT_FALSE(r.last_state(a)["domains"]["desktop"]["view_only"].get<bool>());
}

TEST(ControlDomainsRouting, releaseControlFreesAtOnceStopsTheRobotAndIsIdempotent) {
    Rig r;
    const auto a = r.open(anna, {{"com.test.drive", none}});
    const auto b = r.open(bob, {{"com.test.drive", none}});
    r.send(a, "com.test.drive", "drive", "event");
    r.send(b, "fjarr.core", "release-control", "request", {{"domain", "motion"}});
    EXPECT_TRUE(r.last_result(b).value("ok", false)) << "idempotent, and a non-holder releases nothing";
    EXPECT_EQ(r.holder("motion"), anna.id);
    r.send(a, "fjarr.core", "release-control", "request", {{"domain", "motion"}});
    EXPECT_TRUE(r.last_result(a).value("ok", false));
    EXPECT_EQ(r.holder("motion"), "");
    EXPECT_EQ(r.count("com.test.drive:release:" + a), 1);
    EXPECT_TRUE(r.last_state(b)["domains"]["motion"]["holder"].is_null());
    r.send(a, "fjarr.core", "release-control", "request", {{"domain", "motion"}});
    EXPECT_TRUE(r.last_result(a).value("ok", false));
}

TEST(ControlDomainsRouting, aStaleHolderFailsOpenAndIsReleased) {
    Rig r;
    const auto a = r.open(anna, {{"com.test.drive", none}});
    const auto b = r.open(bob, {{"com.test.drive", none}});
    r.send(a, "com.test.drive", "drive", "event");
    r.tick(29s);
    EXPECT_EQ(r.holder("motion"), anna.id);
    r.tick(31s);
    EXPECT_EQ(r.holder("motion"), "") << "docs/10: no heartbeat for 30 s loses the claim";
    EXPECT_EQ(r.count("com.test.drive:release:" + a), 1) << "and the robot stops (docs/15)";
    EXPECT_TRUE(r.last_state(b)["domains"]["motion"]["holder"].is_null());
    r.send(b, "com.test.drive", "drive", "event");
    EXPECT_EQ(r.holder("motion"), bob.id);
}

TEST(ControlDomainsRouting, theHoldersDisconnectFreesTheDomainAfterItsOwnRelease) {
    Rig r;
    const auto a = r.open(anna, {{"com.test.drive", none}});
    const auto b = r.open(bob, {{"com.test.drive", none}});
    r.send(a, "com.test.drive", "drive", "event");
    r.close(a, "peer-gone");
    EXPECT_EQ(r.count("com.test.drive:release:" + a), 1) << "release_all_input once, on the close path";
    EXPECT_EQ(r.holder("motion"), "");
    EXPECT_TRUE(r.last_state(b)["domains"]["motion"]["holder"].is_null());
}

TEST(ControlDomainsRouting, aRestartKeepsTheClaimAcrossTheGapStillBoundedByThirtySeconds) {
    Rig r;
    const auto a = r.open(anna, {{"com.test.drive", none}});
    const auto b = r.open(bob, {{"com.test.drive", none}});
    r.send(a, "com.test.drive", "drive", "event");
    r.close(a, "media-restart", /*retry=*/true);
    EXPECT_EQ(r.count("com.test.drive:release:" + a), 1) << "the robot still stops: release_all_input on every close path";
    EXPECT_EQ(r.holder("motion"), anna.id) << "docs/23: nobody else takes control during a restart";
    r.send(b, "com.test.drive", "drive", "event");
    EXPECT_EQ(r.count("com.test.drive:drive:" + b), 0);
    const auto a2 = r.open(anna, {{"com.test.drive", none}}); // the retried session
    r.send(a2, "com.test.drive", "drive", "event");
    EXPECT_EQ(r.count("com.test.drive:drive:" + a2), 1);
    // Had Anna never come back, the claim fails open like any other.
    r.close(a2, "media-restart", true);
    r.tick(31s);
    EXPECT_EQ(r.holder("motion"), "");
}

TEST(ControlDomainsRouting, statusDescribesTheHolders) {
    Rig r;
    const auto a = r.open(anna, {{"com.test.drive", none}});
    r.send(a, "com.test.drive", "drive", "event");
    nlohmann::json d;
    r.loop.call_sync([&] { d = r.mgr->describe(); });
    EXPECT_EQ(d["control"]["motion"]["holder"]["id"], anna.id);
    EXPECT_TRUE(d["control"]["desktop"]["holder"].is_null());
}
