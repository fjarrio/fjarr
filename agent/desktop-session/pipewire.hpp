#pragma once
// The PipeWire connection the helper hands the agent for one capture, narrowed to that capture's
// node first: the agent's account must reach the screen and nothing else of the desktop user's,
// microphones included. spec: docs/23-agent-core-architecture.md#desktop-descriptor-handover
#include <cstdint>
#include <string>

namespace fjarr::desktop::helper {

/// A connection to the desktop user's PipeWire that sees only the core, `node` and the
/// `client-node` factory (a capture stream is a client-node); everything else, including objects
/// created later, is hidden, and the holder cannot widen it. The descriptor, or -1 with `error`.
int open_narrowed_pipewire(std::uint32_t node, std::string* error);

} // namespace fjarr::desktop::helper
