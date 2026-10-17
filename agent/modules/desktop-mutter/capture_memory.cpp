// spec: docs/23-agent-core-architecture.md#desktop-capture-memory (ADR-0041)
#include "capture_memory.hpp"

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <gst/allocators/gstdmabuf.h>
#include <gst/app/gstappsrc.h>
#include <gst/video/video.h>
#include <spa/buffer/buffer.h>
#include <spa/buffer/meta.h>
#include <spa/param/buffers.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/iter.h>

#include "media/frame_repeat.hpp"

namespace fjarr::desktop::mutter {

namespace {
constexpr int MAX_CURSOR_SIDE = 384; // docs/08: cursor images at most 384×384
std::size_t cursor_meta_size(int side) { return sizeof(spa_meta_cursor) + sizeof(spa_meta_bitmap) + static_cast<std::size_t>(side) * side * 4; }

constexpr std::uint32_t fourcc(char a, char b, char c, char d) {
    return static_cast<std::uint32_t>(a) | (static_cast<std::uint32_t>(b) << 8) | (static_cast<std::uint32_t>(c) << 16) | (static_cast<std::uint32_t>(d) << 24);
}
// The shared-memory formats the reader takes (its copy is format-blind: 4 bytes a pixel).
constexpr spa_video_format SHM_FORMATS[] = {SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA};

std::string hex_modifier(std::uint64_t m) {
    char buf[24];
    std::snprintf(buf, sizeof buf, "0x%016llx", static_cast<unsigned long long>(m));
    return buf;
}
std::string format_list(const std::vector<std::pair<spa_video_format, std::vector<std::uint64_t>>>& formats) {
    std::string out;
    for (const auto& [f, mods] : formats)
        for (const auto m : mods) out += (out.empty() ? "" : ", ") + drm_format_string(drm_fourcc_of(f), m);
    return out.empty() ? "none" : out;
}

/// All values of a property, whatever its choice; null when absent.
const spa_pod* prop_values(const spa_pod* object, std::uint32_t key, std::uint32_t* n, std::uint32_t* choice, std::uint32_t* flags = nullptr) {
    const spa_pod_prop* p = spa_pod_find_prop(object, nullptr, key);
    if (!p) return nullptr;
    if (flags) *flags = p->flags;
    return spa_pod_get_values(&p->value, n, choice);
}
} // namespace

// ------------------------------------------------------------------ formats

spa_video_format spa_format_of(std::uint32_t drm) {
    switch (drm) {
    case fourcc('X', 'R', '2', '4'): return SPA_VIDEO_FORMAT_BGRx;
    case fourcc('A', 'R', '2', '4'): return SPA_VIDEO_FORMAT_BGRA;
    case fourcc('X', 'B', '2', '4'): return SPA_VIDEO_FORMAT_RGBx;
    case fourcc('A', 'B', '2', '4'): return SPA_VIDEO_FORMAT_RGBA;
    default: return SPA_VIDEO_FORMAT_UNKNOWN;
    }
}

std::uint32_t drm_fourcc_of(spa_video_format f) {
    switch (f) {
    case SPA_VIDEO_FORMAT_BGRx: return fourcc('X', 'R', '2', '4');
    case SPA_VIDEO_FORMAT_BGRA: return fourcc('A', 'R', '2', '4');
    case SPA_VIDEO_FORMAT_RGBx: return fourcc('X', 'B', '2', '4');
    case SPA_VIDEO_FORMAT_RGBA: return fourcc('A', 'B', '2', '4');
    default: return 0;
    }
}

std::string drm_format_string(std::uint32_t drm, std::uint64_t modifier) {
    std::string s(4, ' ');
    for (int i = 0; i < 4; i++) s[i] = static_cast<char>((drm >> (8 * i)) & 0xff);
    return modifier == 0 ? s : s + ":" + hex_modifier(modifier); // GStreamer's drm-format: LINEAR has no suffix
}

bool ProducerFormats::any_modifier() const {
    return std::any_of(formats.begin(), formats.end(), [](const auto& f) { return !f.second.empty(); });
}

ProducerFormats parse_producer_formats(const std::vector<const spa_pod*>& params) {
    ProducerFormats out;
    out.known = true;
    for (const spa_pod* p : params) {
        if (!p || !spa_pod_is_object_type(p, SPA_TYPE_OBJECT_Format)) continue;
        std::uint32_t n = 0, choice = 0;
        const spa_pod* fv = prop_values(p, SPA_FORMAT_VIDEO_format, &n, &choice);
        if (!fv || !spa_pod_is_id(fv)) continue;
        std::vector<spa_video_format> fmts;
        const auto* ids = static_cast<const std::uint32_t*>(SPA_POD_BODY_CONST(fv));
        for (std::uint32_t i = 0; i < n; i++) {
            const auto f = static_cast<spa_video_format>(ids[i]);
            if (std::find(fmts.begin(), fmts.end(), f) == fmts.end()) fmts.push_back(f);
        }
        std::vector<std::uint64_t> mods;
        std::uint32_t nm = 0, mc = 0;
        if (const spa_pod* mv = prop_values(p, SPA_FORMAT_VIDEO_modifier, &nm, &mc); mv && spa_pod_is_long(mv)) {
            const auto* vals = static_cast<const std::int64_t*>(SPA_POD_BODY_CONST(mv));
            for (std::uint32_t i = 0; i < nm; i++) {
                const auto m = static_cast<std::uint64_t>(vals[i]);
                if (std::find(mods.begin(), mods.end(), m) == mods.end()) mods.push_back(m);
            }
        }
        for (const auto f : fmts) {
            auto it = std::find_if(out.formats.begin(), out.formats.end(), [&](const auto& e) { return e.first == f; });
            if (it == out.formats.end()) it = out.formats.insert(out.formats.end(), {f, {}});
            for (const auto m : mods)
                if (std::find(it->second.begin(), it->second.end(), m) == it->second.end()) it->second.push_back(m);
        }
    }
    return out;
}

Offer plan_offer(const MemoryRequest& rq, const ProducerFormats& producer) {
    Offer o;
    if (rq.virtual_monitor) o.shm_reason = "a virtual monitor";
    else if (rq.force_shm) o.shm_reason = "capture_memory = \"shm\"";
    else if (!rq.fallback.empty()) o.shm_reason = rq.fallback;
    else if (rq.imports.empty()) o.shm_reason = "the " + (rq.encoder.empty() ? std::string("media plane's") : rq.encoder) + " encoder imports no DMA-BUF";
    else if (producer.known && !producer.any_modifier()) o.shm_reason = "mutter offers no DMA-BUF (no GPU it can allocate on)";
    else {
        // The import list's order is the preference; mutter's own first choice may be one the
        // encoder cannot import (it is, on the mini-PC), so it is never offered mutter's whole list.
        for (const auto& imp : rq.imports) {
            const spa_video_format f = spa_format_of(imp.drm_fourcc);
            if (f == SPA_VIDEO_FORMAT_UNKNOWN) continue;
            if (producer.known) {
                auto pf = std::find_if(producer.formats.begin(), producer.formats.end(), [&](const auto& e) { return e.first == f; });
                if (pf == producer.formats.end() || std::find(pf->second.begin(), pf->second.end(), imp.modifier) == pf->second.end()) continue;
            }
            auto it = std::find_if(o.dmabuf.begin(), o.dmabuf.end(), [&](const auto& e) { return e.first == f; });
            if (it == o.dmabuf.end()) it = o.dmabuf.insert(o.dmabuf.end(), {f, {}});
            if (std::find(it->second.begin(), it->second.end(), imp.modifier) == it->second.end()) it->second.push_back(imp.modifier);
        }
        if (o.dmabuf.empty()) {
            std::vector<std::pair<spa_video_format, std::vector<std::uint64_t>>> want;
            for (const auto& imp : rq.imports) want.push_back({spa_format_of(imp.drm_fourcc), {imp.modifier}});
            o.shm_reason = "no modifier in common: the " + rq.encoder + " encoder imports " + format_list(want) + "; mutter offers " + format_list(producer.formats);
        }
    }
    // Shared memory last: the DMA-BUF formats first, then the rest the reader takes.
    for (const auto& [f, _] : o.dmabuf) o.shm.push_back(f);
    for (const auto f : SHM_FORMATS)
        if (std::find(o.shm.begin(), o.shm.end(), f) == o.shm.end()) o.shm.push_back(f);
    return o;
}

std::string settled_shm_reason(const Offer& offer, const ProducerFormats& producer) {
    if (!offer.dmabuf_offered()) return offer.shm_reason;
    // DMA-BUF was offered and mutter fixed shared memory: it could not allocate what was offered.
    // Unread formats are the likely cause (nothing narrowed the offer to what it can make).
    return producer.known ? "mutter chose shared memory over the DMA-BUF offered"
                          : "mutter chose shared memory over the DMA-BUF offered (its formats could not be read first)";
}

MemorySettle settle_kind(std::optional<bool> settled, bool dmabuf) {
    if (!settled) return MemorySettle::First;
    return *settled == dmabuf ? MemorySettle::Same : MemorySettle::Changed;
}

bool restart_for_memory_change(bool virtual_monitor, int restarts_so_far) { return !virtual_monitor && restarts_so_far < MAX_MEMORY_RESTARTS; }

std::vector<const spa_pod*> build_enum_formats(const Offer& offer, spa_pod_builder* b, int width, int height) {
    std::vector<const spa_pod*> out;
    auto common = [&](spa_pod_frame* f) {
        spa_pod_builder_push_object(b, f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
        spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    };
    auto size = [&] {
        if (width <= 0 || height <= 0) return;
        const spa_rectangle r{static_cast<std::uint32_t>(width), static_cast<std::uint32_t>(height)};
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&r), 0);
    };
    for (const auto& [format, mods] : offer.dmabuf) {
        spa_pod_frame f[2];
        common(&f[0]);
        spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
        // Mandatory, and not fixated by us: mutter allocates, so mutter picks the modifier.
        spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
        spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
        spa_pod_builder_long(b, static_cast<std::int64_t>(mods.front())); // the default, then every alternative
        for (const auto m : mods) spa_pod_builder_long(b, static_cast<std::int64_t>(m));
        spa_pod_builder_pop(b, &f[1]);
        size();
        // A builder that ran out of room pops null: that offer is left out, never handed on.
        if (const auto* pod = static_cast<const spa_pod*>(spa_pod_builder_pop(b, &f[0]))) out.push_back(pod);
    }
    if (!offer.shm.empty()) {
        spa_pod_frame f[2];
        common(&f[0]);
        spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_format, 0);
        spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
        spa_pod_builder_id(b, offer.shm.front());
        for (const auto fm : offer.shm) spa_pod_builder_id(b, fm);
        spa_pod_builder_pop(b, &f[1]);
        size();
        if (const auto* pod = static_cast<const spa_pod*>(spa_pod_builder_pop(b, &f[0]))) out.push_back(pod);
    }
    return out;
}

std::optional<NegotiatedFormat> parse_format(const spa_pod* param) {
    if (!param || !spa_pod_is_object_type(param, SPA_TYPE_OBJECT_Format)) return std::nullopt;
    std::uint32_t mt = 0, mst = 0;
    if (spa_format_parse(param, &mt, &mst) < 0 || mt != SPA_MEDIA_TYPE_video || mst != SPA_MEDIA_SUBTYPE_raw) return std::nullopt;
    NegotiatedFormat nf;
    std::uint32_t n = 0, choice = 0;
    const spa_pod* fv = prop_values(param, SPA_FORMAT_VIDEO_format, &n, &choice);
    if (!fv || !spa_pod_is_id(fv) || n == 0) return std::nullopt;
    nf.format = static_cast<spa_video_format>(*static_cast<const std::uint32_t*>(SPA_POD_BODY_CONST(fv)));
    if (const spa_pod* sv = prop_values(param, SPA_FORMAT_VIDEO_size, &n, &choice); sv && spa_pod_is_rectangle(sv) && n > 0) {
        const auto* r = static_cast<const spa_rectangle*>(SPA_POD_BODY_CONST(sv));
        nf.width = static_cast<int>(r->width);
        nf.height = static_cast<int>(r->height);
    }
    std::uint32_t flags = 0;
    if (const spa_pod* mv = prop_values(param, SPA_FORMAT_VIDEO_modifier, &n, &choice, &flags); mv && spa_pod_is_long(mv) && n > 0) {
        nf.dmabuf = true;
        const auto* vals = static_cast<const std::int64_t*>(SPA_POD_BODY_CONST(mv));
        nf.modifier = static_cast<std::uint64_t>(vals[0]);
        // A choice of several values is not fixated yet: mutter fixates it and sends the Format again.
        nf.fixed = choice == SPA_CHOICE_None || std::all_of(vals, vals + n, [&](std::int64_t v) { return v == vals[0]; });
    }
    return nf;
}

std::vector<const spa_pod*> build_buffer_params(bool dmabuf, spa_pod_builder* b) {
    std::vector<const spa_pod*> out;
    auto add = [&](void* pod) {
        if (pod) out.push_back(static_cast<const spa_pod*>(pod)); // null: the builder ran out of room
    };
    if (dmabuf) {
        // At least MIN_DMABUF_BUFFERS: the reader holds the newest, and appsrc and the tiers hold a few.
        add(spa_pod_builder_add_object(b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_buffers,
                                       SPA_POD_CHOICE_RANGE_Int(MIN_DMABUF_BUFFERS, MIN_DMABUF_BUFFERS, 16), SPA_PARAM_BUFFERS_dataType,
                                       SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_DmaBuf)));
    } else {
        add(spa_pod_builder_add_object(b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers, SPA_PARAM_BUFFERS_dataType,
                                       SPA_POD_CHOICE_FLAGS_Int((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr))));
    }
    add(spa_pod_builder_add_object(b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta, SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor), SPA_PARAM_META_size,
                                   SPA_POD_CHOICE_RANGE_Int(static_cast<int>(cursor_meta_size(64)), static_cast<int>(cursor_meta_size(1)),
                                                            static_cast<int>(cursor_meta_size(MAX_CURSOR_SIDE)))));
    return out;
}

bool has_pixels(const spa_buffer* sb) {
    if (!sb || sb->n_datas == 0 || !sb->datas[0].chunk) return false;
    const spa_chunk* c = sb->datas[0].chunk;
    return c->size > 0 && !(c->flags & SPA_CHUNK_FLAG_CORRUPTED);
}

// ------------------------------------------------------------------- frames

/// The release path. A token (an empty GstBuffer) stands for one delivery of a DMA-BUF buffer: every
/// GstBuffer pushed over it carries a GstParentBufferMeta on the token, which copies of the buffer
/// carry too, so the token dies when the last of them does, and the reader holds a ref of its own on
/// the newest. Its death is posted here and drained on the PipeWire thread. It outlives Frames:
/// GStreamer may hold buffers after the reader is gone.
struct Frames::Channel {
    std::mutex mu;
    bool alive = true;
    std::function<void()> wake;
    std::vector<std::pair<pw_buffer*, std::uint64_t>> released;
};

namespace {
struct TokenCtx {
    std::shared_ptr<void> channel; // Frames::Channel, type-erased (it is private)
    pw_buffer* pw;
    std::uint64_t delivery;
    void (*post)(void* channel, pw_buffer*, std::uint64_t);
};
void on_token_gone(gpointer data, GstMiniObject*) {
    auto* t = static_cast<TokenCtx*>(data);
    t->post(t->channel.get(), t->pw, t->delivery);
    delete t;
}
} // namespace

Frames::Frames(std::function<void(pw_buffer*)> requeue, std::function<void()> wake)
    : requeue_(std::move(requeue)), channel_(std::make_shared<Channel>()), dmabuf_allocator_(gst_dmabuf_allocator_new()) {
    channel_->wake = std::move(wake);
    media::frame_repeat_meta_info();
}

Frames::~Frames() {
    {
        std::lock_guard<std::mutex> lk(channel_->mu);
        channel_->alive = false; // tokens that die later are nobody's business
    }
    std::lock_guard<std::mutex> lk(mu_);
    held_token_.reset(); // after `alive` is false: its release posts nothing
    for (auto& [_, s] : slots_) {
        for (GstMemory* m : s.planes) gst_memory_unref(m);
        if (s.map) ::munmap(s.map, s.map_size);
    }
    // The rest goes with its owners: the allocator after the memories in flight, which keep a ref.
}

namespace {
bool same_format(const NegotiatedFormat& a, const NegotiatedFormat& b) {
    return a.format == b.format && a.width == b.width && a.height == b.height && a.dmabuf == b.dmabuf && a.modifier == b.modifier;
}
} // namespace

void Frames::set_format(const NegotiatedFormat& f) {
    if (!f.fixed || f.width <= 0 || f.height <= 0) return;
    GstCaps* caps = nullptr;
    if (f.dmabuf) {
        // spec: docs/23-agent-core-architecture.md#desktop-capture-memory (Wrapped, not copied)
        caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "DMA_DRM", "drm-format", G_TYPE_STRING, f.drm_format().c_str(), "width", G_TYPE_INT,
                                   f.width, "height", G_TYPE_INT, f.height, "framerate", GST_TYPE_FRACTION, 0, 1, nullptr);
        gst_caps_set_features(caps, 0, gst_caps_features_new_single_static_str(GST_CAPS_FEATURE_MEMORY_DMABUF));
    } else {
        const char* fmt = f.format == SPA_VIDEO_FORMAT_BGRA ? "BGRA" : f.format == SPA_VIDEO_FORMAT_RGBx ? "RGBx" : f.format == SPA_VIDEO_FORMAT_RGBA ? "RGBA" : "BGRx";
        caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, fmt, "width", G_TYPE_INT, f.width, "height", G_TYPE_INT, f.height, "framerate",
                                   GST_TYPE_FRACTION, 0, 1, nullptr);
    }
    BufferRef old_token, old_shm; // released outside mu_: a token's release may run there
    {
        std::lock_guard<std::mutex> lk(mu_);
        if (!same_format(format_, f)) {
            // The newest frame was made for the old format: re-sent under the new caps, a smaller
            // buffer would be read as a larger one. The next delivered frame is the newest.
            old_token = std::move(held_token_);
            held_ = nullptr;
            old_shm = std::move(last_shm_);
        }
        format_ = f;
        caps_.reset(caps);
        if (appsrc_ && !suspended_) gst_app_src_set_caps(GST_APP_SRC(appsrc_.get()), caps_.get());
    }
}

void Frames::add_buffer(pw_buffer* b) {
    spa_buffer* sb = b->buffer;
    Slot s;
    s.pw = b;
    std::string unwrappable;
    const bool dmabuf = sb->n_datas > 0 && sb->datas[0].type == SPA_DATA_DmaBuf;
    if (dmabuf && sb->n_datas > 1) {
        // spec: docs/23-agent-core-architecture.md#desktop-capture-memory (Wrapped, not copied)
        // An auxiliary or compression plane (Intel CCS, AMD DCC) is not verified on any hardware:
        // GStreamer's VA import counts planes from the DMA_DRM format and falls back to mapping a
        // buffer with more, which copies tiled bytes. Shared memory instead, by name.
        unwrappable = "a DMA-BUF of " + std::to_string(sb->n_datas) + " planes, which the reader does not take yet";
    } else if (dmabuf) {
        // Every plane or none, so plane i is always data i (on_buffer reads its chunk by index).
        for (std::uint32_t i = 0; i < sb->n_datas && unwrappable.empty(); i++) {
            const spa_data& d = sb->datas[i];
            if (d.type != SPA_DATA_DmaBuf || d.fd < 0) {
                unwrappable = "a DMA-BUF plane without a descriptor";
                break;
            }
            // Wrapped once, on a duplicate of its descriptor, and reused for every delivery, as
            // pipewiresrc's pool does, so VA keeps its import of it. Never mapped.
            const int fd = ::fcntl(static_cast<int>(d.fd), F_DUPFD_CLOEXEC, 0);
            if (fd < 0) {
                unwrappable = std::string("a DMA-BUF descriptor that could not be duplicated: ") + std::strerror(errno);
                break;
            }
            gsize size = static_cast<gsize>(d.mapoffset) + d.maxsize;
            if (d.maxsize == 0) {
                // The duplicate shares mutter's file offset: put it back after asking the size.
                const off_t at = ::lseek(fd, 0, SEEK_CUR);
                const off_t end = ::lseek(fd, 0, SEEK_END);
                if (at >= 0) ::lseek(fd, at, SEEK_SET);
                size = end > 0 ? static_cast<gsize>(end) : 0;
            }
            GstMemory* m = size > 0 ? gst_dmabuf_allocator_alloc(dmabuf_allocator_.get(), fd, size) : nullptr;
            if (!m) {
                ::close(fd);
                unwrappable = "a DMA-BUF plane GStreamer could not wrap";
                break;
            }
            if (d.maxsize > 0) gst_memory_resize(m, d.mapoffset, d.maxsize);
            s.planes.push_back(m);
        }
        if (!unwrappable.empty()) {
            for (GstMemory* m : s.planes) gst_memory_unref(m);
            s.planes.clear(); // on_buffer drops its deliveries: no frame, straight back
        }
    }
    if (!dmabuf && sb->n_datas > 0 && sb->datas[0].type == SPA_DATA_MemFd && !sb->datas[0].data && sb->datas[0].fd >= 0) {
        // Shared memory PipeWire did not map: a stream that offered DMA-BUF links without
        // PW_STREAM_FLAG_MAP_BUFFERS, and mutter may still pick shared memory.
        const spa_data& d = sb->datas[0];
        s.map_size = static_cast<std::size_t>(d.mapoffset) + d.maxsize;
        void* p = ::mmap(nullptr, s.map_size, PROT_READ, MAP_SHARED, static_cast<int>(d.fd), 0);
        if (p != MAP_FAILED) s.map = p;
    }
    std::lock_guard<std::mutex> lk(mu_);
    slots_[b] = std::move(s);
    if (!unwrappable.empty() && !unwrappable_said_) {
        unwrappable_said_ = true;
        unwrappable_ = unwrappable;
    }
}

std::optional<std::string> Frames::take_unwrappable() {
    std::lock_guard<std::mutex> lk(mu_);
    auto out = std::move(unwrappable_);
    unwrappable_.reset();
    return out;
}

void Frames::remove_buffer(pw_buffer* b) {
    BufferRef drop; // released outside mu_: its release may run there
    std::lock_guard<std::mutex> lk(mu_);
    auto it = slots_.find(b);
    if (it == slots_.end()) return;
    if (held_ == b) {
        drop = std::move(held_token_); // the newest frame goes with its buffer: a renegotiation brings new ones
        held_ = nullptr;
    }
    for (GstMemory* m : it->second.planes) gst_memory_unref(m); // buffers in flight keep theirs
    if (it->second.map) ::munmap(it->second.map, it->second.map_size);
    slots_.erase(it);
}

GstBuffer* Frames::wrap_locked(const Slot& s, GstBuffer* token, bool repeat) const {
    GstBuffer* out = gst_buffer_new();
    for (GstMemory* m : s.planes) gst_buffer_append_memory(out, gst_memory_ref(m));
    const GstVideoFormat vf = gst_video_dma_drm_fourcc_to_format(drm_fourcc_of(format_.format));
    gst_buffer_add_video_meta_full(out, GST_VIDEO_FRAME_FLAG_NONE, vf == GST_VIDEO_FORMAT_UNKNOWN ? GST_VIDEO_FORMAT_BGRx : vf, static_cast<guint>(format_.width),
                                   static_cast<guint>(format_.height), static_cast<guint>(s.planes.size()), const_cast<gsize*>(s.offset), const_cast<gint*>(s.stride));
    gst_buffer_add_parent_buffer_meta(out, token); // the token lives while this buffer, or a copy of it, does
    if (repeat) media::mark_repeat(out);
    return out;
}

bool Frames::push_locked(GstBuffer* buf) {
    last_push_ = std::chrono::steady_clock::now();
    if (!appsrc_ || suspended_) {
        gst_buffer_unref(buf);
        return false;
    }
    // A dropped old one may release its token here: that only posts. FLUSHING: the appsrc of a
    // pipeline that was torn down, which the source keeps until a new one is built.
    return gst_app_src_push_buffer(GST_APP_SRC(appsrc_.get()), buf) == GST_FLOW_OK;
}

bool Frames::repeat_newest_locked() {
    if (held_ && held_token_) {
        auto it = slots_.find(held_);
        if (it == slots_.end()) return false;
        return push_locked(wrap_locked(it->second, held_token_.get(), true)); // a new GstBuffer over the same memory
    }
    if (last_shm_) {
        GstBuffer* again = gst_buffer_copy(last_shm_.get());
        media::mark_repeat(again);
        return push_locked(again);
    }
    return false;
}

GstBuffer* Frames::copy_shm_locked(const Slot* slot, const spa_buffer* sb) const {
    const spa_data& d = sb->datas[0];
    const auto* base = static_cast<const std::uint8_t*>(d.data);
    if (!base && slot && slot->map) base = static_cast<const std::uint8_t*>(slot->map) + d.mapoffset;
    const int width = format_.width, height = format_.height;
    if (!base || width <= 0 || height <= 0) return nullptr;
    const int stride = d.chunk->stride > 0 ? d.chunk->stride : width * 4;
    const std::size_t row = static_cast<std::size_t>(width) * 4;
    if (d.maxsize > 0 && static_cast<std::size_t>(d.chunk->offset) + static_cast<std::size_t>(stride) * (height - 1) + row > d.maxsize)
        return nullptr; // a chunk that does not fit its memory (a size change in flight): never read past it
    GstBuffer* frame = gst_buffer_new_allocate(nullptr, row * height, nullptr);
    GstMapInfo map;
    if (!gst_buffer_map(frame, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(frame);
        return nullptr;
    }
    const std::uint8_t* src = base + d.chunk->offset;
    for (int y = 0; y < height; y++) std::memcpy(map.data + row * y, src + static_cast<std::size_t>(stride) * y, row);
    gst_buffer_unmap(frame, &map);
    return frame;
}

Delivery Frames::on_buffer(pw_buffer* b) {
    spa_buffer* sb = b->buffer;
    BufferRef previous; // released outside mu_ (declared first, so it goes after the lock)
    std::unique_lock<std::mutex> lk(mu_);
    if (!has_pixels(sb)) {
        counts_.cursor_only++;
        return Delivery::CursorOnly; // only the cursor: nothing to hold
    }
    auto it = slots_.find(b);
    Slot* slot = it == slots_.end() ? nullptr : &it->second;
    if (slot && !slot->planes.empty() && format_.dmabuf) {
        // DMA-BUF: pushed without a copy, and held while GStreamer holds it. A token stands for this
        // delivery; it dies when the last GstBuffer over it does (see Channel), and the buffer then
        // goes back to mutter, so mutter never records into a frame the encoder may still read.
        slot->out = true;
        slot->delivery = ++deliveries_;
        gsize at = 0;
        for (std::size_t i = 0; i < slot->planes.size(); i++) {
            const spa_chunk* c = sb->datas[i].chunk;
            slot->offset[i] = at + (c ? c->offset : 0);
            slot->stride[i] = c && c->stride > 0 ? c->stride : format_.width * 4;
            at += gst_memory_get_sizes(slot->planes[i], nullptr, nullptr);
        }
        GstBuffer* token = gst_buffer_new();
        gst_mini_object_weak_ref(GST_MINI_OBJECT(token), on_token_gone,
                                 new TokenCtx{channel_, b, slot->delivery, [](void* ch, pw_buffer* pw, std::uint64_t delivery) {
                                                  auto* c = static_cast<Channel*>(ch);
                                                  std::lock_guard<std::mutex> l(c->mu);
                                                  if (!c->alive) return;
                                                  c->released.emplace_back(pw, delivery);
                                                  if (c->wake) c->wake(); // never re-enters Frames
                                              }});
        previous = std::move(held_token_); // the frame before: back to mutter once GStreamer is done
        held_ = b;
        held_token_.reset(token); // our ref: the newest frame, for the keepalive and a new pipeline
        counts_.fresh++;
        counts_.newest = std::chrono::steady_clock::now();
        push_locked(wrap_locked(*slot, token, false));
        lk.unlock();
        return Delivery::Held;
    }
    // Shared memory: copied, and straight back to mutter (as before ADR-0041). A DMA-BUF slot
    // without planes (unwrappable, take_unwrappable) has no data to copy and is dropped here.
    GstBuffer* frame = copy_shm_locked(slot, sb);
    if (!frame) return Delivery::Dropped;
    last_shm_.reset(frame);
    counts_.fresh++;
    counts_.newest = std::chrono::steady_clock::now();
    push_locked(gst_buffer_copy(frame)); // a shallow copy: appsrc stamps it, and the frame may go out again
    return Delivery::Copied;
}

void Frames::keepalive(int interval_ms) {
    std::lock_guard<std::mutex> lk(mu_);
    if (std::chrono::steady_clock::now() - last_push_ < std::chrono::milliseconds(interval_ms)) return;
    // spec: docs/23-agent-core-architecture.md#desktop-capture-memory (The keepalive with GPU buffers)
    if (repeat_newest_locked()) counts_.repeated++;
}

void Frames::attach(GstElement* appsrc) {
    std::lock_guard<std::mutex> lk(mu_);
    if (appsrc_.get() == appsrc) return;
    appsrc_.reset(appsrc ? GST_ELEMENT(gst_object_ref(appsrc)) : nullptr);
    if (!appsrc_ || suspended_) return; // suspended: fed from resume()
    if (caps_) gst_app_src_set_caps(GST_APP_SRC(appsrc_.get()), caps_.get());
    // A new viewer's first frame now, not at the next damage: marked, it repeats what was sent.
    if (repeat_newest_locked()) counts_.repeated++;
}

void Frames::suspend() {
    std::lock_guard<std::mutex> lk(mu_);
    suspended_ = true;
}

void Frames::resume() {
    std::lock_guard<std::mutex> lk(mu_);
    if (!suspended_) return;
    suspended_ = false;
    if (!appsrc_) return;
    if (caps_) gst_app_src_set_caps(GST_APP_SRC(appsrc_.get()), caps_.get());
    if (repeat_newest_locked()) counts_.repeated++;
}

void Frames::drain_releases() {
    std::vector<std::pair<pw_buffer*, std::uint64_t>> released;
    {
        std::lock_guard<std::mutex> lk(channel_->mu);
        released.swap(channel_->released);
    }
    std::vector<pw_buffer*> back;
    {
        std::lock_guard<std::mutex> lk(mu_);
        for (const auto& [pw, delivery] : released) {
            auto it = slots_.find(pw);
            // Gone (removed in a renegotiation) or delivered again since: nothing to give back.
            if (it == slots_.end() || !it->second.out || it->second.delivery != delivery) continue;
            it->second.out = false;
            back.push_back(pw);
        }
    }
    for (pw_buffer* pw : back) requeue_(pw);
}

FrameCounts Frames::counts() const {
    std::lock_guard<std::mutex> lk(mu_);
    return counts_;
}

bool Frames::has_frame() const {
    std::lock_guard<std::mutex> lk(mu_);
    return held_token_ != nullptr || last_shm_ != nullptr;
}

std::size_t Frames::buffers_out() const {
    std::lock_guard<std::mutex> lk(mu_);
    return static_cast<std::size_t>(std::count_if(slots_.begin(), slots_.end(), [](const auto& s) { return s.second.out; }));
}

GstCaps* Frames::caps() const {
    std::lock_guard<std::mutex> lk(mu_);
    return caps_ ? gst_caps_ref(caps_.get()) : nullptr;
}

} // namespace fjarr::desktop::mutter
