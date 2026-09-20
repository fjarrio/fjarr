#pragma once
// GET /memory: RSS, the RAII kit's live-object census, hub buffers, channel bytes — and the
// diff since a checkpoint, which is the soak's oracle. When GStreamer's `leaks` tracer is
// loaded (GST_TRACERS=leaks), a checkpoint also brackets its created/removed object lists.
// spec: docs/24-pipeline-introspection.md#on-the-robot-the-local-introspection-endpoint
// spec: docs/23-agent-core-architecture.md#memory-and-lifetime-discipline-and-the-tooling-that-enforces-it
#include <deque>
#include <functional>
#include <string>

#include <nlohmann/json.hpp>

namespace fjarr::introspect {

class MemoryCensus {
  public:
    /// Live counts the census cannot see itself (hub buffers, channel bytes, sessions alive…), as JSON fields.
    using Extra = std::function<nlohmann::json()>;
    explicit MemoryCensus(Extra extra = nullptr);

    /// The current numbers (docs/24 `/memory`).
    nlohmann::json now() const;
    /// Store the current numbers under a new token (bounded to the last 16) and, with the
    /// leaks tracer loaded, reset its activity checkpoint. Returns the stored report with `checkpoint`.
    nlohmann::json checkpoint();
    /// `/memory?since=<token>`: now, then, and the per-field diff; `leaks.created` (objects created
    /// since the checkpoint and still alive, "Type@address") when the tracer is loaded. Reads are
    /// idempotent — the tracer's window is merged into a process-wide ledger, never consumed.
    /// Unknown token → `error` field.
    nlohmann::json since(const std::string& token) const;

    /// Process RSS from /proc/self/statm, bytes (0 when unreadable).
    static std::uint64_t rss_bytes();
    /// Whether GStreamer's leaks tracer is active in this process.
    static bool leaks_tracer_active();

  private:
    Extra extra_;
    unsigned next_token_ = 1;
    std::deque<std::pair<std::string, nlohmann::json>> checkpoints_;
};

} // namespace fjarr::introspect
