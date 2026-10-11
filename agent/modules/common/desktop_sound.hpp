#pragma once
// The desktop's sound, the module's side (docs/23#desktop-audio): the `desktop-audio` track's source,
// and the helper protocol's sound messages (`speakers`, `start-audio`, `audio-started`,
// `audio-failed`). Shared by module E (whose session helper serves them) and module A (whose kiosk
// session program does, ADR-0034). Everything runs on the core loop. In modules/common/: module code,
// compiled into each module (which do not link libfjarr's RAII kit), never into libfjarr.
// spec: docs/23-agent-core-architecture.md#desktop-audio · #desktop-helper-protocol (Sound)
#include <functional>
#include <memory>
#include <string>

#include <glib.h>
#include <nlohmann/json.hpp>

#include <fjarr/video_source.hpp>

#include "desktop/helper_protocol.hpp"

namespace fjarr::desktop {

class SoundSource;

class DesktopSound {
  public:
    using Send = std::function<void(const nlohmann::json&)>;
    using Log = std::function<void(int level, const std::string&)>;

    /// `send` reaches the helper (dropped while there is none).
    DesktopSound(GMainContext* ctx, Send send, Log log);
    ~DesktopSound();
    DesktopSound(const DesktopSound&) = delete;
    DesktopSound& operator=(const DesktopSound&) = delete;

    /// The track's source, made on the first call: available while a helper reports an output and the
    /// sound is not paused; captured only while the producer's pipeline runs.
    std::shared_ptr<VideoSource> source();

    /// A helper was welcomed (true) or left (false); one that leaves takes its speakers with it.
    void helper(bool present);
    /// Held off without the helper leaving: GNOME's stop-sharing (docs/23#desktop-sharing-stopped).
    void pause(bool paused, const std::string& why);
    /// `speakers`, `audio-started` or `audio-failed`: handled, and true. Anything else: false.
    bool handle(proto::Message& m);

    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;
};

} // namespace fjarr::desktop
