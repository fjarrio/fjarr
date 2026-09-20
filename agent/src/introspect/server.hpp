#pragma once
// The local introspection endpoint (libsoup-3 server on the core context).
// Slice 3b routes: /pipelines, /pipelines/<id>.{json,txt,dot}, /sources.
// spec: docs/24-pipeline-introspection.md#on-the-robot-the-local-introspection-endpoint
#include <functional>
#include <memory>
#include <string>

#include <nlohmann/json.hpp>

#include <fjarr/agent.hpp>

#include "introspector.hpp"

namespace fjarr::introspect {

class Server {
  public:
    Server(const AgentConfig::IntrospectSection& config, SnapshotStore& store, std::function<nlohmann::json()> sources,
           std::function<void(const std::string& pipeline_id)> snapshot_now);
    ~Server();
    /// Start listening on the thread-default context. Throws FjarrError on a port in use.
    void start();
    int port() const { return port_; }

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    int port_ = 0;
};

} // namespace fjarr::introspect
