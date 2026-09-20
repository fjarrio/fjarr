#pragma once
// fjarr.introspect — the built-in capability exposing the snapshot rings over
// the session (docs/24 message table): list, subscribe (snapshot events with
// json/dot as blob references, newest-wins per pipeline while a blob is in
// flight), history, one snapshot by seq, stats. The core gates it by grant;
// this class never sees an ungranted session. spec:
// docs/24-pipeline-introspection.md#from-the-dashboard-the-fjarrintrospect-capability
//       docs/08-protocol.md#blob-frames ·
//       docs/06-capabilities.md#fjarrintrospect--pipeline-introspection-slice-3-core-slice-5-ui
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>

#include <fjarr/capability.hpp>
#include <fjarr/session_context.hpp>

#include "introspect/introspector.hpp"

namespace fjarr::capabilities {

class IntrospectCapability final : public Capability {
  public:
    /// `store` resolves lazily: the agent creates the rings when it boots, after
    /// registration.
    IntrospectCapability(std::function<introspect::SnapshotStore *()> store, std::function<nlohmann::json()> stats);
    ~IntrospectCapability() override;

    CapabilityManifest manifest() const override;
    void configure(const nlohmann::json &, const SourceFactory &) override {}
    void session_attached(SessionContext &ctx, const nlohmann::json &granted_params) override;
    void session_detached(const SessionId &id, DetachReason reason, std::string_view detail) override;
    void on_message(SessionContext &ctx, const Envelope &msg) override;
    void shutdown() override;

    /// Snapshots replaced by a newer one while their pipeline's blobs were still
    /// in flight (GET /stats, tests).
    unsigned long coalesced() const { return coalesced_; }

  private:
    struct Subscription {
        std::string filter = "*"; // "*" or one pipeline_id
        std::set<std::string> forms{"txt", "json", "dot"};
        bool matches(const std::string &pipeline_id) const { return filter == "*" || filter == pipeline_id; }
    };
    struct Attached {
        SessionContext *ctx = nullptr;
        std::optional<Subscription> sub;
        std::map<std::string, int> in_flight;                                    // pipeline → blobs pending completion
        std::map<std::string, std::shared_ptr<const introspect::Snapshot>> held; // newest snapshot waiting for in_flight == 0
    };
    void ensure_listening();
    void on_snapshot(const introspect::Snapshot &snap);
    void deliver(const SessionId &id, std::shared_ptr<const introspect::Snapshot> snap);
    /// The docs/24 snapshot shape: metadata, `txt` inline, `json`/`dot` as blob
    /// references whose bytes start flowing after the caller's envelope.
    /// `on_done` runs per blob.
    nlohmann::json snapshot_payload(Attached &a, const introspect::Snapshot &snap, const std::set<std::string> &forms,
                                    const std::string &pipeline_id);
    static std::set<std::string> parse_forms(const nlohmann::json &payload);

    std::function<introspect::SnapshotStore *()> store_;
    std::function<nlohmann::json()> stats_;
    std::map<SessionId, Attached> sessions_;
    unsigned listener_ = 0;
    unsigned long coalesced_ = 0;
};

} // namespace fjarr::capabilities
