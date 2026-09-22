// AgentConfig: TOML (toml++) + FJARR_* environment overrides.
// spec: docs/23-agent-core-architecture.md#configuration
#include <cstdlib>
#include <fstream>
#include <sstream>

#include <toml++/toml.hpp>

#include <fjarr/agent.hpp>
#include <fjarr/errors.hpp>

namespace fjarr {

namespace {

nlohmann::json toml_to_json(const toml::node& node) {
    if (auto v = node.as_table()) {
        nlohmann::json out = nlohmann::json::object();
        for (const auto& [k, val] : *v) out[std::string(k.str())] = toml_to_json(val);
        return out;
    }
    if (auto v = node.as_array()) {
        nlohmann::json out = nlohmann::json::array();
        for (const auto& val : *v) out.push_back(toml_to_json(val));
        return out;
    }
    if (auto v = node.as_string()) return v->get();
    if (auto v = node.as_integer()) return v->get();
    if (auto v = node.as_floating_point()) return v->get();
    if (auto v = node.as_boolean()) return v->get();
    return nullptr;
}

template <class T> void read(const toml::table* t, const char* key, T& out) {
    if (!t) return;
    if (auto v = t->get(key)) {
        if (auto x = v->value<T>()) out = *x;
        else throw FjarrError("config", std::string("wrong type for ") + key);
    }
}

const char* env(const char* name) {
    const char* v = std::getenv(name);
    return (v && *v) ? v : nullptr;
}

void env_str(const char* name, std::string& out) {
    if (const char* v = env(name)) out = v;
}
void env_int(const char* name, int& out) {
    if (const char* v = env(name)) {
        char* end = nullptr;
        const long n = std::strtol(v, &end, 10);
        if (*v == '\0' || (end && *end != '\0')) throw FjarrError("config", std::string(name) + " must be an integer, got '" + v + "'");
        out = static_cast<int>(n);
    }
}
void env_bool(const char* name, bool& out) {
    if (const char* v = env(name)) {
        std::string s = v;
        out = (s == "1" || s == "true" || s == "yes" || s == "on");
    }
}

} // namespace

AgentConfig AgentConfig::from_toml(const std::string& text) {
    toml::table tbl;
    try {
        tbl = toml::parse(text);
    } catch (const toml::parse_error& e) {
        throw FjarrError("config", std::string("TOML parse error: ") + e.description().data());
    }
    AgentConfig c;
    const toml::table* agent = tbl["agent"].as_table();
    read(agent, "robot_id", c.agent.robot_id);
    read(agent, "server_url", c.agent.server_url);
    read(agent, "credential_file", c.agent.credential_file);
    read(agent, "dev_token", c.agent.dev_token);
    read(agent, "ice_policy", c.agent.ice_policy);
    read(agent, "log_level", c.agent.log_level);
    read(agent, "log_format", c.agent.log_format);
    read(agent, "dot_dir", c.agent.dot_dir);
    {
        std::int64_t v = c.agent.watchdog_secs;
        read(agent, "watchdog_secs", v);
        c.agent.watchdog_secs = static_cast<int>(v);
    }
    read(agent, "allow_unsupervised", c.agent.allow_unsupervised);

    const toml::table* media = tbl["media"].as_table();
    read(media, "encoder", c.media.encoder);
    std::int64_t i64 = 0;
    i64 = c.media.gop_seconds; read(media, "gop_seconds", i64); c.media.gop_seconds = static_cast<int>(i64);
    i64 = c.media.active_kbps; read(media, "active_kbps", i64); c.media.active_kbps = static_cast<int>(i64);
    i64 = c.media.thumbnail_kbps; read(media, "thumbnail_kbps", i64); c.media.thumbnail_kbps = static_cast<int>(i64);
    i64 = c.media.tier_grace_ms; read(media, "tier_grace_ms", i64); c.media.tier_grace_ms = static_cast<int>(i64);

    const toml::table* intro = tbl["introspect"].as_table();
    read(intro, "enabled", c.introspect.enabled);
    read(intro, "bind", c.introspect.bind);
    i64 = c.introspect.port; read(intro, "port", i64); c.introspect.port = static_cast<int>(i64);
    read(intro, "socket", c.introspect.socket);
    read(intro, "token", c.introspect.token);
    read(intro, "viewer_dir", c.introspect.viewer_dir);
    i64 = c.introspect.history; read(intro, "history", i64); c.introspect.history = static_cast<int>(i64);

    if (const toml::table* caps = tbl["capabilities"].as_table()) {
        for (const auto& [name, node] : *caps) {
            if (!node.is_table()) throw FjarrError("config", "capabilities." + std::string(name.str()) + " must be a table");
            c.capabilities[std::string(name.str())] = toml_to_json(node);
        }
    }
    return c;
}

AgentConfig AgentConfig::from_file(const std::string& path) {
    std::ifstream in(path);
    if (!in) throw FjarrError("config", "cannot read " + path);
    std::stringstream ss;
    ss << in.rdbuf();
    return from_toml(ss.str());
}

void AgentConfig::apply_env() {
    // Long forms FJARR_<SECTION>_<KEY> and the short aliases named in docs/23.
    env_str("FJARR_AGENT_ROBOT_ID", agent.robot_id);
    env_str("FJARR_ROBOT_ID", agent.robot_id);
    env_str("FJARR_AGENT_SERVER_URL", agent.server_url);
    env_str("FJARR_SERVER_URL", agent.server_url);
    env_str("FJARR_AGENT_CREDENTIAL_FILE", agent.credential_file);
    env_str("FJARR_AGENT_DEV_TOKEN", agent.dev_token);
    env_str("FJARR_DEV_DEVICE_TOKEN", agent.dev_token);
    env_str("FJARR_AGENT_ICE_POLICY", agent.ice_policy);
    env_str("FJARR_ICE_POLICY", agent.ice_policy);
    env_str("FJARR_AGENT_LOG_LEVEL", agent.log_level);
    env_str("FJARR_LOG_LEVEL", agent.log_level);
    env_str("FJARR_AGENT_LOG_FORMAT", agent.log_format);
    env_str("FJARR_LOG_FORMAT", agent.log_format);
    env_str("FJARR_AGENT_DOT_DIR", agent.dot_dir);
    env_str("FJARR_DOT_DIR", agent.dot_dir);
    env_int("FJARR_AGENT_WATCHDOG_SECS", agent.watchdog_secs);
    env_bool("FJARR_AGENT_ALLOW_UNSUPERVISED", agent.allow_unsupervised);
    env_str("FJARR_MEDIA_ENCODER", media.encoder);
    env_int("FJARR_MEDIA_GOP_SECONDS", media.gop_seconds);
    env_int("FJARR_MEDIA_ACTIVE_KBPS", media.active_kbps);
    env_int("FJARR_MEDIA_THUMBNAIL_KBPS", media.thumbnail_kbps);
    env_int("FJARR_MEDIA_TIER_GRACE_MS", media.tier_grace_ms);
    env_bool("FJARR_INTROSPECT_ENABLED", introspect.enabled);
    env_str("FJARR_INTROSPECT_BIND", introspect.bind);
    env_int("FJARR_INTROSPECT_PORT", introspect.port);
    env_str("FJARR_INTROSPECT_SOCKET", introspect.socket);
    env_str("FJARR_INTROSPECT_TOKEN", introspect.token);
    env_str("FJARR_INTROSPECT_VIEWER_DIR", introspect.viewer_dir);
    env_int("FJARR_INTROSPECT_HISTORY", introspect.history);
    // Test capability hooks (the demo and CI): FJARR_TEST_HOOKS=1
    if (const char* v = env("FJARR_TEST_HOOKS")) {
        std::string s = v;
        auto& t = capabilities["fjarr.test"];
        if (!t.is_object()) t = nlohmann::json::object();
        t["enabled"] = true;
        t["test_hooks"] = (s == "1" || s == "true");
    }
}

void AgentConfig::validate() const {
    if (agent.robot_id.empty()) throw FjarrError("config", "agent.robot_id is required (or FJARR_ROBOT_ID)");
    if (agent.server_url.rfind("ws://", 0) != 0 && agent.server_url.rfind("wss://", 0) != 0)
        throw FjarrError("config", "agent.server_url must be ws:// or wss://");
    if (agent.ice_policy != "all" && agent.ice_policy != "relay") throw FjarrError("config", "agent.ice_policy must be all|relay");
    if (agent.log_format != "text" && agent.log_format != "json") throw FjarrError("config", "agent.log_format must be text|json");
    if (media.encoder != "auto" && media.encoder != "vaapi" && media.encoder != "software")
        throw FjarrError("config", "media.encoder must be auto|vaapi|software");
    if (media.gop_seconds < 1 || media.gop_seconds > 30) throw FjarrError("config", "media.gop_seconds out of range");
    if (agent.log_level != "debug" && agent.log_level != "info" && agent.log_level != "warn" && agent.log_level != "error")
        throw FjarrError("config", "agent.log_level must be debug|info|warn|error");
    if (introspect.port < 0 || introspect.port > 65535) throw FjarrError("config", "introspect.port out of range");
    if (agent.watchdog_secs < 0) throw FjarrError("config", "agent.watchdog_secs must be >= 0");
    if (introspect.enabled && introspect.bind != "127.0.0.1" && introspect.bind != "localhost" && introspect.token.empty() &&
        introspect.socket.empty())
        throw FjarrError("config", "introspect.bind on a non-loopback address requires introspect.token (docs/24)");
}

} // namespace fjarr
