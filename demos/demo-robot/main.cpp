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

namespace {
const char* env_or(const char* name, const char* fallback) {
    const char* v = std::getenv(name);
    return v && *v ? v : fallback; // an empty variable (compose's `${X:-}`) means unset
}
/// The demo's webcam: FJARR_DEMO_WEBCAM (a by-id name or node), FJARR_DEMO_WEBCAM_FORMAT (mjpeg|yuyv → 1280x720@30).
nlohmann::json webcam_source() {
    nlohmann::json src{{"type", "v4l2"}, {"device", env_or("FJARR_DEMO_WEBCAM", "/dev/video0")}, {"format", env_or("FJARR_DEMO_WEBCAM_FORMAT", "auto")}};
    if (std::string(src["format"]) != "auto") {
        src["width"] = 1280;
        src["height"] = 720;
        src["fps"] = 30;
    }
    return src;
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    fjarr::AgentConfig config;
    config.agent.robot_id = "demo-robot-01";
    config.agent.allow_unsupervised = true;
    config.capabilities["fjarr.test"] = {{"enabled", true}, {"test_hooks", true}}; // the demo drives the hooks
    // fjarr.camera (docs/06): three tracks through the video source contract — a pattern, the lab's
    // RTSP simulator (rtsp-sim in docker-compose.yml), and the host webcam, which is only there
    // with docker-compose.camera.yml (docs/12); without it the track is unavailable, with its reason
    // in /sources, and absent from the manifest — the docs/26 missing-device behaviour.
    config.capabilities["fjarr.camera"] = {
        {"tracks",
         {{"pattern", {{"label", "Pattern (camera)"}, {"source", {{"type", "test"}, {"pattern", "ball"}, {"width", 1280}, {"height", 720}, {"fps", 30}}}}},
          // Passthrough (docs/06, slice 6b): the simulator's own H.264 is sent untouched — no decode, no
          // encoder on the robot — with its low-resolution mount as the thumbnail tier.
          {"rtsp",
           {{"label", "RTSP simulator"},
            {"source",
             {{"type", "rtsp"},
              {"url", env_or("FJARR_DEMO_RTSP_URL", "rtsp://rtsp-sim:8554/pattern")},
              {"thumbnail_url", env_or("FJARR_DEMO_RTSP_THUMBNAIL_URL", "rtsp://rtsp-sim:8554/pattern-low")},
              {"passthrough", true},
              {"latency", 200},
              {"protocols", "tcp"}}}}},
          {"webcam", {{"label", "Webcam"}, {"source", webcam_source()}}}}}};
    config.apply_env(); // FJARR_SERVER_URL, FJARR_DEV_DEVICE_TOKEN, FJARR_MEDIA_ENCODER, FJARR_ROBOT_ID …
    try {
        config.validate();
    } catch (const fjarr::FjarrError& e) {
        std::fprintf(stderr, "demo-robot: %s\n", e.what());
        return 1;
    }
    fjarr::Agent agent{config};
    agent.register_capability(std::make_unique<fjarr::TestCapability>());
    agent.register_capability(std::make_unique<fjarr::CameraCapability>());
    agent.on_session_event([](const fjarr::SessionEvent& ev) {
        std::printf("demo-robot audit: session %s %s operator=%s %s\n", fjarr::short_session_id(ev.session_id).c_str(), ev.type.c_str(),
                    ev.operator_info.label.c_str(), ev.reason.c_str());
    });
    agent.stop_on_signal(SIGTERM); // orderly: input released, sessions told agent-shutdown (docs/15)
    agent.stop_on_signal(SIGINT);
    return agent.run();
}
