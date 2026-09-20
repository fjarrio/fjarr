#pragma once
// The capability plugin interface — the extension model's agent-side seam.
// spec: docs/05-extension-model.md#the-capability-contract-agent-side
// spec: docs/09-interfaces.md#the-capability-interface-agent-side
//
// ABI is NOT stable before M6 (docs/05#compatibility-rules).
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

namespace fjarr {

class SourceFactory; // fjarr/video_source.hpp

/// The wire session_id (UUIDv7 from fjarr-server). spec: docs/08-protocol.md#signaling
using SessionId = std::string;

struct SemVer {
    int major = 0, minor = 0, patch = 0;
};

enum class TrackKind : std::uint8_t { Video, Audio };

/// A media track a capability can produce (announced in the track manifest).
/// spec: docs/08-protocol.md#track-manifest
struct TrackDecl {
    std::string track_id; // stable id, e.g. "cam-front"
    std::string label;    // human label, e.g. "Front"
    TrackKind kind = TrackKind::Video;
};

/// DataChannel classes. spec: docs/08-protocol.md#datachannel-topology
enum class ChannelClass : std::uint8_t { Control, Realtime, Bulk, Stream };

/// A bulk channel's framing, declared here and never guessed by a peer (docs/08#blob-frames):
/// `Raw` hands every binary message to the capability as-is (terminal input); `Blob` carries
/// blob frames the core parses and checks before delivering chunks.
enum class BulkFraming : std::uint8_t { Raw, Blob };

struct ChannelDecl {
    ChannelClass channel = ChannelClass::Control;
    BulkFraming framing = BulkFraming::Raw; // bulk only
};

/// One validated chunk of a blob as the core delivers it (docs/08#blob-frames). The payload view
/// is valid for the duration of the on_blob_chunk() call only.
struct BlobChunk {
    std::string blob_id; // uuid text, as the envelope's reference names it
    std::uint64_t offset = 0;
    std::uint64_t blob_len = 0;
    std::span<const std::byte> payload;
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
    /// Track CAPACITY — the concrete per-session set is provided at
    /// session_attached (docs/05; M1 API-fit review F1).
    std::vector<TrackDecl> tracks;
    std::vector<ChannelDecl> channels;
    nlohmann::json config_schema; // JSON Schema for this capability's config
    std::vector<Privilege> privileges;
    ConsumerKinds consumers;
    /// Names of capabilities this one requires (F4).
    std::vector<std::string> dependencies;
    /// Takes the docs/10 ownership lease; release_all_input() is called on
    /// every detach path, first. spec: docs/15-testing-strategy.md#safety-behaviors
    bool input_bearing = false;
};

/// One control/realtime/backend message addressed to a capability's namespace.
/// spec: docs/08-protocol.md#envelope
struct Envelope {
    std::string cap;      // capability name
    std::string type;     // message type within the namespace
    std::string event_id; // correlation id
    std::string kind;     // request | accept | feedback | result | event
    nlohmann::json payload = nlohmann::json::object();
};

/// Sending surface with mandatory backpressure.
/// spec: docs/08-protocol.md#backpressure (M1 API-fit review F3)
class ChannelSender {
  public:
    virtual ~ChannelSender() = default;
    /// control/realtime: throws FjarrError(payload-invalid) above 16 KiB UTF-8.
    virtual void send(const Envelope& msg) = 0;
    /// bulk/stream only: false = above HIGH_WATER, not sent — pump on on_drain.
    [[nodiscard]] virtual bool send_binary(std::span<const std::byte> frame) = 0;
    virtual std::size_t buffered_amount() const = 0;
    virtual void on_drain(std::function<void()> below_low_watermark) = 0;
};

/// Coarse detach reason; the exact docs/08 string travels in `detail`
/// ("operator-closed", "peer-gone", "heartbeat", "media-restart",
/// "ice-restart", "media-error", "negotiation-timeout:<milestone>",
/// "agent-shutdown", "ice-failed").
enum class DetachReason : std::uint8_t { Closed, PeerGone, Heartbeat, Error };

class SessionContext;
class BackendContext;

class Capability {
  public:
    virtual ~Capability() = default;

    virtual CapabilityManifest manifest() const = 0;

    /// Config already validated against manifest().config_schema.
    /// `sources` resolves a `source = …` value (a description string or {type = …}) into a
    /// VideoSource through the agent's registry, so no capability re-implements the contract.
    virtual void configure(const nlohmann::json& validated_config, const SourceFactory& sources) = 0;

    /// Sessions: attach/detach; ctx provides tracks, channel senders, the
    /// worker pool. Both run on the core loop.
    virtual void session_attached(SessionContext& ctx, const nlohmann::json& granted_params) = 0;
    virtual void session_detached(const SessionId& id, DetachReason reason,
                                  std::string_view detail) = 0;

    /// Safety (docs/15): called FIRST on every detach path for input-bearing
    /// capabilities, before pipelines are touched. Default no-op.
    virtual void release_all_input(const SessionId& /*id*/) {}

    /// Envelopes addressed to this capability's namespace. select-tracks /
    /// bandwidth-stats never arrive here: the core serves them for every
    /// track-owning capability (docs/08#track-control).
    virtual void on_message(SessionContext& ctx, const Envelope& msg) = 0;

    /// Incoming bytes on this capability's bulk channel (docs/08#blob-frames). A `Raw` channel
    /// delivers every binary message as-is; a `Blob` channel delivers chunks whose header the core
    /// already parsed and checked (a bad chunk is counted and never delivered). Default no-ops: a
    /// capability that only sends declares nothing more. Both run on the core loop.
    virtual void on_binary(SessionContext& /*ctx*/, std::span<const std::byte> /*bytes*/) {}
    virtual void on_blob_chunk(SessionContext& /*ctx*/, const BlobChunk& /*chunk*/) {}

    /// Backend-consumer hooks (F2) — default no-ops so peer-only
    /// capabilities are unaffected.
    virtual void backend_attached(BackendContext&) {}
    virtual void backend_detached() {}
    virtual void on_backend_message(BackendContext&, const Envelope&) {}

    /// A configured source as the endpoint and the doctor list it (docs/24 `/sources`), before any
    /// session registers its track: the docs/26 missing-device behaviour needs the reason up front.
    struct ConfiguredSource {
        std::string track_id, label, identity;
        bool available = false;
        bool required = false;
        std::string reason; // why unavailable
    };
    /// Track-owning capabilities describe their configured sources; others return nothing.
    virtual std::vector<ConfiguredSource> configured_sources() const { return {}; }

    virtual void shutdown() = 0;
};

} // namespace fjarr
