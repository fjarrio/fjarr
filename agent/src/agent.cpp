#include <fjarr/agent.hpp>

#include <chrono>
#include <cstdio>
#include <thread>
#include <utility>
#include <vector>

namespace fjarr {

// M0 skeleton: holds registrations, logs, idles. The real core —
// signaling client, session lifecycle, FrameHub, DC router, reconnect
// ladder (docs/02-architecture.md) — is the M1 deliverable and is written
// only against docs/05, docs/08, docs/09.
struct Agent::Impl {
    AgentConfig config;
    std::vector<std::unique_ptr<Capability>> capabilities;
    std::function<void(const SessionEvent&)> session_callback;
    bool running = false;
};

AgentConfig AgentConfig::from_file(const std::string& path) {
    std::fprintf(stderr, "fjarr: AgentConfig::from_file(%s) — M1, not implemented\n",
                 path.c_str());
    return {};
}

Agent::Agent(AgentConfig config) : impl_(std::make_unique<Impl>()) {
    impl_->config = std::move(config);
}

Agent::~Agent() = default;

void Agent::register_capability(std::unique_ptr<Capability> capability) {
    std::printf("fjarr: registered capability %s\n",
                capability->manifest().name.c_str());
    impl_->capabilities.push_back(std::move(capability));
}

void Agent::on_session_event(std::function<void(const SessionEvent&)> callback) {
    impl_->session_callback = std::move(callback);
}

void Agent::start() { impl_->running = true; }
void Agent::stop() { impl_->running = false; }

void Agent::run() {
    start();
    std::printf("fjarr: agent skeleton alive (robot_id=%s, %zu capabilities) — "
                "core lands in M1 (docs/17-roadmap.md)\n",
                impl_->config.robot_id.c_str(), impl_->capabilities.size());
    while (impl_->running) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        std::printf("fjarr: heartbeat (skeleton)\n");
        std::fflush(stdout);
    }
}

} // namespace fjarr
