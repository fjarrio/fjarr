#pragma once
// PipelineIntrospector — one walk over a GstBin, three renderings (DOT, JSON,
// summary), stored in a per-pipeline ring; the endpoint and the capability
// are readers of the same ring.
// spec: docs/24-pipeline-introspection.md#the-data-model · #implementation-notes-docs23-hooks
#include <chrono>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <gst/gst.h>
#include <nlohmann/json.hpp>

#include "core/glib/raii.hpp"

namespace fjarr::introspect {

struct SnapshotMeta {
    std::string pipeline_id; // producer:<track>:<tier> | session:<session_id> | agent
    std::string kind;        // producer | session | agent
    std::string session_id;
    std::string robot_id;
    unsigned generation = 0;
    unsigned seq = 0;
    std::int64_t ts = 0;
    std::string trigger;
    std::string state;
};

struct Snapshot {
    SnapshotMeta meta;
    std::string json; // introspect.schema.json, serialized compact: a parsed tree costs several times its text (docs/24 retention)
    std::string dot;  // empty for a ring's older entries (DOT is kept for the last DOT_KEPT snapshots)
    std::string txt;
};

/// Walk a bin into a Snapshot (core loop only; reads element state without locking streaming threads).
Snapshot walk(GstBin* bin, SnapshotMeta meta);
/// The summary line form (docs/24) from a JSON snapshot.
std::string summarize(const nlohmann::json& snapshot);

/// Per-pipeline history ring with 250 ms trailing-edge coalescing (a burst of
/// triggers yields one snapshot per window, the LAST one always lands) and
/// optional DOT files.
class SnapshotStore {
  public:
    using Scheduler = std::function<glib::SourceGuard(std::chrono::milliseconds, std::function<void()>)>;
    explicit SnapshotStore(std::size_t history = 64, std::string dot_dir = "", Scheduler schedule = nullptr);
    /// Take a snapshot now (bypasses coalescing when `force`).
    void take(GstBin* bin, SnapshotMeta meta, bool force = false);
    /// Remove a pipeline (closed session) — keeps the last 8 for 10 min.
    void retire(const std::string& pipeline_id);
    std::vector<SnapshotMeta> list() const;
    std::shared_ptr<const Snapshot> latest(const std::string& pipeline_id) const;
    std::shared_ptr<const Snapshot> at(const std::string& pipeline_id, unsigned seq) const;
    std::vector<SnapshotMeta> history(const std::string& pipeline_id) const;
    void on_snapshot(std::function<void(const Snapshot&)> fn) { listener_ = std::move(fn); }
    void expire_retired();

  private:
    struct Ring {
        std::deque<std::shared_ptr<const Snapshot>> items;
        unsigned seq = 0;
        std::chrono::steady_clock::time_point last{};
        bool retired = false;
        std::chrono::steady_clock::time_point retired_at{};
        // A trigger inside the window is deferred to its end (the bin is kept alive by a ref).
        glib::GstObjectPtr<GstBin> pending_bin;
        SnapshotMeta pending_meta;
        bool pending = false;
        glib::SourceGuard pending_timer; // owned here: a destroyed store fires nothing
    };
    void take_now(Ring& ring, GstBin* bin, SnapshotMeta meta);
    static constexpr std::size_t MAX_RETIRED = 8; // closed pipelines kept (docs/24), oldest evicted first
    static constexpr std::size_t DOT_KEPT = 8;    // DOT bodies kept per ring (the most recent ones)
    std::size_t history_;
    std::string dot_dir_;
    Scheduler schedule_;
    std::map<std::string, Ring> rings_;
    std::function<void(const Snapshot&)> listener_;
};

} // namespace fjarr::introspect
