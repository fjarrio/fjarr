#pragma once
// What the robot's speakers are: the desktop user's effective default output, watched on a lasting
// PipeWire connection driven from the helper's GLib loop. The helper reports it to the agent and,
// asked, hands over a connection narrowed to it (open_narrowed_pipewire).
// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol (Sound) · #desktop-audio
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

namespace fjarr::desktop::helper {

struct Sink {
    std::uint32_t node = 0;
    std::string name;        // node.name, what the default names
    std::string description; // node.description, for people
};

class SoundWatch {
  public:
    /// Watches from now on, reconnecting when PipeWire restarts; `changed` runs on the GLib default
    /// context whenever the default output may have changed (the caller compares).
    explicit SoundWatch(std::function<void()> changed);
    ~SoundWatch();
    SoundWatch(const SoundWatch&) = delete;
    SoundWatch& operator=(const SoundWatch&) = delete;

    /// The default output, once both the default's name and that node are known.
    std::optional<Sink> default_sink() const;

    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;
};

} // namespace fjarr::desktop::helper
