// demo-robot — plays the role of a robot company's software embedding
// libfjarr. Public API only (the demo-as-integration-test rule, docs/02).
//
// Slice 3b: registers fjarr::TestCapability through the public API (the
// install smoke test, docs/06) and streams the test pattern to the demo
// dashboard through fjarr-server.
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>

#include <fjarr/fjarr.hpp>

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    fjarr::AgentConfig config;
    config.agent.robot_id = "demo-robot-01";
    config.agent.allow_unsupervised = true;
    config.capabilities["fjarr.test"] = {{"enabled", true}, {"test_hooks", true}}; // the demo drives the hooks
    config.apply_env(); // FJARR_SERVER_URL, FJARR_DEV_DEVICE_TOKEN, FJARR_MEDIA_ENCODER, FJARR_ROBOT_ID …
    try {
        config.validate();
    } catch (const fjarr::FjarrError& e) {
        std::fprintf(stderr, "demo-robot: %s\n", e.what());
        return 1;
    }
    fjarr::Agent agent{config};
    agent.register_capability(std::make_unique<fjarr::TestCapability>());
    agent.on_session_event([](const fjarr::SessionEvent& ev) {
        std::printf("demo-robot audit: session %s %s operator=%s %s\n", fjarr::short_session_id(ev.session_id).c_str(), ev.type.c_str(),
                    ev.operator_info.label.c_str(), ev.reason.c_str());
    });
    agent.stop_on_signal(SIGTERM); // orderly: input released, sessions told agent-shutdown (docs/15)
    agent.stop_on_signal(SIGINT);
    return agent.run();
}
