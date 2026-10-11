#pragma once
// The effective default output, as PipeWire's session manager publishes it: the `default` metadata's
// `default.audio.sink`, whose value is JSON naming the sink's node.name ({"name": "alsa_output…"}).
// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol (Sound)
#include <string>

#include <nlohmann/json.hpp>

namespace fjarr::desktop {

/// The sink's node name from a `default.audio.sink` value; empty when the value is absent (the key
/// was cleared), not JSON, or names nothing.
inline std::string default_sink_name(const char* value) {
    if (!value || !*value) return {};
    const auto j = nlohmann::json::parse(value, nullptr, /*allow_exceptions=*/false);
    if (!j.is_object()) return {};
    const auto it = j.find("name");
    return it != j.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

} // namespace fjarr::desktop
