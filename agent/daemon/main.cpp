// fjarr-agent — the thin reference daemon: exactly what an embedder writes.
// spec: docs/09-interfaces.md#embedding
#include <cstdio>
#include <cstring>

#include <gst/gst.h>

#include <fjarr/fjarr.hpp>

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    // Line-buffered stdout so container logs stream live.
    std::setvbuf(stdout, nullptr, _IOLBF, 0);

    std::printf("fjarr-agent %s (GStreamer %s)\n", fjarr::version().c_str(),
                fjarr::gstreamer_version().c_str());

    const bool check_only =
        argc > 1 && std::strcmp(argv[1], "--check") == 0;

    std::printf("hardware H.264 encode: %s\n",
                fjarr::hardware_encode_available() ? "available"
                                                   : "UNAVAILABLE");
    if (check_only) {
        return 0;
    }

    fjarr::AgentConfig config;
    config.robot_id = std::getenv("FJARR_ROBOT_ID") != nullptr
                          ? std::getenv("FJARR_ROBOT_ID")
                          : "unconfigured";
    config.server_url = std::getenv("FJARR_SERVER_URL") != nullptr
                            ? std::getenv("FJARR_SERVER_URL")
                            : "ws://localhost:8080/ws";

    fjarr::Agent agent{std::move(config)};
    agent.run();
    return 0;
}
