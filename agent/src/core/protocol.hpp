#pragma once
// Wire codec: signaling messages and envelopes, validated the way the TS
// guards do (unknown types/fields ignored, a higher `v` refused).
// spec: docs/08-protocol.md#signaling · #envelope · #track-manifest · #versioning
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <fjarr/capability.hpp>
#include <fjarr/session_context.hpp>

namespace fjarr::protocol {

inline constexpr int PROTO_VERSION = 1;
inline constexpr std::size_t MAX_ENVELOPE_BYTES = 16 * 1024;

/// UUIDv7-shaped id: time-ordered, unique per process.
std::string new_event_id();
/// Unix milliseconds.
std::int64_t now_ms();

// ------------------------------------------------------------ envelopes

/// Parse and validate an envelope from a DataChannel text message; nullopt when invalid.
std::optional<Envelope> parse_envelope(std::string_view text);
nlohmann::json envelope_to_json(const Envelope& env);
/// Serialized envelope; throws FjarrError(payload-invalid) above MAX_ENVELOPE_BYTES.
std::string serialize_envelope(const Envelope& env);
bool valid_cap_name(std::string_view cap);

Envelope make_envelope(std::string cap, std::string type, std::string kind, nlohmann::json payload,
                       std::string event_id = "");

// ------------------------------------------------------------ signaling

struct TurnCredentials {
    std::vector<std::string> urls;
    std::string username;
    std::string credential;
    int ttl = 0;
};

struct CapabilityGrant {
    std::string name;
    nlohmann::json params = nlohmann::json::object();
};

/// A validated inbound signaling message (the common fields + the JSON body).
struct SignalingMessage {
    std::string type;
    std::string event_id;
    std::int64_t ts = 0;
    nlohmann::json body; // the full message
    std::string session_id; // when present
};

/// Structural validation per message type; nullopt when the message must be ignored.
/// `higher_major` is set when `v` is a newer major (answer payload-invalid).
std::optional<SignalingMessage> parse_signaling(std::string_view text, bool* higher_major = nullptr);

std::optional<TurnCredentials> parse_turn(const nlohmann::json& j);

/// webrtcbin's `add-turn-server` wants `turn(s)://user:pass@host:port`, but the URL a
/// server mints is the RFC 7065 form every browser takes — `turn:host:port`, with no
/// slashes. Both are accepted here and the credentials are URL-escaped into the result.
/// Returns an empty string for anything that is not a turn/turns URL, so the caller can
/// say so instead of dropping it silently (docs/02 deployment topologies: relay is the
/// path that matters most and the one nobody exercises until a customer is behind a
/// symmetric NAT). spec: docs/09-interfaces.md#a-session-grants
std::string turn_url_with_credentials(const std::string& url, const std::string& username, const std::string& credential);
std::optional<std::vector<CapabilityGrant>> parse_capabilities(const nlohmann::json& j);
std::optional<OperatorInfo> parse_operator(const nlohmann::json& j);

/// Common fields for an outbound message.
nlohmann::json signaling_base(std::string_view type);

/// One manifest entry (docs/08#track-manifest).
struct ManifestEntry {
    std::string track_id;
    std::string cap;
    TrackKind kind = TrackKind::Video;
    std::string label;
    std::string codec; // H264 | OPUS
    int pt = 96;
    std::string mid;
    std::optional<MonitorInfo> monitor;
};
nlohmann::json manifest_to_json(const std::vector<ManifestEntry>& tracks);
nlohmann::json monitor_to_json(const MonitorInfo& m);

} // namespace fjarr::protocol
