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
    static std::unique_ptr<StreamReader> start(GMainContext* ctx, int fd, std::uint32_t node, int keepalive_ms, Shape shape, Position position, Log log);
    ~StreamReader();
    StreamReader(const StreamReader&) = delete;
    StreamReader& operator=(const StreamReader&) = delete;

    /// The track's source for a newly built pipeline: `appsrc` fed by this reader, starting with the
    /// last frame. A later call replaces the earlier appsrc (the producer rebuilt its pipeline).
    GstBin* create_bin();

    struct Impl;

  private:
    explicit StreamReader(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace fjarr::desktop::mutter
