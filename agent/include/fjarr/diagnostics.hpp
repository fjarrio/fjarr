#pragma once
// The diagnostics bundle (docs/24): what `fjarr-agent --diagnostics` writes, for embedders too.
// spec: docs/24-pipeline-introspection.md#files-and-bundles
#include <string>

#include <fjarr/agent.hpp>

namespace fjarr {

struct DiagnosticsResult {
    bool ok = false;
    std::string source;  // "endpoint" (the running agent's rings and log) | "offline" (config, versions, check only)
    std::string message; // one line for the terminal
};

/// Write the bundle to `out_path`: from the running agent's introspection endpoint (per
/// `config.introspect`) when it answers, else an offline bundle of what this process can know.
DiagnosticsResult write_diagnostics(const AgentConfig& config, const std::string& out_path);

} // namespace fjarr
