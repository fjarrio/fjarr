#pragma once
// The capability plugin interface — the extension model's agent-side seam.
// spec: docs/05-extension-model.md#the-capability-contract-agent-side
// spec: docs/09-interfaces.md#the-capability-interface-agent-side
//
// M0 STATUS: interface only. The core that drives it lands in M1; the shape
// below is the docs/09 design source mirrored into code. ABI is NOT stable
// before M6 (docs/05#compatibility-rules).
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace fjarr {

struct SemVer {
    int major = 0, minor = 0, patch = 0;
};

/// A media track a capability can produce (announced in the track manifest).
/// spec: docs/08-protocol.md#track-manifest
struct TrackDecl {
    std::string track_id;   // stable id, e.g. "cam-front"
    std::string label;      // human label, e.g. "Front"
    std::string kind;       // "video" (audio: open question #3)
};

/// DataChannel classes a capability uses.
/// spec: docs/08-protocol.md#datachannel-topology
enum class ChannelClass : std::uint8_t { Control, Realtime, Bulk };

struct ChannelDecl {
    ChannelClass channel = ChannelClass::Control;
};

/// Privileges a capability requires; granted explicitly by integrator
/// config, never assumed. spec: docs/10-security.md
struct Privilege {
    std::string name; // e.g. "uinput", "fs-read:/var/log"
};

/// Who this capability serves. spec: docs/05-extension-model.md#consumers-peer-or-backend
struct ConsumerKinds {
    bool peer = true;     // P2P-connected operator clients
    bool backend = false; // the fjarr-server / Fjarr Cloud itself
};

struct CapabilityManifest {
    std::string name; // reverse-DNS, e.g. "fjarr.camera"
    SemVer version;
    std::vector<TrackDecl> tracks;
    std::vector<ChannelDecl> channels;
    nlohmann::json config_schema; // JSON Schema for this capability's config
    std::vector<Privilege> privileges;
    ConsumerKinds consumers;
};

using SessionId = std::uint64_t;

enum class DetachReason : std::uint8_t { Closed, PeerGone, Heartbeat, Error };

/// One control/backend message addressed to a capability's namespace.
/// spec: docs/08-protocol.md#envelope
struct Envelope {
    std::string cap;      // capability name
    std::string type;     // message type within the namespace
    std::string event_id; // correlation id
    std::string kind;     // request | accept | feedback | result | event
    nlohmann::json payload;
};

/// Per-session handle given to capabilities: tracks, channel senders, and a
/// worker pool so plugins never block the core loop. Populated in M1.
class SessionContext;

class Capability {
  public:
    virtual ~Capability() = default;

    virtual CapabilityManifest manifest() const = 0;

    /// Config already validated against manifest().config_schema.
    virtual void configure(const nlohmann::json& validated_config) = 0;

    virtual void session_attached(SessionContext& ctx,
                                  const nlohmann::json& granted_params) = 0;
    virtual void session_detached(SessionId id, DetachReason reason) = 0;

    /// Envelopes addressed to this capability's namespace only.
    virtual void on_message(SessionContext& ctx, const Envelope& msg) = 0;

    virtual void shutdown() = 0;
};

} // namespace fjarr
