// demo-robot — plays the role of a robot company's software embedding
// libfjarr. Public API only (the demo-as-integration-test rule, docs/02).
//
// M0: registers a fake telemetry-ish capability skeleton and idles.
// M1 turns this into the camera-streaming showcase against robot-sim.
#include <cstdio>
#include <cstdlib>
#include <memory>

#include <fjarr/fjarr.hpp>

namespace {

// A minimal stand-in capability, proving a third party can implement the
// interface with nothing but public headers.
class DemoTelemetryCapability final : public fjarr::Capability {
  public:
    fjarr::CapabilityManifest manifest() const override {
        fjarr::CapabilityManifest m;
        m.name = "com.example.demo-telemetry";
        m.version = {0, 1, 0};
        m.channels = {{fjarr::ChannelClass::Control}};
        m.consumers = {.peer = true, .backend = false};
        m.config_schema = {{"type", "object"}};
        return m;
    }
    void configure(const nlohmann::json&) override {}
    void session_attached(fjarr::SessionContext&,
                          const nlohmann::json&) override {}
    void session_detached(fjarr::SessionId, fjarr::DetachReason) override {}
    void on_message(fjarr::SessionContext&, const fjarr::Envelope&) override {}
    void shutdown() override {}
};

} // namespace

int main() {
    // Line-buffered stdout so container logs stream live.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    fjarr::AgentConfig config;
    config.robot_id = "demo-robot-01";
    config.server_url = std::getenv("FJARR_SERVER_URL") != nullptr
                            ? std::getenv("FJARR_SERVER_URL")
                            : "ws://localhost:8080/ws";

    fjarr::Agent agent{std::move(config)};
    agent.register_capability(std::make_unique<DemoTelemetryCapability>());
    agent.on_session_event([](const fjarr::SessionEvent& ev) {
        std::printf("demo-robot audit: session %llu %s\n",
                    static_cast<unsigned long long>(ev.id), ev.type.c_str());
    });
    agent.run();
    return 0;
}
