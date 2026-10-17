#pragma once
// Capture memory for module E's stream reader (ADR-0041): which memory a capture asks mutter for,
// the EnumFormat offer and the params that follow the Format, and the frames between mutter's
// buffers and the track's appsrc (DMA-BUF wrapped once, held while GStreamer holds them, queued
// back on release; shared memory copied as before; repeats marked). No PipeWire loop here: the
// reader drives it on its PipeWire thread, and the unit tests drive it with hand-made buffers in
// place of a pw_stream (agent/tests/test_capture_memory.cpp).
// spec: docs/23-agent-core-architecture.md#desktop-capture-memory
#include <chrono>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gst/gst.h>
#include <gst/video/video.h>
#include <pipewire/stream.h>
#include <spa/param/video/raw.h>
#include <spa/pod/builder.h>

#include <fjarr/desktop_backend.hpp>

namespace fjarr::desktop::mutter {

// ------------------------------------------------------------------ formats

/// The four 8-bit RGB layouts mutter records a monitor in: DRM fourcc <-> SPA video format
/// (XR24 = BGRx, AR24 = BGRA, XB24 = RGBx, AB24 = RGBA in memory order). UNKNOWN / 0 otherwise.
spa_video_format spa_format_of(std::uint32_t drm_fourcc);
std::uint32_t drm_fourcc_of(spa_video_format format);
/// "XR24:0x0200000000000901" ("XR24" for LINEAR), as GStreamer's drm-format names it.
std::string drm_format_string(std::uint32_t drm_fourcc, std::uint64_t modifier);

/// What one capture asks for (CaptureOptions, the monitor's kind, helper.capture_memory).
struct MemoryRequest {
    std::vector<DmaBufFormat> imports; // the media plane's import list
    std::string encoder;               // its encoder family, named when the list is empty
    bool virtual_monitor = false;      // mutter's Meta- monitors: no KMS CRTC, always SHM
    bool force_shm = false;            // helper.capture_memory = "shm"
    /// A restarted capture whose DMA-BUF buffers the reader could not take (Frames::take_unwrappable):
    /// shared memory, for this reason.
    std::string fallback;
};

/// What mutter's node offers, from its EnumFormat params: per format (in mutter's order) the
/// DMA-BUF modifiers it can allocate (empty: shared memory only). `known` is false when the node
/// could not be asked; the offer then trusts PipeWire's own intersection.
struct ProducerFormats {
    bool known = false;
    std::vector<std::pair<spa_video_format, std::vector<std::uint64_t>>> formats;
    bool any_modifier() const;
};
ProducerFormats parse_producer_formats(const std::vector<const spa_pod*>& enum_formats);

/// The reader's EnumFormat offer, in order of preference: one DMA-BUF offer per format of the
/// import list with the modifiers mutter can also make (mandatory, left for mutter to fixate), then
/// the shared-memory formats: the same formats first, then the others the reader takes.
struct Offer {
    std::vector<std::pair<spa_video_format, std::vector<std::uint64_t>>> dmabuf; // empty: SHM only
    std::vector<spa_video_format> shm;
    std::string shm_reason; // why DMA-BUF is not offered; "" when it is
    bool dmabuf_offered() const { return !dmabuf.empty(); }
};
Offer plan_offer(const MemoryRequest& request, const ProducerFormats& producer);
/// Why a capture that settled on shared memory is on it: the offer's reason, or, when DMA-BUF was
/// offered, that mutter chose shared memory over it (saying so when its formats could not be read).
std::string settled_shm_reason(const Offer& offer, const ProducerFormats& producer);

/// A capture keeps its memory for its life (docs/23#desktop-capture-memory): what a fixed format
/// means against the memory settled so far (none yet: `settled` empty).
enum class MemorySettle { First, Same, Changed };
MemorySettle settle_kind(std::optional<bool> settled_dmabuf, bool dmabuf);
/// A changed memory restarts the capture, at most this often and never a virtual monitor's (a
/// restart would make another monitor); past that the capture adopts the new memory.
inline constexpr int MAX_MEMORY_RESTARTS = 3;
bool restart_for_memory_change(bool virtual_monitor, int restarts_so_far);

/// The EnumFormat params of an offer, built in `b` (which must outlive them). `width`×`height` > 0:
/// exactly that size (a virtual monitor takes its size from its consumer's format). A param that did
/// not fit the builder is left out (the caller sees fewer, or none).
std::vector<const spa_pod*> build_enum_formats(const Offer& offer, spa_pod_builder* b, int width = 0, int height = 0);

/// A Format param as the reader sees it. A DMA-BUF format carries a modifier; until mutter, which
/// allocates, has fixated it, the modifier is still a choice (`fixed` false).
struct NegotiatedFormat {
    spa_video_format format = SPA_VIDEO_FORMAT_UNKNOWN;
    int width = 0, height = 0;
    bool dmabuf = false;
    bool fixed = true;
    std::uint64_t modifier = 0;
    std::string drm_format() const { return drm_format_string(drm_fourcc_of(format), modifier); }
};
std::optional<NegotiatedFormat> parse_format(const spa_pod* format);

/// The Buffers and Meta params for a format: DMA-BUF buffers (at least 8 of them: one held frame and
/// a full appsrc queue must still leave mutter one to record into) or MemFd|MemPtr, and the cursor.
/// Never SyncTimeline: implicit sync, as measured (ADR-0041). A param that did not fit is left out.
std::vector<const spa_pod*> build_buffer_params(bool dmabuf, spa_pod_builder* b);
inline constexpr int MIN_DMABUF_BUFFERS = 8;

/// A buffer has pixels when its first chunk is non-empty and not flagged corrupted, whatever its
/// memory: a cursor-only buffer is told apart that way, never by mapping it.
bool has_pixels(const spa_buffer* buffer);

// ------------------------------------------------------------------- frames

/// What became of a buffer mutter delivered (Frames::on_buffer).
enum class Delivery {
    CursorOnly, // no pixels, only the cursor: back to mutter now
    Copied,     // shared memory, copied into a frame: back to mutter now
    Held,       // DMA-BUF, pushed without a copy: held until GStreamer releases it
    Dropped,    // pixels, but no frame could be made of them (unmapped, out of bounds, unwrapped): back now
};
/// Whether the reader gives the buffer back at once (everything but a held DMA-BUF frame).
inline bool requeue_now(Delivery d) { return d != Delivery::Held; }
/// Whether a frame came of it, for the reader's first-frame watch.
inline bool made_frame(Delivery d) { return d == Delivery::Copied || d == Delivery::Held; }

struct FrameCounts {
    std::uint64_t fresh = 0;       // buffers mutter delivered with pixels
    std::uint64_t repeated = 0;    // pushed again: keepalive re-sends, the frame a new pipeline is given
    std::uint64_t cursor_only = 0; // buffers that carried only the cursor
    std::optional<std::chrono::steady_clock::time_point> newest; // when the newest new frame arrived
};

/// The frames of one capture between mutter's buffers and the appsrc. Thread-safe: the PipeWire
/// thread delivers, the core loop attaches a new pipeline, and GStreamer's threads release.
class Frames {
  public:
    /// `requeue`: give a buffer back to mutter (pw_stream_queue_buffer), called on the PipeWire
    /// thread from drain_releases(). `wake`: a released buffer is waiting; arrange drain_releases()
    /// on the PipeWire thread soon. Called from any thread, never re-entering Frames.
    Frames(std::function<void(pw_buffer*)> requeue, std::function<void()> wake);
    ~Frames();
    Frames(const Frames&) = delete;
    Frames& operator=(const Frames&) = delete;

    /// The negotiated format (a fixed one): the appsrc caps follow it. A different format, size or
    /// modifier drops the newest frame: its buffer was made for the old one.
    void set_format(const NegotiatedFormat& format);
    /// mutter's buffer arrived or went (pw_stream add_buffer / remove_buffer). A DMA-BUF buffer's
    /// planes are wrapped here, once, on duplicates of their descriptors, all of them or none; a
    /// MemFd buffer PipeWire did not map is mapped here.
    void add_buffer(pw_buffer* buffer);
    void remove_buffer(pw_buffer* buffer);
    /// Set once when a DMA-BUF buffer could not be wrapped: more than one plane (not verified on
    /// any hardware yet, docs/23#desktop-capture-memory), or a plane whose descriptor could not be
    /// duplicated or wrapped. The reader then moves the capture to shared memory.
    std::optional<std::string> take_unwrappable();
    /// A buffer mutter delivered: what became of it (requeue_now says whether it goes back now).
    Delivery on_buffer(pw_buffer* buffer);
    /// The keepalive tick: re-send the newest frame when nothing went out for `interval_ms`.
    void keepalive(int interval_ms);
    /// Feed `appsrc` (a ref is taken), starting with the newest frame, as a repeat.
    void attach(GstElement* appsrc);
    /// On the PipeWire thread, after `wake`: give mutter back the buffers GStreamer released.
    void drain_releases();
    /// Stop feeding the appsrc: the stream's memory changed under a pipeline built for the old one
    /// (docs/23#desktop-capture-memory). Frames are still taken and held; nothing is pushed, and
    /// an appsrc attached meanwhile is only kept.
    void suspend();
    /// Feed again (the source's caps follow the new memory by now): the appsrc gets the current
    /// caps, and the newest frame if there is one.
    void resume();
    FrameCounts counts() const;
    bool has_frame() const;
    /// Buffers delivered as DMA-BUF and not yet given back (the held one included): for tests and logs.
    std::size_t buffers_out() const;
    /// The appsrc caps now (a new ref, or null before a fixed format).
    GstCaps* caps() const;

  private:
    struct Slot {
        pw_buffer* pw = nullptr;
        std::uint64_t delivery = 0; // which delivery is out, so a stale release is ignored
        bool out = false;           // delivered to us and not given back
        std::vector<GstMemory*> planes; // DMA-BUF, wrapped once (our refs)
        gsize offset[GST_VIDEO_MAX_PLANES]{};
        gint stride[GST_VIDEO_MAX_PLANES]{};
        void* map = nullptr; // MemFd we mapped ourselves
        std::size_t map_size = 0;
    };
    struct Channel; // the release path, which outlives Frames while GStreamer holds buffers
    // RAII for the refs Frames holds (docs/09). Not core/glib/raii.hpp's: that one counts into
    // libfjarr's census and brings the WebRTC headers, and this module is built apart from both.
    struct Unref {
        void operator()(GstBuffer* b) const noexcept { gst_buffer_unref(b); }
        void operator()(GstCaps* c) const noexcept { gst_caps_unref(c); }
        void operator()(GstObject* o) const noexcept { gst_object_unref(o); }
        void operator()(GstElement* e) const noexcept { gst_object_unref(e); }
        void operator()(GstAllocator* a) const noexcept { gst_object_unref(a); }
    };
    using BufferRef = std::unique_ptr<GstBuffer, Unref>;

    GstBuffer* wrap_locked(const Slot& slot, GstBuffer* token, bool repeat) const;
    bool push_locked(GstBuffer* buffer); // takes `buffer`; true when the appsrc took it
    bool repeat_newest_locked();         // the newest frame again, marked; true when the appsrc took it
    GstBuffer* copy_shm_locked(const Slot* slot, const spa_buffer* sb) const;

    std::function<void(pw_buffer*)> requeue_;
    std::shared_ptr<Channel> channel_;
    mutable std::mutex mu_;
    std::unique_ptr<GstAllocator, Unref> dmabuf_allocator_;
    std::map<pw_buffer*, Slot> slots_;
    std::uint64_t deliveries_ = 0;
    NegotiatedFormat format_;
    std::unique_ptr<GstCaps, Unref> caps_;
    std::unique_ptr<GstElement, Unref> appsrc_; // the source's, not ours: no end-of-stream, a new reader may follow
    bool suspended_ = false;
    std::optional<std::string> unwrappable_; // set by add_buffer, taken once by the reader
    bool unwrappable_said_ = false;
    pw_buffer* held_ = nullptr; // DMA-BUF: the newest frame, for the keepalive and a new pipeline
    BufferRef held_token_;      // ... and our ref on its token (see on_buffer)
    BufferRef last_shm_;        // SHM: the newest frame, copied
    FrameCounts counts_;
    std::chrono::steady_clock::time_point last_push_{};
};

} // namespace fjarr::desktop::mutter
