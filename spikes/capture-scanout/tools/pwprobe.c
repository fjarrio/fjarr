// pwprobe.c: a raw pw_stream consumer of a mutter ScreenCast node, for the capture-scanout spike.
// Built from spikes/desktop-comfort/cursor.c. It negotiates DMA-BUF with the given modifiers (or
// SHM), asks for the header, cursor, crop and damage metas (and, with --sync, SPA_META_SyncTimeline,
// i.e. explicit sync), and logs every buffer: data type, chunk, header, cursor position and bitmap,
// damage, sync points, and (with --sample) the colour of 64 pixels sampled from the frame (the test
// window is one flat colour, so tiling does not matter for the colour of a LINEAR buffer; tiled
// buffers cannot be mapped: mmap fails with EPERM).
//
// usage: pwprobe NODE SECONDS OUTLOG [--dmabuf MOD[,MOD...]] [--sync] [--sample] [--copy] [--bitmaps DIR]
//   NODE           the ScreenCast stream's PipeWire node id (castsession.py writes it)
//   --dmabuf MODS  offer BGRx and BGRA with these DRM modifiers (hex), DMA-BUF only;
//                  without it: BGRx/BGRA without a modifier, i.e. SHM (MemFd)
//   --sync         announce SPA_META_SyncTimeline together with a Buffers param whose metaType asks
//                  for it (announcing only the meta made gnome-shell 50.1 log assertion criticals);
//                  wait on each pixel buffer's acquire point before reading and signal its release
//                  point after (DRM syncobj on PWPROBE_DRM, default /dev/dri/renderD128)
//   --sample       map each frame (DMA-BUF: mmap + DMA_BUF_IOCTL_SYNC) and classify 64 pixels
//   --copy         --sample, plus memcpy the whole frame and log what that costs (a CPU reader)
//   --bitmaps DIR  write each new cursor bitmap to DIR/cursor-<n>.pam
// PWPROBE_STOP=FILE ends it early when FILE appears.
//
// build (the dev container has the headers):
//   gcc -O2 -Wall -Wextra -Wno-missing-field-initializers -o pwprobe pwprobe.c
//       $(pkg-config --cflags --libs libpipewire-0.3 libdrm)          (one line)
// run as the desktop user (as-desktop.sh), after castsession.py has a node.
#include <errno.h>
#include <fcntl.h>
#include <linux/dma-buf.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/debug/types.h>
#include <spa/param/buffers.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>
#include <xf86drm.h>

#define CURSOR_META_SIZE(w, h) (sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) + (w) * (h) * 4)
#define MAX_MODS 32
#define MAX_MAPS 32

struct map { int fd; void* ptr; size_t size; };
struct sobj { int fd; uint32_t handle; };

struct data {
    struct pw_main_loop* loop;
    struct pw_stream* stream;
    FILE* out;
    int dmabuf, sync, sample, copy;
    uint8_t* dst;
    size_t dst_size;
    long copies;
    double copy_ms_sum, copy_ms_max, t_first;
    struct rusage ru_first;
    const char* bitmap_dir;
    uint64_t mods[MAX_MODS];
    int n_mods;
    struct spa_video_info_raw fmt;
    int drm_fd;
    struct map maps[MAX_MAPS];
    struct sobj sobjs[MAX_MAPS];
    int map_failed;
    long n, full, cursor_only, with_cursor, bitmaps, magenta, green, other_frames, sync_waits, sync_fail;
    int last_x, last_y;
    uint32_t last_id;
};

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

#define LOG(d, ...) do { fprintf((d)->out, "%.3f ", now()); fprintf((d)->out, __VA_ARGS__); fputc('\n', (d)->out); fflush((d)->out); } while (0)

static const char* dtype(uint32_t t) {
    switch (t) {
    case SPA_DATA_MemPtr: return "MemPtr";
    case SPA_DATA_MemFd: return "MemFd";
    case SPA_DATA_DmaBuf: return "DmaBuf";
    case SPA_DATA_SyncObj: return "SyncObj";
    default: return "other";
    }
}

static void* find_map(struct data* d, int fd, size_t* size) {
    for (int i = 0; i < MAX_MAPS; i++)
        if (d->maps[i].ptr && d->maps[i].fd == fd) { *size = d->maps[i].size; return d->maps[i].ptr; }
    return NULL;
}

static uint32_t find_sobj(struct data* d, int fd) {
    for (int i = 0; i < MAX_MAPS; i++)
        if (d->sobjs[i].fd == fd && d->sobjs[i].handle) return d->sobjs[i].handle;
    for (int i = 0; i < MAX_MAPS; i++)
        if (!d->sobjs[i].handle) {
            uint32_t h = 0;
            if (drmSyncobjFDToHandle(d->drm_fd, fd, &h) != 0) {
                LOG(d, "syncobj import of fd %d failed: %s", fd, strerror(errno));
                return 0;
            }
            d->sobjs[i].fd = fd;
            d->sobjs[i].handle = h;
            LOG(d, "syncobj fd %d imported as handle %u", fd, h);
            return h;
        }
    return 0;
}

static void on_add_buffer(void* userdata, struct pw_buffer* b) {
    struct data* d = userdata;
    struct spa_buffer* sb = b->buffer;
    char line[512];
    int o = snprintf(line, sizeof(line), "add_buffer n_datas=%u n_metas=%u:", sb->n_datas, sb->n_metas);
    for (uint32_t i = 0; i < sb->n_datas && o < (int)sizeof(line); i++)
        o += snprintf(line + o, sizeof(line) - o, " [%u %s fd=%ld maxsize=%u flags=0x%x]", i, dtype(sb->datas[i].type),
                      (long)sb->datas[i].fd, sb->datas[i].maxsize, sb->datas[i].flags);
    for (uint32_t i = 0; i < sb->n_metas && o < (int)sizeof(line); i++)
        o += snprintf(line + o, sizeof(line) - o, " meta=%s(%u)", spa_debug_type_find_short_name(spa_type_meta_type, sb->metas[i].type),
                      sb->metas[i].size);
    LOG(d, "%s", line);
    if (!d->sample || d->map_failed) return;
    struct spa_data* d0 = &sb->datas[0];
    if (d0->type == SPA_DATA_DmaBuf || (d0->type == SPA_DATA_MemFd && !d0->data)) {
        size_t size = d0->type == SPA_DATA_DmaBuf ? (size_t)lseek((int)d0->fd, 0, SEEK_END) : d0->maxsize + d0->mapoffset;
        void* p = mmap(NULL, size, PROT_READ, MAP_SHARED, (int)d0->fd, 0);
        if (p == MAP_FAILED) {
            LOG(d, "mmap of %s fd %ld (%zu bytes) failed: %s; sampling off", dtype(d0->type), (long)d0->fd, size, strerror(errno));
            d->map_failed = 1;
            return;
        }
        for (int i = 0; i < MAX_MAPS; i++)
            if (!d->maps[i].ptr) { d->maps[i] = (struct map){(int)d0->fd, p, size}; break; }
        LOG(d, "mapped %s fd %ld, %zu bytes", dtype(d0->type), (long)d0->fd, size);
    }
}

static void on_remove_buffer(void* userdata, struct pw_buffer* b) {
    struct data* d = userdata;
    struct spa_data* d0 = &b->buffer->datas[0];
    for (int i = 0; i < MAX_MAPS; i++)
        if (d->maps[i].ptr && d->maps[i].fd == (int)d0->fd) {
            munmap(d->maps[i].ptr, d->maps[i].size);
            d->maps[i].ptr = NULL;
        }
    for (uint32_t k = 1; k < b->buffer->n_datas; k++)
        if (b->buffer->datas[k].type == SPA_DATA_SyncObj)
            for (int i = 0; i < MAX_MAPS; i++)
                if (d->sobjs[i].handle && d->sobjs[i].fd == (int)b->buffer->datas[k].fd) {
                    drmSyncobjDestroy(d->drm_fd, d->sobjs[i].handle);
                    d->sobjs[i].handle = 0;
                }
    LOG(d, "remove_buffer fd=%ld", (long)d0->fd);
}

// 64 pixels spread over the frame: count magenta / green / other.
static void classify(struct data* d, const uint8_t* base, size_t size, uint32_t offset, int32_t stride, char* out, size_t outlen) {
    int m = 0, g = 0, x = 0;
    uint32_t h = d->fmt.size.height ? d->fmt.size.height : 1;
    uint64_t span = (uint64_t)(stride > 0 ? (uint32_t)stride : d->fmt.size.width * 4) * h;
    for (int i = 0; i < 64; i++) {
        uint64_t off = offset + ((((uint64_t)i * 2654435761u) + i * 977) % (span / 4)) * 4;
        if (off + 4 > size) continue;
        const uint8_t* p = base + off;
        int B = p[0], G = p[1], R = p[2];
        if (R > 200 && B > 200 && G < 60) m++;
        else if (G > 200 && R < 60 && B < 60) g++;
        else x++;
    }
    const char* verdict = m == 64 ? "magenta" : g == 64 ? "green" : "mixed";
    if (m == 64) d->magenta++;
    else if (g == 64) d->green++;
    else d->other_frames++;
    snprintf(out, outlen, " colour=%s(m%d,g%d,o%d)", verdict, m, g, x);
}

static void dump_bitmap(struct data* d, struct spa_meta_cursor* mc, struct spa_meta_bitmap* bm) {
    if (!d->bitmap_dir || bm->size.width == 0 || bm->size.height == 0) return;
    char path[512];
    snprintf(path, sizeof(path), "%s/cursor-%ld-%ux%u.pam", d->bitmap_dir, d->bitmaps, bm->size.width, bm->size.height);
    FILE* f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "P7\nWIDTH %u\nHEIGHT %u\nDEPTH 4\nMAXVAL 255\nTUPLTYPE RGB_ALPHA\nENDHDR\n", bm->size.width, bm->size.height);
    const uint8_t* px = SPA_PTROFF(bm, bm->offset, uint8_t);
    for (uint32_t y = 0; y < bm->size.height; y++) fwrite(px + y * bm->stride, 1, bm->size.width * 4, f);
    fclose(f);
    (void)mc;
}

static void on_process(void* userdata) {
    struct data* d = userdata;
    struct pw_buffer* b = pw_stream_dequeue_buffer(d->stream);
    if (!b) return;
    struct spa_buffer* sb = b->buffer;
    struct spa_data* d0 = &sb->datas[0];
    d->n++;
    char line[2048];
    int o = 0;
    struct spa_meta_header* hdr = spa_buffer_find_meta_data(sb, SPA_META_Header, sizeof(*hdr));
    o += snprintf(line + o, sizeof(line) - o, "buf n=%ld", d->n);
    if (hdr) o += snprintf(line + o, sizeof(line) - o, " seq=%" PRIu64 " pts=%" PRIi64 " hflags=0x%x", hdr->seq, hdr->pts, hdr->flags);
    o += snprintf(line + o, sizeof(line) - o, " %s fd=%ld chunk(off=%u size=%u stride=%d flags=0x%x)", dtype(d0->type), (long)d0->fd,
                  d0->chunk->offset, d0->chunk->size, d0->chunk->stride, d0->chunk->flags);
    int has_pixels = d0->chunk->size > 0 && !(d0->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED);
    if (has_pixels) d->full++;
    else d->cursor_only++;

    // explicit sync: wait for the acquire point before touching the pixels
    struct spa_meta_sync_timeline* st = spa_buffer_find_meta_data(sb, SPA_META_SyncTimeline, sizeof(*st));
    uint32_t rel_handle = 0;
    if (st && sb->n_datas >= 3) {
        uint32_t acq_handle = find_sobj(d, (int)sb->datas[1].fd);
        rel_handle = find_sobj(d, (int)sb->datas[2].fd);
        o += snprintf(line + o, sizeof(line) - o, " sync(acq=%" PRIu64 " rel=%" PRIu64 ")", st->acquire_point, st->release_point);
        if (acq_handle && has_pixels) {
            struct timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            int64_t deadline = (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec + 1000000000LL;
            uint64_t point = st->acquire_point;
            double t0 = now();
            int r = drmSyncobjTimelineWait(d->drm_fd, &acq_handle, &point, 1, deadline, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT, NULL);
            d->sync_waits++;
            if (r != 0) d->sync_fail++;
            o += snprintf(line + o, sizeof(line) - o, " acq-wait=%s/%.2fms", r == 0 ? "ok" : strerror(errno), (now() - t0) * 1000);
        }
    }

    if (has_pixels && d->t_first == 0) {
        d->t_first = now();
        getrusage(RUSAGE_SELF, &d->ru_first);
    }
    if (d->copy && has_pixels && !d->map_failed) {
        // --copy: what a CPU reader pays for the whole frame (as module E's reader copies SHM today)
        size_t size = 0;
        const uint8_t* base = NULL;
        if (d0->type == SPA_DATA_DmaBuf) base = find_map(d, (int)d0->fd, &size);
        else if (d0->data) { base = d0->data; size = d0->maxsize; }
        size_t n = (size_t)d0->chunk->size;
        if (base && d0->chunk->offset + n <= size) {
            if (d->dst_size < n) { free(d->dst); d->dst = malloc(n); d->dst_size = n; }
            double t0 = now();
            struct dma_buf_sync s = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
            if (d0->type == SPA_DATA_DmaBuf) ioctl((int)d0->fd, DMA_BUF_IOCTL_SYNC, &s);
            memcpy(d->dst, base + d0->chunk->offset, n);
            if (d0->type == SPA_DATA_DmaBuf) { s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ; ioctl((int)d0->fd, DMA_BUF_IOCTL_SYNC, &s); }
            double ms = (now() - t0) * 1000;
            d->copies++;
            d->copy_ms_sum += ms;
            if (ms > d->copy_ms_max) d->copy_ms_max = ms;
            o += snprintf(line + o, sizeof(line) - o, " copy=%.2fms", ms);
        }
    }
    if (d->sample && has_pixels && !d->map_failed) {
        size_t size = 0;
        const uint8_t* base = NULL;
        if (d0->type == SPA_DATA_DmaBuf) base = find_map(d, (int)d0->fd, &size);
        else if (d0->data) { base = d0->data; size = d0->maxsize; }
        else base = find_map(d, (int)d0->fd, &size);
        if (base) {
            struct dma_buf_sync s = {DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ};
            if (d0->type == SPA_DATA_DmaBuf) ioctl((int)d0->fd, DMA_BUF_IOCTL_SYNC, &s);
            o += 0;
            char col[128];
            classify(d, base, size, d0->chunk->offset, d0->chunk->stride, col, sizeof(col));
            o += snprintf(line + o, sizeof(line) - o, "%s", col);
            if (d0->type == SPA_DATA_DmaBuf) {
                s.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ;
                ioctl((int)d0->fd, DMA_BUF_IOCTL_SYNC, &s);
            }
        }
    }

    struct spa_meta_cursor* mc = spa_buffer_find_meta_data(sb, SPA_META_Cursor, sizeof(*mc));
    if (mc) {
        if (spa_meta_cursor_is_valid(mc)) {
            d->with_cursor++;
            o += snprintf(line + o, sizeof(line) - o, " cursor(id=%u pos=%d,%d hot=%d,%d", mc->id, mc->position.x, mc->position.y,
                          mc->hotspot.x, mc->hotspot.y);
            if (mc->bitmap_offset >= sizeof(*mc)) {
                struct spa_meta_bitmap* bm = SPA_PTROFF(mc, mc->bitmap_offset, struct spa_meta_bitmap);
                uint32_t opaque = 0, hash = 2166136261u;
                if (bm->size.width > 0 && bm->size.height > 0) {
                    const uint8_t* px = SPA_PTROFF(bm, bm->offset, uint8_t);
                    for (uint32_t y = 0; y < bm->size.height; y++)
                        for (uint32_t x = 0; x < bm->size.width; x++) {
                            const uint8_t* p = px + y * bm->stride + x * 4;
                            if (p[3]) opaque++;
                            for (int k = 0; k < 4; k++) hash = (hash ^ p[k]) * 16777619u;
                        }
                    d->bitmaps++;
                    dump_bitmap(d, mc, bm);
                }
                o += snprintf(line + o, sizeof(line) - o, " bitmap=%ux%u fmt=%s stride=%d opaque=%u hash=%08x", bm->size.width,
                              bm->size.height, spa_debug_type_find_short_name(spa_type_video_format, bm->format), bm->stride, opaque, hash);
            }
            o += snprintf(line + o, sizeof(line) - o, ")");
        } else {
            o += snprintf(line + o, sizeof(line) - o, " cursor(invalid id=0)");
        }
    }
    struct spa_meta* dm = spa_buffer_find_meta(sb, SPA_META_VideoDamage);
    if (dm) {
        struct spa_meta_region* r;
        int k = 0;
        o += snprintf(line + o, sizeof(line) - o, " damage[");
        spa_meta_for_each(r, dm) {
            if (!spa_meta_region_is_valid(r)) break;
            if (k++ < 3) o += snprintf(line + o, sizeof(line) - o, "%s%d,%d %ux%u", k > 1 ? " " : "", r->region.position.x, r->region.position.y,
                                       r->region.size.width, r->region.size.height);
        }
        o += snprintf(line + o, sizeof(line) - o, "]%s", k > 3 ? "+" : "");
    }
    LOG(d, "%s", line);

    if (rel_handle && st) {
        uint64_t point = st->release_point;
        if (drmSyncobjTimelineSignal(d->drm_fd, &rel_handle, &point, 1) != 0) {
            d->sync_fail++;
            LOG(d, "release signal failed: %s", strerror(errno));
        }
    }
    pw_stream_queue_buffer(d->stream, b);
}

static void set_buffer_params(struct data* d) {
    uint8_t buf[2048];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod* params[8];
    int n = 0;
    int types = d->dmabuf ? (1 << SPA_DATA_DmaBuf) : ((1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr));
    if (d->sync) {
        // The explicit-sync variant, matching mutter's: plane + acquire + release syncobj blocks, and
        // SPA_PARAM_BUFFERS_metaType (mandatory) = SyncTimeline. Without the metaType prop the filter
        // rejects mutter's mandatory one and the fallback (1 block) wins even with the meta announced.
        struct spa_pod_frame f;
        spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
        spa_pod_builder_add(&b,
            SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
            SPA_PARAM_BUFFERS_blocks, SPA_POD_Int(3),
            SPA_PARAM_BUFFERS_size, SPA_POD_CHOICE_RANGE_Int(0, 0, INT32_MAX),
            SPA_PARAM_BUFFERS_stride, SPA_POD_CHOICE_RANGE_Int(0, 0, INT32_MAX),
            SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(types), 0);
        spa_pod_builder_prop(&b, SPA_PARAM_BUFFERS_metaType, SPA_POD_PROP_FLAG_MANDATORY);
        spa_pod_builder_int(&b, 1 << SPA_META_SyncTimeline);
        params[n++] = spa_pod_builder_pop(&b, &f);
    }
    params[n++] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(8, 2, 16),
        SPA_PARAM_BUFFERS_blocks, SPA_POD_CHOICE_RANGE_Int(1, 1, 4),
        SPA_PARAM_BUFFERS_size, SPA_POD_CHOICE_RANGE_Int(0, 0, INT32_MAX),
        SPA_PARAM_BUFFERS_stride, SPA_POD_CHOICE_RANGE_Int(0, 0, INT32_MAX),
        SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(types));
    params[n++] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Header), SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_header)));
    params[n++] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop), SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_region)));
    params[n++] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
        SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(CURSOR_META_SIZE(384, 384), sizeof(struct spa_meta_cursor), CURSOR_META_SIZE(384, 384)));
    params[n++] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
        SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(sizeof(struct spa_meta_region) * 16, sizeof(struct spa_meta_region), sizeof(struct spa_meta_region) * 16));
    if (d->sync)
        params[n++] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
            SPA_PARAM_META_type, SPA_POD_Id(SPA_META_SyncTimeline), SPA_PARAM_META_size, SPA_POD_Int(sizeof(struct spa_meta_sync_timeline)));
    pw_stream_update_params(d->stream, params, n);
}

static void on_param_changed(void* userdata, uint32_t id, const struct spa_pod* param) {
    struct data* d = userdata;
    if (!param || id != SPA_PARAM_Format) return;
    uint32_t mt, mst;
    if (spa_format_parse(param, &mt, &mst) < 0) return;
    struct spa_video_info_raw f = {0};
    spa_format_video_raw_parse(param, &f);
    const struct spa_pod_prop* pm = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier);
    if (pm) {
        uint32_t nv = 0, choice = 0;
        const struct spa_pod* vals = spa_pod_get_values(&pm->value, &nv, &choice);
        const int64_t* mods = SPA_POD_BODY(vals);
        char m[512];
        int o = 0;
        for (uint32_t i = 0; i < nv && o < (int)sizeof(m); i++) o += snprintf(m + o, sizeof(m) - o, "%s0x%016" PRIx64, i ? "," : "", (uint64_t)mods[i]);
        LOG(d, "format %s %ux%u framerate=%u/%u max=%u/%u modifier-prop flags=0x%x choice=%u values=[%s]%s",
            spa_debug_type_find_short_name(spa_type_video_format, f.format), f.size.width, f.size.height, f.framerate.num, f.framerate.denom,
            f.max_framerate.num, f.max_framerate.denom, pm->flags, choice, m,
            (pm->flags & SPA_POD_PROP_FLAG_DONT_FIXATE) ? " (not fixed yet: the producer fixates)" : " (fixed)");
    } else {
        LOG(d, "format %s %ux%u framerate=%u/%u max=%u/%u no modifier (SHM)", spa_debug_type_find_short_name(spa_type_video_format, f.format),
            f.size.width, f.size.height, f.framerate.num, f.framerate.denom, f.max_framerate.num, f.max_framerate.denom);
    }
    d->fmt = f;
    set_buffer_params(d);
}

static void on_state_changed(void* userdata, enum pw_stream_state old, enum pw_stream_state state, const char* error) {
    struct data* d = userdata;
    LOG(d, "state %s -> %s%s%s", pw_stream_state_as_string(old), pw_stream_state_as_string(state), error ? " error: " : "", error ? error : "");
}

static const struct pw_stream_events events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed,
    .param_changed = on_param_changed,
    .add_buffer = on_add_buffer,
    .remove_buffer = on_remove_buffer,
    .process = on_process,
};

static const struct spa_pod* build_format(struct spa_pod_builder* b, uint32_t format, struct data* d) {
    struct spa_pod_frame f[2];
    struct spa_rectangle def_size = SPA_RECTANGLE(1920, 1080), min_size = SPA_RECTANGLE(1, 1), max_size = SPA_RECTANGLE(16384, 16384);
    struct spa_fraction def_rate = SPA_FRACTION(0, 1), min_rate = SPA_FRACTION(0, 1), max_rate = SPA_FRACTION(1000, 1);
    spa_pod_builder_push_object(b, &f[0], SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);
    if (d->dmabuf) {
        spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
        spa_pod_builder_push_choice(b, &f[1], SPA_CHOICE_Enum, 0);
        spa_pod_builder_long(b, (int64_t)d->mods[0]);
        for (int i = 0; i < d->n_mods; i++) spa_pod_builder_long(b, (int64_t)d->mods[i]);
        spa_pod_builder_pop(b, &f[1]);
    }
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&def_size, &min_size, &max_size), 0);
    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&def_rate, &min_rate, &max_rate), 0);
    return spa_pod_builder_pop(b, &f[0]);
}

static void quit_cb(void* data, uint64_t expirations) {
    (void)expirations;
    pw_main_loop_quit(((struct data*)data)->loop);
}

static void stop_cb(void* data, uint64_t expirations) {
    (void)expirations;
    struct data* d = data;
    const char* stop = getenv("PWPROBE_STOP");
    if (stop && access(stop, F_OK) == 0) pw_main_loop_quit(d->loop);
}

int main(int argc, char** argv) {
    if (argc < 4) {
        fprintf(stderr, "usage: pwprobe NODE SECONDS OUTLOG [--dmabuf MODS] [--sync] [--sample] [--copy] [--bitmaps DIR]\n");
        return 2;
    }
    pw_init(&argc, &argv);
    struct data d = {0};
    d.last_x = d.last_y = -1;
    d.drm_fd = -1;
    d.out = fopen(argv[3], "a");
    if (!d.out) { perror("outlog"); return 2; }
    for (int i = 4; i < argc; i++) {
        if (!strcmp(argv[i], "--dmabuf") && i + 1 < argc) {
            d.dmabuf = 1;
            char* s = strdup(argv[++i]);
            for (char* t = strtok(s, ","); t && d.n_mods < MAX_MODS; t = strtok(NULL, ",")) d.mods[d.n_mods++] = strtoull(t, NULL, 0);
            free(s);
        } else if (!strcmp(argv[i], "--sync")) d.sync = 1;
        else if (!strcmp(argv[i], "--sample")) d.sample = 1;
        else if (!strcmp(argv[i], "--copy")) d.sample = d.copy = 1;
        else if (!strcmp(argv[i], "--bitmaps") && i + 1 < argc) d.bitmap_dir = argv[++i];
    }
    if (d.sync) {
        const char* drm = getenv("PWPROBE_DRM") ? getenv("PWPROBE_DRM") : "/dev/dri/renderD128";
        d.drm_fd = open(drm, O_RDWR | O_CLOEXEC);
        if (d.drm_fd < 0) { LOG(&d, "open %s: %s", drm, strerror(errno)); return 2; }
    }
    char mods[512] = "";
    for (int i = 0, o = 0; i < d.n_mods; i++) o += snprintf(mods + o, sizeof(mods) - o, "%s0x%016" PRIx64, i ? "," : "", d.mods[i]);
    LOG(&d, "start pid=%d node=%s dmabuf=%d mods=[%s] sync=%d sample=%d libpipewire=%s", getpid(), argv[1], d.dmabuf, mods, d.sync, d.sample,
        pw_get_library_version());
    d.loop = pw_main_loop_new(NULL);
    d.stream = pw_stream_new_simple(pw_main_loop_get_loop(d.loop), "capture-scanout-pwprobe",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", NULL), &events, &d);
    uint8_t buf[4096];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod* params[2];
    params[0] = build_format(&b, SPA_VIDEO_FORMAT_BGRx, &d);
    params[1] = build_format(&b, SPA_VIDEO_FORMAT_BGRA, &d);
    int flags = PW_STREAM_FLAG_AUTOCONNECT | (d.dmabuf ? 0 : PW_STREAM_FLAG_MAP_BUFFERS);
    int r = pw_stream_connect(d.stream, PW_DIRECTION_INPUT, (uint32_t)atoi(argv[1]), flags, params, 2);
    if (r < 0) { LOG(&d, "connect failed: %s", strerror(-r)); return 1; }
    struct pw_loop* l = pw_main_loop_get_loop(d.loop);
    struct spa_source* timer = pw_loop_add_timer(l, quit_cb, &d);
    struct timespec t = {(time_t)atof(argv[2]), 0};
    pw_loop_update_timer(l, timer, &t, NULL, false);
    struct spa_source* stopper = pw_loop_add_timer(l, stop_cb, &d);
    struct timespec every = {0, 200000000};
    pw_loop_update_timer(l, stopper, &every, &every, false);
    pw_main_loop_run(d.loop);
    if (d.t_first > 0) {
        struct rusage ru;
        getrusage(RUSAGE_SELF, &ru);
        double cpu = (ru.ru_utime.tv_sec - d.ru_first.ru_utime.tv_sec) + (ru.ru_utime.tv_usec - d.ru_first.ru_utime.tv_usec) / 1e6 +
                     (ru.ru_stime.tv_sec - d.ru_first.ru_stime.tv_sec) + (ru.ru_stime.tv_usec - d.ru_first.ru_stime.tv_usec) / 1e6;
        double wall = now() - d.t_first;
        LOG(&d, "cpu since the first frame: %.2f s over %.1f s = %.1f%% of a core; copies=%ld mean=%.2fms max=%.2fms", cpu, wall,
            100.0 * cpu / wall, d.copies, d.copies ? d.copy_ms_sum / d.copies : 0.0, d.copy_ms_max);
    }
    LOG(&d, "end buffers=%ld full=%ld cursor-only=%ld with-cursor-meta=%ld bitmaps=%ld frames magenta=%ld green=%ld other=%ld sync-waits=%ld sync-failures=%ld",
        d.n, d.full, d.cursor_only, d.with_cursor, d.bitmaps, d.magenta, d.green, d.other_frames, d.sync_waits, d.sync_fail);
    pw_stream_destroy(d.stream);
    pw_main_loop_destroy(d.loop);
    if (d.drm_fd >= 0) close(d.drm_fd);
    return 0;
}

