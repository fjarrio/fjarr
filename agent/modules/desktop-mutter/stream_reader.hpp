#pragma once
// The stream reader (docs/23#desktop-helper-protocol, The stream reader and the cursor; spikes
// 2026-10-03): the only consumer of one capture's PipeWire node. From every buffer it takes the frame,
// for the track's appsrc, and the cursor (SPA_META_Cursor), which pipewiresrc drops. Linked as soon as
// the capture starts, on a PipeWire thread of its own; results reach the core loop by posting.
// spec: docs/22-remote-desktop-client.md#cursor-strategy · docs/08 `cursor`, `cursor-position`
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <glib.h>
#include <gst/gst.h>

#include "desktop/cursor_image.hpp"

namespace fjarr::desktop::mutter {

class StreamReader {
  public:
    using Shape = std::function<void(const CursorImage&)>;
    /// The pointer within this capture's stream, normalized; only while it is inside it.
    using Position = std::function<void(double nx, double ny)>;
    using Log = std::function<void(int level, const std::string&)>;

    /// Takes `fd`, a PipeWire connection narrowed to `node`. Callbacks run on `ctx`. Nullptr (logged)
    /// when PipeWire refuses.
    /// `width`×`height` > 0: ask for exactly that size — a virtual monitor takes its size from its
    /// consumer's format (docs/23, Virtual monitors).
    static std::unique_ptr<StreamReader> start(GMainContext* ctx, int fd, std::uint32_t node, int keepalive_ms, Shape shape, Position position, Log log,
                                               int width = 0, int height = 0);
    ~StreamReader();
    StreamReader(const StreamReader&) = delete;
    StreamReader& operator=(const StreamReader&) = delete;

    /// Feed `appsrc` (a ref is taken), starting with the last frame and replacing any earlier one. The
    /// appsrc belongs to the track's source, so a capture that is restarted with a new reader goes on
    /// feeding the pipeline already built (docs/23, The stream reader).
    void attach(GstElement* appsrc);

    /// Called on the core loop when the stream has streamed for a while without a single frame
    /// (docs/23, The stream reader: mutter in metadata mode sometimes records none for a freshly
    /// plugged monitor). The capture is restarted then.
    void on_stalled(std::function<void()> cb);

    struct Impl;

  private:
    explicit StreamReader(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace fjarr::desktop::mutter
