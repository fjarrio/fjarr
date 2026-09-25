// fjarr-agent — the thin reference daemon: exactly what an embedder writes.
// spec: docs/09-interfaces.md#embedding · docs/23-agent-core-architecture.md#configuration
//       ADR-0019 (supervision), docs/26 (--check, --probe-source)
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>

#include <gst/gst.h>

#include <fjarr/diagnostics.hpp>
#include <fjarr/fjarr.hpp>

#ifdef FJARR_HAVE_SYSTEMD
#include <systemd/sd-daemon.h>
#endif

namespace {

int usage() {
    std::printf("fjarr-agent [--config /etc/fjarr/fjarr.toml] [--check] [--probe-source '<description|type>'] [--diagnostics [out.tar.gz]]\n"
                "  --check          the doctor: encoder, configured sources, endpoint (exit 0/1)\n"
                "  --probe-source   bring one source up standalone and report caps + fps\n"
                "  --diagnostics    write the support bundle (docs/24) from the running agent's endpoint, or an offline one\n"
                "env: FJARR_ROBOT_ID FJARR_SERVER_URL FJARR_DEV_DEVICE_TOKEN FJARR_MEDIA_ENCODER FJARR_TEST_HOOKS … (docs/23)\n");
    return 2;
}

int probe_source(const std::string& spec) {
    // docs/09: negotiated caps, measured fps, bus errors — no server, no browser.
    const fjarr::ProbeResult r = fjarr::probe_source(spec);
    std::printf("probe: source  %s\nprobe: caps    %s\nprobe: memory  %s\nprobe: frames  %d in 2 s (%.1f fps)\n", r.description.c_str(),
                r.caps.empty() ? "(none negotiated)" : r.caps.c_str(), r.memory.empty() ? "(unknown)" : r.memory.c_str(), r.frames, r.fps);
    // Passthrough (docs/06): the track costs no encoder, and the browser has to accept this codec.
    if (r.passthrough)
        std::printf("probe: encoded %s (passthrough: parsed and packetized, never transcoded)\n", r.codec.empty() ? "(codec unknown)" : r.codec.c_str());
    if (!r.error.empty()) std::printf("probe: ERROR   %s\n", r.error.c_str());
    std::printf("probe: result  %s\n", r.ok ? "OK" : "FAILED");
    return r.ok ? 0 : 1;
}

} // namespace

int main(int argc, char** argv) {
    gst_init(&argc, &argv);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::string config_path;
    bool check_only = false;
    std::string probe;
    bool diagnostics = false;
    std::string diagnostics_out = "fjarr-diagnostics.tar.gz";
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--config" && i + 1 < argc) config_path = argv[++i];
        else if (a == "--check") check_only = true;
        else if (a == "--probe-source" && i + 1 < argc) probe = argv[++i];
        else if (a == "--diagnostics") {
            diagnostics = true;
            if (i + 1 < argc && argv[i + 1][0] != '-') diagnostics_out = argv[++i];
        }
        else if (a == "--help" || a == "-h") return usage();
        else return usage();
    }
    std::printf("fjarr-agent %s (GStreamer %s)\n", fjarr::version().c_str(), fjarr::gstreamer_version().c_str());
    if (!probe.empty()) return probe_source(probe);

    fjarr::AgentConfig config;
    try {
        if (!config_path.empty()) config = fjarr::AgentConfig::from_file(config_path);
        config.apply_env();
        if (config.capabilities.find("fjarr.test") == config.capabilities.end())
            config.capabilities["fjarr.test"] = nlohmann::json{{"enabled", true}, {"test_hooks", false}}; // the install smoke test (docs/06)
        config.validate();
    } catch (const fjarr::FjarrError& e) {
        std::fprintf(stderr, "fjarr-agent: %s\n", e.what());
        return 1;
    }
    if (diagnostics) {
        const fjarr::DiagnosticsResult r = fjarr::write_diagnostics(config, diagnostics_out);
        std::printf("diagnostics: %s\n", r.message.c_str());
        return r.ok ? 0 : 1;
    }
    const bool hw = fjarr::hardware_encode_available();
    std::printf("hardware H.264 encode: %s (media.encoder = %s)\n", hw ? "available" : "UNAVAILABLE", config.media.encoder.c_str());
    if (check_only) {
        bool ok = config.media.encoder == "software" || hw;
        // docs/23: the doctor gains a row per configured source.
        if (config.capabilities.count("fjarr.camera")) {
            fjarr::CameraCapability cam;
            try {
                fjarr::validate_json_schema(cam.manifest().config_schema, config.capabilities["fjarr.camera"]); // as the agent does at start
                cam.configure(config.capabilities["fjarr.camera"], *fjarr::builtin_source_factory());
            } catch (const std::exception& e) {
                std::printf("source: %s\n", e.what());
                ok = false;
            }
            for (const auto& t : cam.configured_sources())
                std::printf("source %-16s %-12s %s%s%s\n", t.track_id.c_str(), t.available ? "available" : "UNAVAILABLE", t.identity.c_str(),
                            t.required ? " (required)" : "", t.reason.empty() ? "" : (" — " + t.reason).c_str());
        }
        // The desktop is always present (ADR-0021), so it always gets a row — the whole point is
        // that "can this robot share its screen?" has an answer on every machine, and that the
        // answer names the package when it is no. Never a reason to fail the check: a robot with
        // no desktop is the normal case.
        {
            fjarr::DesktopCapability desk;
            const auto table = config.capabilities.count("fjarr.desktop") ? config.capabilities["fjarr.desktop"] : nlohmann::json::object();
            try {
                fjarr::validate_json_schema(desk.manifest().config_schema, table);
                desk.configure(table, *fjarr::builtin_source_factory());
                for (const auto& t : desk.configured_sources())
                    std::printf("desktop %-16s %-12s %s%s\n", t.track_id.c_str(), t.available ? "available" : "unavailable", t.identity.c_str(),
                                t.reason.empty() ? "" : (" — " + t.reason).c_str());
            } catch (const std::exception& e) {
                std::printf("desktop: %s\n", e.what());
                ok = false; // a *malformed* desktop config is a real configuration error
            }
        }
        const bool encoder_ok = config.media.encoder == "software" || hw;
        std::printf("check: %s\n", ok ? "OK" : (encoder_ok ? "FAILED — see the source rows above" : "FAILED — VA-API unavailable; set media.encoder = \"software\" or fix /dev/dri"));
        return ok ? 0 : 1;
    }

    fjarr::Agent agent{config};
    if (config.capabilities["fjarr.test"].value("enabled", true)) agent.register_capability(std::make_unique<fjarr::TestCapability>());
    if (config.capabilities.count("fjarr.camera")) agent.register_capability(std::make_unique<fjarr::CameraCapability>()); // tracks from fjarr.toml (docs/06)
    // Off unless the table exists AND names a user — there is deliberately no default account (docs/10#terminal).
    if (config.capabilities.count("fjarr.terminal")) agent.register_capability(std::make_unique<fjarr::TerminalCapability>());
    agent.on_session_event([](const fjarr::SessionEvent& ev) {
        std::printf("audit: session %s %s operator=%s %s\n", fjarr::short_session_id(ev.session_id).c_str(), ev.type.c_str(), ev.operator_info.label.c_str(),
                    ev.reason.c_str());
    });
#ifdef FJARR_HAVE_SYSTEMD
    {
        fjarr::Supervision sup;
        uint64_t usec = 0;
        const bool under_systemd = sd_watchdog_enabled(0, &usec) > 0 || std::getenv("NOTIFY_SOCKET") != nullptr;
        if (under_systemd) {
            sup.ready = [] { sd_notify(0, "READY=1"); };
            if (usec > 0) {
                sup.watchdog = [] { sd_notify(0, "WATCHDOG=1"); };
                // docs/23: watchdog_secs = 0 means WatchdogSec/3 from the unit; a value overrides it.
                sup.watchdog_interval_ms = config.agent.watchdog_secs > 0 ? config.agent.watchdog_secs * 1000 : static_cast<int>(usec / 1000 / 3);
            }
            agent.supervision(std::move(sup));
        } else if (!config.agent.allow_unsupervised && std::getenv("INVOCATION_ID") == nullptr) {
            // docs/23 configuration: the packaged agent refuses to start outside systemd unless told otherwise.
            std::fprintf(stderr, "fjarr-agent: not running under systemd; set agent.allow_unsupervised = true "
                                 "(or FJARR_AGENT_ALLOW_UNSUPERVISED=1) for containers and dev (ADR-0019)\n");
            return 1;
        }
    }
#endif
    // SIGTERM/SIGINT: sessions close with agent-shutdown, input released, exit 0 — bounded (docs/15).
    agent.stop_on_signal(SIGTERM);
    agent.stop_on_signal(SIGINT);
    return agent.run();
}
