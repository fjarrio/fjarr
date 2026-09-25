#pragma once
// The capability-facing per-session handle and the backend handle.
// spec: docs/23-agent-core-architecture.md#the-concrete-sessioncontext-and-backendcontext
// spec: docs/08-protocol.md#envelope (correlation helpers)
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include <fjarr/blob.hpp>
#include <fjarr/capability.hpp>
#include <fjarr/video_source.hpp>

namespace fjarr {

/// spec: docs/08-protocol.md#track-manifest
struct MonitorInfo {
    std::string id; // stable identity (connector name) — never key on index
    int index = 0;
    bool primary = false;
    int x = 0, y = 0, w = 0, h = 0;
    double scale = 1.0;
    std::string name;
};

struct OperatorInfo {
    std::string id;
    std::string label;
};

struct TrackSpec {
    std::string track_id; // stable; camera: the config key; desktop: "desk-<connector>"
    std::string label;
    TrackKind kind = TrackKind::Video;
    SourceRef source; // a VideoSource + output name; owned by the media plane
    std::optional<MonitorInfo> monitor;
};

struct TrackState {
    bool enabled = false;
    std::string tier = "active"; // active | thumbnail
    int subscribers = 0;
    std::string mid;      // once offered
    bool available = true; // false: the source is unavailable (missing driver, unplugged)
};

/// A deadman the capability arms per input stream (docs/15): `feed()`
/// within the budget or `on_expiry` fires on the core loop, and again on
/// session end. Disarms when destroyed.
class DeadmanHandle {
  public:
    virtual ~DeadmanHandle() = default;
    virtual void feed() = 0;
    virtual bool expired() const = 0;
    /// Milliseconds since the last feed (or arm).
    virtual std::chrono::milliseconds since_feed() const = 0;
};

/// A file descriptor watched on the core loop. The watch is removed when this is destroyed.
/// spec: docs/09-interfaces.md#the-capability-interface-agent-side
class FdWatch {
  public:
    virtual ~FdWatch() = default;
};

/// A repeating timer on the core loop. The timer stops when this is destroyed.
/// spec: docs/09-interfaces.md#the-capability-interface-agent-side
class Timer {
  public:
    virtual ~Timer() = default;
};

class SessionContext {
  public:
    virtual ~SessionContext() = default;

    virtual const SessionId& id() const = 0;
    virtual const OperatorInfo& operator_info() const = 0;
    virtual const nlohmann::json& granted_params(std::string_view cap) const = 0;

    // Tracks: declared during session_attached (frozen into the first offer)
    // and changeable later (renegotiation, docs/08#renegotiation).
    virtual void add_track(TrackSpec spec) = 0;                        // only valid inside session_attached
    virtual void update_tracks(std::vector<TrackSpec> full_set) = 0;   // diffed by track_id, coalesced
    virtual TrackState track_state(std::string_view track_id) const = 0;
    /// The manifest_version of the last offer sent (0 before the first).
    virtual unsigned manifest_version() const = 0;

    // Channels (docs/08 classes). Bulk/stream senders exist only if declared.
    virtual ChannelSender& control() = 0;
    virtual ChannelSender& realtime() = 0;
    virtual ChannelSender& bulk() = 0;   // this capability's fjarr:bulk:<cap>
    virtual ChannelSender& stream() = 0; // this capability's fjarr:stream:<cap> (ADR-0018; M4)

    // Correlation helpers (docs/08#envelope) — the only way to answer a request.
    virtual void accept(const Envelope& request) = 0;
    virtual void feedback(const Envelope& request, nlohmann::json payload) = 0;
    virtual void result(const Envelope& request, nlohmann::json payload) = 0; // payload.ok required
    virtual void fail(const Envelope& request, std::string_view code, std::string_view message) = 0;
    virtual void event(std::string_view type, nlohmann::json payload) = 0;    // kind=event on control

    // Blobs (docs/08#blob-frames): the core chunks `bytes` onto this capability's `blob`-framed
    // bulk channel, pumps under the watermarks and reports completion — `done(ok)` runs on the core
    // loop, false when cancelled or the session ended first. Put the returned reference into the
    // envelope you send *before* the bytes. Throws FjarrError when the manifest declares no bulk
    // channel. Queued until the channel opens; never blocks.
    virtual blob::BlobRef send_blob(std::string bytes, std::string media_type, std::function<void(bool ok)> done = {}) = 0;
    /// Drop a blob not yet fully sent (its receiver times out per docs/08); done(false) if queued.
    virtual void cancel_blob(std::string_view blob_id) = 0;

    /// Watch `fd` for readability on the core loop; `on_readable` runs there, never on another
    /// thread, so a capability's async I/O obeys the same single-loop rule as everything else
    /// (docs/23 threading). Return false to remove the watch. The capability owns the fd and
    /// closes it; destroying the handle only stops the watching. Added in M2 for `fjarr.terminal`,
    /// whose pty is an fd and which `run_async` cannot serve: that is fire-once, not a read loop.
    virtual std::unique_ptr<FdWatch> watch_readable(int fd, std::function<bool()> on_readable) = 0;

    /// Call `on_tick` on the core loop every `period` until it returns false or the handle is
    /// destroyed; the session ending destroys it either way. Added in M4.5 for `fjarr.net`, whose
    /// `link-stats` is specified per second *while the link is open* — an idle link with rising
    /// drop counters is exactly the case a support engineer needs to see, and driving the event
    /// off traffic would show nothing then. Not a scheduler: work that belongs off the loop still
    /// goes through run_async.
    virtual std::unique_ptr<Timer> every(std::chrono::milliseconds period, std::function<bool()> on_tick) = 0;

    // Off-loop work: run `job` on the pool, then `done` back on the core loop
    // — dropped if the session is gone by then (generation-guarded).
    virtual void run_async(std::function<void()> job, std::function<void()> done) = 0;

    // Safety helpers (docs/15): a deadman the capability arms per input stream.
    virtual std::unique_ptr<DeadmanHandle> arm_deadman(std::chrono::milliseconds budget,
                                                       std::function<void()> on_expiry) = 0;

    virtual void close(std::string_view reason) = 0; // capability-initiated session end
};

class BackendContext {
  public:
    virtual ~BackendContext() = default;
    virtual ChannelSender& channel() = 0; // backend-stream envelopes over the signaling connection
    virtual bool online() const = 0;      // signaling connected
    /// Store-and-forward (M7): durable event types are queued while offline.
    /// Slice 3 ships the interface with drop-when-offline.
    virtual void declare_durable(std::vector<std::string> event_types) = 0;
};

} // namespace fjarr
