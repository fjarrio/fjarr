#pragma once
// The stream reader (docs/23#desktop-helper-protocol, The stream reader and the cursor; spikes
// 2026-10-03): the only consumer of one capture's PipeWire node. From every buffer it takes the frame,
// for the track's appsrc, and the cursor (SPA_META_Cursor), which pipewiresrc drops. Linked as soon as
// the capture starts, on a PipeWire thread of its own; results reach the core loop by posting.
// A physical monitor's frames come as DMA-BUF where the encoder imports them, and as shared memory
// otherwise (ADR-0041, docs/23#desktop-capture-memory; capture_memory.hpp).
// spec: docs/22-remote-desktop-client.md#cursor-strategy · docs/08 `cursor`, `cursor-position`
#include <cstdint>
#include <functional>
#include <memory>
#include <string>

#include <glib.h>
#include <gst/gst.h>
#include <nlohmann/json.hpp>

#include "capture_memory.hpp"
#include "desktop/cursor_image.hpp"

namespace fjarr::desktop::mutter {

class StreamReader {
  public:
    using Shape = std::function<void(const CursorImage&)>;
    /// The pointer within this capture's stream, normalized; only while it is inside it.
    using Position = std::function<void(double nx, double ny)>;
    using Log = std::function<void(int level, const std::string&)>;

    struct Options {
        int keepalive_ms = 100;
        /// > 0: ask for exactly that size: a virtual monitor takes its size from its consumer's
        /// format (docs/23, Virtual monitors).
        int width = 0, height = 0;
        std::string name;     // the monitor's connector, for the log
        MemoryRequest memory; // what it may be read as (docs/23#desktop-capture-memory)
    };

    /// Takes `fd`, a PipeWire connection narrowed to `node`. Callbacks run on `ctx`. Nullptr (logged)
    /// when PipeWire refuses.
    static std::unique_ptr<StreamReader> start(GMainContext* ctx, int fd, std::uint32_t node, Options options, Shape shape, Position position, Log log);
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
    /// Called on the core loop once the capture's memory is settled (the first fixed format), and
    /// again if the reader adopts another (on_memory_changed): DMA-BUF (true) or shared memory.
    /// The source's caps follow it.
    void on_memory(std::function<void(bool dmabuf)> cb);
    /// Called on the core loop when a renegotiation changed the memory (docs/23#desktop-capture-memory:
    /// a capture keeps its memory for its life). The reader has stopped feeding the appsrc, whose
    /// pipeline was built for the old memory. Return true when the capture is restarted (a new
    /// reader replaces this one); false, and this reader adopts the new memory: on_memory fires
    /// again, so the source's caps follow, and it feeds the appsrc again, whose producer then fails
    /// to negotiate and is rebuilt from those caps.
    void on_memory_changed(std::function<bool(const std::string& why)> cb);
    /// Called on the core loop when mutter's DMA-BUF buffers cannot be wrapped (more than one
    /// plane, or a descriptor that would not duplicate): the capture is to restart on shared memory
    /// with `why` as its reason. The reader feeds nothing more.
    void on_fallback(std::function<void(const std::string& why)> cb);
    /// Called on the core loop when the reader cannot go on (it could not link to the stream after
    /// reading mutter's formats): the source is lost, by name. The callback may drop the reader.
    void on_failed(std::function<void(const std::string& why)> cb);

    /// docs/24#capture-frame-counts: {frames: {new, repeated, cursor_only, newest_age_ms},
    /// memory: {kind: "dmabuf", drm_format} | {kind: "shm", reason}} (memory null until negotiated).
    nlohmann::json status() const;

    struct Impl;

  private:
    explicit StreamReader(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace fjarr::desktop::mutter
