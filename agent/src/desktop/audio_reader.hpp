#pragma once
// The audio reader (docs/23#desktop-audio): what the robot's speakers play, read from the default
// output's monitor through the PipeWire connection a helper narrowed to that sink (the session helper
// on GNOME, fjarr-x11-session on X11), on a PipeWire thread of its own like module E's stream
// reader's, into the desktop sound track's appsrc. Shared by modules E and A.
// spec: docs/23-agent-core-architecture.md#desktop-audio
#include <functional>
#include <memory>
#include <string>

#include <glib.h>
#include <gst/gst.h>

namespace fjarr::desktop {

class AudioReader {
  public:
    using Log = std::function<void(int level, const std::string&)>;

    /// Takes `fd`, a PipeWire connection narrowed to the sink named `sink`, and captures its monitor
    /// as S16LE 48 kHz stereo. `log` runs on `ctx`. Nullptr (logged) when PipeWire refuses.
    static std::unique_ptr<AudioReader> start(GMainContext* ctx, int fd, const std::string& sink, Log log);
    ~AudioReader();
    AudioReader(const AudioReader&) = delete;
    AudioReader& operator=(const AudioReader&) = delete;

    /// The caps every buffer has, for the appsrc.
    static GstCaps* caps();

    /// Feed `appsrc` (a ref is taken), replacing any earlier one; null stops feeding.
    void attach(GstElement* appsrc);

    struct Impl;

  private:
    explicit AudioReader(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace fjarr::desktop
