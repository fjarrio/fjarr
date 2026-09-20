#pragma once
// The local introspection endpoint (libsoup-3 server on the core context).
// Routes: /pipelines, /pipelines/<id>.{json,txt,dot}[?seq], /pipelines/<id>/history, /sources,
// /stats, /memory[?since], POST /memory/checkpoint, /log[?minutes], /events (SSE),
// /diagnostics.tar.gz, POST /snapshot.
// spec: docs/24-pipeline-introspection.md#on-the-robot-the-local-introspection-endpoint
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include <fjarr/agent.hpp>

#include "introspector.hpp"
#include "memory.hpp"

namespace fjarr::introspect {

/// What the endpoint reads; every provider runs on the core loop.
struct Providers {
    std::function<nlohmann::json()> sources;                    // GET /sources
    std::function<void(const std::string& pipeline_id)> snapshot_now; // POST /snapshot
    std::function<nlohmann::json()> stats;                      // GET /stats (sessions, hub, producers)
    std::function<nlohmann::json()> bundle_extra;               // diagnostics: config (redacted), versions, check
    MemoryCensus* memory = nullptr;                             // GET /memory, POST /memory/checkpoint
};

class Server {
  public:
    Server(const AgentConfig::IntrospectSection& config, SnapshotStore& store, Providers providers);
    ~Server();
    /// Start listening on the thread-default context. Throws FjarrError on a port in use.
    void start();
    int port() const { return port_; }
    /// Open `/events` streams (tests, /stats).
    std::size_t event_clients() const;

    /// One SSE frame for a snapshot (docs/24 framing); `body` is "" | json | dot | txt.
    static std::string sse_frame(const Snapshot& snap, const std::string& body);
    /// The diagnostics bundle bytes (tar.gz) — also what `fjarr-agent --diagnostics` writes.
    std::string diagnostics_bundle() const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int port_ = 0;
    unsigned listener_ = 0;
};

} // namespace fjarr::introspect
