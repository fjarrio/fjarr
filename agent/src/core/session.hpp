#pragma once
// Session — the agent-side state machine (requested → building → offered →
// connected → closing → closed), its ChannelRouter, the per-capability
// SessionContext, heartbeat/liveness, the negotiation watchdog, deadmans
// and the renegotiation queue.
// spec: docs/23-agent-core-architecture.md#agent-side-session-state-machine · #datachannel-router
//       docs/08-protocol.md#datachannel-topology · #fjarr-core · #track-control · #renegotiation
#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include <gst/webrtc/webrtc.h>
#include <nlohmann/json.hpp>

#include <fjarr/agent.hpp>
#include <fjarr/capability.hpp>
#include <fjarr/session_context.hpp>

#include "blob_pump.hpp"
#include "glib/raii.hpp"
#include "loop.hpp"
#include "media/consumer.hpp"
#include "media/media_plane.hpp"
#include "media/rate_estimator.hpp"
#include "media/tier_policy.hpp"
#include "protocol.hpp"

namespace fjarr::core {

struct AttachedCapability {
    Capability* capability = nullptr;
    CapabilityManifest manifest;
    nlohmann::json params;
};

struct SessionDeps {
    CoreLoop* loop = nullptr;
    media::MediaPlane* plane = nullptr;
    const AgentConfig* config = nullptr;
    std::function<void(nlohmann::json)> send_signal;
    std::function<void(const SessionEvent&)> emit;
    /// Snapshot trigger for this session's pipeline (docs/24).
    std::function<void(GstBin*, const std::string& trigger)> snapshot;
    /// Called once the session is closed and may be forgotten.
    std::function<void(const SessionId&)> on_closed;
    /// Lease refresh (docs/10): pings from an input-owning session.
    std::function<void(const SessionId&)> on_ping;
    /// Test hooks enabled (fjarr.test silence).
    bool test_hooks = false;
    /// Is this capability registered on the agent at all? A registered-but-ungranted capability is
    /// answered `capability-denied`, an unknown one `capability-unknown` (docs/08#envelope).
    std::function<bool(const std::string&)> known_capability;
};

class Session;

/// The per-(session, capability) handle.
class SessionContextImpl final : public SessionContext {
  public:
    SessionContextImpl(Session& session, std::string cap);
    const SessionId& id() const override;
    const OperatorInfo& operator_info() const override;
    const nlohmann::json& granted_params(std::string_view cap) const override;
    void add_track(TrackSpec spec) override;
    void update_tracks(std::vector<TrackSpec> full_set) override;
    TrackState track_state(std::string_view track_id) const override;
    unsigned manifest_version() const override;
    ChannelSender& control() override;
    ChannelSender& realtime() override;
    ChannelSender& bulk() override;
    ChannelSender& stream() override;
    void accept(const Envelope& request) override;
    void feedback(const Envelope& request, nlohmann::json payload) override;
    void result(const Envelope& request, nlohmann::json payload) override;
    void fail(const Envelope& request, std::string_view code, std::string_view message) override;
    void event(std::string_view type, nlohmann::json payload) override;
    blob::BlobRef send_blob(std::string bytes, std::string media_type, std::function<void(bool ok)> done) override;
    void cancel_blob(std::string_view blob_id) override;
    std::unique_ptr<FdWatch> watch_readable(int fd, std::function<bool()> on_readable) override;
    std::unique_ptr<Timer> every(std::chrono::milliseconds period, std::function<bool()> on_tick) override;
    void run_async(std::function<void()> job, std::function<void()> done) override;
    std::unique_ptr<DeadmanHandle> arm_deadman(std::chrono::milliseconds budget, std::function<void()> on_expiry) override;
    void close(std::string_view reason) override;

    /// fjarr.test hooks (test_hooks = true only): stop answering pings / close every valve for `ms`.
    void test_silence(bool pings, bool media, std::chrono::milliseconds ms);
    const std::string& cap() const { return cap_; }

  private:
    Session& session_;
    std::string cap_;
};

class Session : public std::enable_shared_from_this<Session> {
  public:
    enum class State { Requested, Building, Offered, Connected, Closing, Closed };
    static const char* state_name(State s);

    Session(SessionDeps deps, SessionId id, OperatorInfo op, std::vector<protocol::CapabilityGrant> grants,
            std::optional<protocol::TurnCredentials> turn, bool input_owner);
    ~Session();

    const SessionId& id() const { return id_; }
    const std::string& sid8() const { return sid8_; }
    State state() const { return state_; }
    Generation generation() const { return generation_; }
    const OperatorInfo& operator_info() const { return operator_; }
    bool input_owner() const { return input_owner_; }
    void set_input_owner(bool v) { input_owner_ = v; }
    std::chrono::steady_clock::time_point attached_at() const { return attached_at_; }

    /// requested → building → offered. `caps` are the granted, enabled capabilities.
    void attach(std::vector<AttachedCapability> caps);
    /// Inbound signaling for this session (answer, ice, ice-restart, session-close, peer-gone).
    void on_signal(const protocol::SignalingMessage& msg);
    /// Close from any state; sends session-close unless `from_server`.
    void close(const std::string& reason, bool retry = false, bool from_server = false);
    /** Flush everything now (shutdown): completes a deferred close synchronously. */
    void flush_close();

    media::ConsumerPipeline* consumer() { return consumer_.get(); }
    unsigned manifest_version() const { return manifest_version_; }
    std::vector<protocol::ManifestEntry> manifest() const { return consumer_ ? consumer_->manifest() : std::vector<protocol::ManifestEntry>{}; }
    nlohmann::json describe() const;
    /// The last per-second stats sample as sent on `bandwidth-stats`, per capability (GET /stats).
    const nlohmann::json& last_stats() const { return last_stats_; }
    /// Bytes queued in every DataChannel sender (GET /memory).
    std::size_t buffered_bytes() const;

    // --- used by SessionContextImpl (core loop only)
    const nlohmann::json& granted_params(std::string_view cap) const;
    void add_track(const std::string& cap, TrackSpec spec);
    void update_tracks(const std::string& cap, std::vector<TrackSpec> full_set);
    TrackState track_state(std::string_view track_id) const;
    ChannelSender& sender(ChannelClass cls, const std::string& cap);
    void send_control(const Envelope& env);
    /// docs/08#blob-frames: queue a blob on `cap`'s bulk channel; the pump runs now if it is open.
    blob::BlobRef send_blob(const std::string& cap, std::string bytes, std::string media_type, std::function<void(bool ok)> done);
    void cancel_blob(std::string_view blob_id);
    /// Binary frames dropped for a bad blob header or a closing session (GET /stats).
    unsigned long dropped_binary() const { return dropped_binary_; }
    void run_async(std::function<void()> job, std::function<void()> done);
    std::unique_ptr<DeadmanHandle> arm_deadman(std::chrono::milliseconds budget, std::function<void()> on_expiry);
    void test_silence(bool pings, bool media, std::chrono::milliseconds ms);
    CoreLoop& loop() { return *deps_.loop; }
    bool test_hooks() const { return deps_.test_hooks; }
    struct Deadman;

  private:
    struct ControlSender;
    struct RealtimeSender;
    struct BulkSender;
    struct StreamSender;
    struct DeniedSender;
    friend struct Deadman;

    void set_state(State s);
    void finish_close(const std::string& reason, bool retry, bool from_server);
    bool closing() const { return state_ == State::Closing || state_ == State::Closed; }
    /// Everything a DataChannel signal handler may touch off the loop (docs/23 callback context).
    struct ChannelContext {
        std::weak_ptr<Session> session;
        Generation generation;
        CoreLoop* loop;
        std::string label;
    };
    void milestone(const std::string& name);
    void arm_watchdog();
    void arm_liveness();
    void maybe_connected();
    void send_offer(const std::string& sdp);
    void request_offer();
    void on_channel_open(GstWebRTCDataChannel* dc, const std::string& label);
    void on_channel_text(const std::string& label, const std::string& text);
    void on_channel_data(const std::string& label, const std::string& bytes);
    void pump_blobs(const std::string& label);
    void route(const Envelope& env, const std::string& label);
    void handle_core(const Envelope& env);
    void handle_select_tracks(const Envelope& env, const AttachedCapability& cap);
    void sample_stats();
    /// docs/23#rate-control-and-tier-switching: read the peer's TWCC window, update the estimate,
    /// share it across the enabled tracks, report to the plane, tick each track's tier policy.
    void rate_tick();
    void apply_demand(const std::string& track_id, bool enabled, const std::string& tier);
    void unsubscribe_all();
    AttachedCapability* attached(std::string_view cap);
    void reply(const Envelope& request, nlohmann::json payload);
    void reply_error(const Envelope& request, std::string_view code, std::string_view message);

    SessionDeps deps_;
    SessionId id_;
    std::string sid8_;
    OperatorInfo operator_;
    std::vector<protocol::CapabilityGrant> grants_;
    std::optional<protocol::TurnCredentials> turn_;
    bool input_owner_;
    State state_ = State::Requested;
    Generation generation_ = 1;
    std::chrono::steady_clock::time_point attached_at_{};
    std::vector<AttachedCapability> caps_;
    std::map<std::string, std::unique_ptr<SessionContextImpl>> contexts_;
    bool attaching_ = false;
    std::string attaching_cap_;
    std::unique_ptr<media::ConsumerPipeline> consumer_;
    std::vector<std::pair<std::string, TrackSpec>> pending_tracks_; // (cap, spec) declared at attach
    std::map<std::string, std::pair<std::string, TrackSpec>> tracks_; // track_id → (cap, spec)
    std::map<std::string, std::string> subscribed_tier_; // track_id → tier currently subscribed
    unsigned manifest_version_ = 0;
    bool renegotiation_pending_ = false;
    bool answered_once_ = false;
    std::string last_milestone_ = "attached";
    std::string connection_state_ = "new";
    bool control_open_ = false;
    glib::SourceGuard watchdog_;
    glib::SourceGuard liveness_;
    glib::SourceGuard stats_timer_;
    glib::SourceGuard rate_timer_;
    std::optional<media::RateEstimator> estimator_;
    media::TwccSample last_twcc_; // the window read at the previous tick: an identical read is no new feedback
    std::map<std::string, media::TierPolicy> tier_policy_; // track_id → this viewer's tier override
    glib::SourceGuard silence_timer_;
    glib::SourceGuard close_timer_;
    std::string closing_reason_;
    bool closing_retry_ = false;
    bool silent_pings_ = false;
    bool silent_media_ = false;
    std::map<std::string, glib::GObjectPtr<GstWebRTCDataChannel>> channels_; // label → dc
    std::vector<glib::SignalConnection> dc_signals_;
    std::map<std::string, std::unique_ptr<ChannelSender>> senders_; // label → sender
    std::unique_ptr<ChannelSender> denied_;
    std::map<std::string, BlobPump> blob_pumps_; // bulk label → outbound blobs (docs/08#blob-frames)
    std::vector<std::weak_ptr<Deadman>> deadmans_;
    unsigned long dropped_envelopes_ = 0;
    unsigned long dropped_binary_ = 0;
    nlohmann::json last_stats_ = nlohmann::json::object();
    bool closed_sent_ = false;
};

} // namespace fjarr::core
