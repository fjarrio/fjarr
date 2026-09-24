#include "protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <random>

#include <fjarr/errors.hpp>

#include "core/glib/raii.hpp"

namespace fjarr::protocol {

namespace {
bool is_str(const nlohmann::json& j, const char* k) { return j.contains(k) && j[k].is_string(); }
bool is_nonempty_str(const nlohmann::json& j, const char* k) { return is_str(j, k) && !j[k].get_ref<const std::string&>().empty(); }
bool is_int(const nlohmann::json& j, const char* k) { return j.contains(k) && j[k].is_number_integer(); }
bool is_obj(const nlohmann::json& j, const char* k) { return j.contains(k) && j[k].is_object(); }

std::string hex(std::uint64_t v, int digits) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%0*llx", digits, static_cast<unsigned long long>(v));
    return buf;
}
} // namespace

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

std::string new_event_id() {
    static std::atomic<std::uint32_t> counter{0};
    static thread_local std::mt19937_64 gen{std::random_device{}()};
    const std::uint64_t ms = static_cast<std::uint64_t>(now_ms());
    const std::uint64_t rand_a = (gen() & 0x0fff) | 0x7000; // version 7
    const std::uint64_t rand_b = (gen() & 0x3fffffffffffffffULL) | 0x8000000000000000ULL; // variant
    const std::uint32_t c = counter++;
    // time_high-time_low-ver_rand-var_rand-node
    return hex(ms >> 16, 8) + "-" + hex(ms & 0xffff, 4) + "-" + hex(rand_a, 4) + "-" + hex(rand_b >> 48, 4) + "-" +
           hex(((rand_b & 0xffffffffffffULL) ^ c), 12);
}

bool valid_cap_name(std::string_view cap) {
    // ^[a-z0-9]+(\.[a-z0-9-]+)+$
    if (cap.empty()) return false;
    std::size_t dots = 0;
    bool seg_started = false;
    bool first_seg = true;
    for (char ch : cap) {
        if (ch == '.') {
            if (!seg_started) return false;
            dots++;
            seg_started = false;
            first_seg = false;
            continue;
        }
        const bool ok = (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || (!first_seg && ch == '-');
        if (!ok) return false;
        seg_started = true;
    }
    return dots >= 1 && seg_started;
}

std::optional<Envelope> parse_envelope(std::string_view text) {
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    if (!j.contains("v") || !j["v"].is_number_integer() || j["v"].get<int>() != PROTO_VERSION) return std::nullopt;
    if (!is_nonempty_str(j, "cap") || !valid_cap_name(j["cap"].get_ref<const std::string&>())) return std::nullopt;
    if (!is_nonempty_str(j, "type") || !is_nonempty_str(j, "event_id") || !is_nonempty_str(j, "kind")) return std::nullopt;
    const auto& kind = j["kind"].get_ref<const std::string&>();
    if (kind != "request" && kind != "accept" && kind != "feedback" && kind != "result" && kind != "event") return std::nullopt;
    if (!j.contains("payload") || !j["payload"].is_object()) return std::nullopt;
    Envelope e;
    e.cap = j["cap"];
    e.type = j["type"];
    e.event_id = j["event_id"];
    e.kind = kind;
    e.payload = j["payload"];
    return e;
}

nlohmann::json envelope_to_json(const Envelope& env) {
    return nlohmann::json{{"v", PROTO_VERSION}, {"cap", env.cap}, {"type", env.type}, {"event_id", env.event_id},
                          {"kind", env.kind}, {"payload", env.payload.is_null() ? nlohmann::json::object() : env.payload}};
}

std::string serialize_envelope(const Envelope& env) {
    std::string text = envelope_to_json(env).dump();
    if (text.size() > MAX_ENVELOPE_BYTES)
        throw FjarrError(std::string(error_codes::payload_invalid), env.cap + "/" + env.type + ": envelope exceeds 16 KiB (docs/08)");
    return text;
}

Envelope make_envelope(std::string cap, std::string type, std::string kind, nlohmann::json payload, std::string event_id) {
    Envelope e;
    e.cap = std::move(cap);
    e.type = std::move(type);
    e.kind = std::move(kind);
    e.payload = payload.is_object() ? std::move(payload) : nlohmann::json::object();
    e.event_id = event_id.empty() ? new_event_id() : std::move(event_id);
    return e;
}

std::string turn_url_with_credentials(const std::string& url, const std::string& username, const std::string& credential) {
    const auto colon = url.find(':');
    if (colon == std::string::npos) return {};
    const std::string scheme = url.substr(0, colon);
    if (scheme != "turn" && scheme != "turns") return {};
    // `turn://host:port` (what webrtcbin wants) and `turn:host:port` (RFC 7065, what a
    // browser and therefore a server's config uses) differ only by the slashes.
    std::string rest = url.substr(colon + 1);
    if (rest.rfind("//", 0) == 0) rest = rest.substr(2);
    if (rest.empty()) return {};
    glib::GStrPtr user(g_uri_escape_string(username.c_str(), nullptr, FALSE));
    glib::GStrPtr pass(g_uri_escape_string(credential.c_str(), nullptr, FALSE));
    return scheme + "://" + user.get() + ":" + pass.get() + "@" + rest;
}

std::optional<TurnCredentials> parse_turn(const nlohmann::json& j) {
    if (!j.is_object() || !j.contains("urls") || !j["urls"].is_array() || !is_str(j, "username") || !is_str(j, "credential") ||
        !is_int(j, "ttl"))
        return std::nullopt;
    TurnCredentials t;
    for (const auto& u : j["urls"]) {
        if (!u.is_string()) return std::nullopt;
        if (!u.get_ref<const std::string&>().empty()) t.urls.push_back(u);
    }
    t.username = j["username"];
    t.credential = j["credential"];
    t.ttl = j["ttl"];
    return t;
}

std::optional<std::vector<CapabilityGrant>> parse_capabilities(const nlohmann::json& j) {
    if (!j.is_array()) return std::nullopt;
    std::vector<CapabilityGrant> out;
    for (const auto& c : j) {
        if (!c.is_object() || !is_nonempty_str(c, "name")) return std::nullopt;
        CapabilityGrant g;
        g.name = c["name"];
        if (c.contains("params")) {
            if (!c["params"].is_object()) return std::nullopt;
            g.params = c["params"];
        }
        out.push_back(std::move(g));
    }
    return out;
}

std::optional<OperatorInfo> parse_operator(const nlohmann::json& j) {
    if (!j.is_object() || !is_str(j, "id") || !is_str(j, "label")) return std::nullopt;
    return OperatorInfo{j["id"], j["label"]};
}

std::optional<SignalingMessage> parse_signaling(std::string_view text, bool* higher_major) {
    if (higher_major) *higher_major = false;
    nlohmann::json j = nlohmann::json::parse(text, nullptr, false);
    if (!j.is_object()) return std::nullopt;
    if (!j.contains("v") || !j["v"].is_number_integer()) return std::nullopt;
    if (j["v"].get<int>() != PROTO_VERSION) {
        if (higher_major && j["v"].get<int>() > PROTO_VERSION) *higher_major = true;
        return std::nullopt;
    }
    if (!is_str(j, "type") || !is_nonempty_str(j, "event_id") || !is_int(j, "ts")) return std::nullopt;
    SignalingMessage m;
    m.type = j["type"];
    m.event_id = j["event_id"];
    m.ts = j["ts"];
    if (is_str(j, "session_id")) m.session_id = j["session_id"];
    const auto& t = m.type;
    // Per-type structural checks (agent-bound types).
    if (t == "hello-ack") {
        if (!is_int(j, "proto_version")) return std::nullopt;
        if (j.contains("turn") && !parse_turn(j["turn"])) return std::nullopt;
    } else if (t == "session-request") {
        if (m.session_id.empty() || !parse_capabilities(j.value("capabilities", nlohmann::json())) ||
            !parse_operator(j.value("operator", nlohmann::json())))
            return std::nullopt;
        if (j.contains("turn") && !parse_turn(j["turn"])) return std::nullopt;
    } else if (t == "answer") {
        if (m.session_id.empty() || !is_str(j, "sdp")) return std::nullopt;
    } else if (t == "ice") {
        if (m.session_id.empty() || !is_str(j, "candidate") || !is_int(j, "sdp_mline_index")) return std::nullopt;
    } else if (t == "ice-restart" || t == "peer-gone") {
        if (m.session_id.empty()) return std::nullopt;
    } else if (t == "session-close") {
        if (m.session_id.empty() || !is_str(j, "reason")) return std::nullopt;
    } else if (t == "error") {
        if (!is_str(j, "code") || !is_str(j, "message")) return std::nullopt;
    } else if (t == "backend-stream") {
        if (!is_str(j, "capability") || !is_obj(j, "payload")) return std::nullopt;
    }
    m.body = std::move(j);
    return m;
}

nlohmann::json signaling_base(std::string_view type) {
    return nlohmann::json{{"v", PROTO_VERSION}, {"type", std::string(type)}, {"event_id", new_event_id()}, {"ts", now_ms()}};
}

nlohmann::json monitor_to_json(const MonitorInfo& m) {
    nlohmann::json j{{"id", m.id}, {"index", m.index}, {"primary", m.primary}, {"x", m.x}, {"y", m.y},
                     {"w", m.w},   {"h", m.h},         {"scale", m.scale}};
    if (!m.name.empty()) j["name"] = m.name;
    return j;
}

nlohmann::json manifest_to_json(const std::vector<ManifestEntry>& tracks) {
    nlohmann::json arr = nlohmann::json::array();
    for (const auto& t : tracks) {
        nlohmann::json j{{"track_id", t.track_id}, {"cap", t.cap}, {"kind", t.kind == TrackKind::Video ? "video" : "audio"},
                         {"label", t.label},       {"codec", t.codec}, {"pt", t.pt}, {"mid", t.mid}};
        j["monitor"] = t.monitor ? monitor_to_json(*t.monitor) : nlohmann::json(nullptr);
        arr.push_back(std::move(j));
    }
    return arr;
}

} // namespace fjarr::protocol
