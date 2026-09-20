#pragma once
// The embedding surface: how a robot application hosts Fjarr in-process.
// spec: docs/09-interfaces.md#embedding
// spec: docs/23-agent-core-architecture.md#configuration (AgentConfig)
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

#include <fjarr/capability.hpp>
#include <fjarr/session_context.hpp>
#include <fjarr/video_source.hpp>

namespace fjarr {

/// `fjarr.toml` (docs/23#configuration) with `FJARR_*` environment overrides winning.
struct AgentConfig {
    struct AgentSection {
        std::string robot_id;
        std::string server_url = "ws://localhost:8080/ws";
        std::string credential_file; // M5 device key
        std::string dev_token;       // or FJARR_DEV_DEVICE_TOKEN (env wins); never logged
        std::string ice_policy = "all"; // all | relay
        std::string log_level = "info";
        std::string log_format = "text"; // text | json
        std::string dot_dir;             // snapshots as files (docs/24)
        int watchdog_secs = 0;           // 0 = WatchdogSec/3 from the unit
        bool allow_unsupervised = false;
    } agent;

    struct MediaSection {
        std::string encoder = "auto"; // auto | vaapi | software — never a silent fallback
        int gop_seconds = 2;
        int active_kbps = 4000;
        int thumbnail_kbps = 300;
        int tier_grace_ms = 10000;
    } media;

    struct IntrospectSection {
        bool enabled = true;
        std::string bind = "127.0.0.1";
        int port = 7381;
        std::string socket;
        std::string token;
        int history = 64;
    } introspect;

    /// `[capabilities."<name>"]` tables, validated against each capability's schema.
    std::map<std::string, nlohmann::json> capabilities;

    /// Load a TOML file; throws FjarrError("config", …) on a malformed file.
    static AgentConfig from_file(const std::string& path);
    /// Parse TOML text (for tests and embedders).
    static AgentConfig from_toml(const std::string& text);
    /// Apply `FJARR_<SECTION>_<KEY>` overrides and the short aliases named
    /// in docs/23 (FJARR_ROBOT_ID, FJARR_SERVER_URL, FJARR_DEV_DEVICE_TOKEN,
    /// FJARR_LOG_LEVEL, FJARR_LOG_FORMAT, FJARR_DOT_DIR, FJARR_INTROSPECT_PORT…).
    void apply_env();
    /// Validate what can be validated without capabilities; throws FjarrError.
    void validate() const;
};

/// spec: docs/09-interfaces.md#embedding — the audit hook of docs/10.
struct SessionEvent {
    std::string type; // "started" | "ended" | "error" | "audio-uplink"
    SessionId session_id;
    OperatorInfo operator_info;
    std::string reason; // ended
    std::string code;   // error
};

/// Supervision seam (ADR-0019): sd_notify READY/WATCHDOG when running under
/// systemd; embedders may leave it unset.
struct Supervision {
    std::function<void()> ready;
    std::function<void()> watchdog;
    int watchdog_interval_ms = 0; // 0 = off
    int stop_deadline_ms = 3000;  // stop_on_signal(): orderly shutdown must finish within this, else _Exit(0)
};

/// Validate `instance` against a JSON Schema the way the agent validates capability config
/// (throws FjarrError("config", …) with the validator's message).
void validate_json_schema(const nlohmann::json& schema, const nlohmann::json& instance);

class Agent {
  public:
    explicit Agent(AgentConfig config);
    ~Agent();

    Agent(const Agent&) = delete;
    Agent& operator=(const Agent&) = delete;

    /// Register a capability plugin (docs/05). Must precede run()/start().
    void register_capability(std::unique_ptr<Capability> capability);
    /// Register a source type reachable from config (docs/09). Must precede run()/start().
    void register_source_type(SourceType type);
    /// Audit/session hook for the embedding application.
    void on_session_event(std::function<void(const SessionEvent&)> callback);
    /// systemd notify/watchdog seam (ADR-0019 addendum).
    void supervision(Supervision s);

    /// Blocking run (reference daemon style); returns the process exit code
    /// (0 clean, 1 configuration error, 2 "restart me").
    int run();
    /// …or host-loop integration: the core loop runs on its own thread.
    void start();
    void stop();
    /// Run an orderly stop() when `signum` (SIGTERM, SIGINT) arrives: the signal is
    /// delivered as a core-loop callback, so shutdown never runs in async-signal
    /// context, and `Supervision::stop_deadline_ms` bounds it. Call before run()/start().
    void stop_on_signal(int signum);

    /// The local introspection endpoint's port (0 when disabled). docs/24
    int introspect_port() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fjarr
