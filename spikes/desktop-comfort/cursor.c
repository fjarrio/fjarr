// Throwaway spike (M3 3.5): does headless mutter put SPA_META_Cursor on a ScreenCast stream
// recorded with cursor-mode=2 ("metadata")? Connects to <node>, asks for the cursor meta, and
// prints every change of position, hotspot and bitmap for <seconds>.
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/buffers.h>
#include <spa/debug/types.h>
#include <stdio.h>
#include <stdlib.h>

#define CURSOR_META_SIZE(w, h) (sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) + (w) * (h) * 4)

struct data {
    struct pw_main_loop* loop;
    struct pw_stream* stream;
    int frames, with_meta, bitmaps;
    int last_x, last_y;
    uint32_t last_id;
};

static void on_process(void* userdata) {
    struct data* d = userdata;
    struct pw_buffer* b = pw_stream_dequeue_buffer(d->stream);
    if (!b) return;
    d->frames++;
    struct spa_meta_cursor* mc = spa_buffer_find_meta_data(b->buffer, SPA_META_Cursor, sizeof(*mc));
    if (mc && getenv("CURSOR_RAW") && d->frames <= 8) {
        printf("raw frame %d: id=%u pos=%d,%d hotspot=%d,%d bitmap_offset=%u", d->frames, mc->id, mc->position.x, mc->position.y, mc->hotspot.x, mc->hotspot.y, mc->bitmap_offset);
        if (mc->bitmap_offset >= sizeof(*mc)) {
            struct spa_meta_bitmap* bm = SPA_PTROFF(mc, mc->bitmap_offset, struct spa_meta_bitmap);
            printf(" bitmap %ux%u fmt=%u stride=%d offset=%u", bm->size.width, bm->size.height, bm->format, bm->stride, bm->offset);
        }
        printf("\n");
    } else if (!mc && getenv("CURSOR_RAW") && d->frames <= 8) printf("raw frame %d: no cursor meta\n", d->frames);
    if (mc && spa_meta_cursor_is_valid(mc)) {
        d->with_meta++;
        if (mc->position.x != d->last_x || mc->position.y != d->last_y || mc->id != d->last_id) {
            printf("cursor id=%u pos=%d,%d hotspot=%d,%d", mc->id, mc->position.x, mc->position.y, mc->hotspot.x, mc->hotspot.y);
            if (mc->bitmap_offset >= sizeof(*mc)) {
                struct spa_meta_bitmap* bm = SPA_PTROFF(mc, mc->bitmap_offset, struct spa_meta_bitmap);
                if (bm->size.width > 0) {
                    d->bitmaps++;
                    printf(" bitmap=%ux%u format=%s stride=%d", bm->size.width, bm->size.height,
                           spa_debug_type_find_short_name(spa_type_video_format, bm->format), bm->stride);
                }
            }
            printf("\n");
            fflush(stdout);
            d->last_x = mc->position.x; d->last_y = mc->position.y; d->last_id = mc->id;
        }
    }
    pw_stream_queue_buffer(d->stream, b);
}

static void on_param_changed(void* userdata, uint32_t id, const struct spa_pod* param) {
    struct data* d = userdata;
    if (!param || id != SPA_PARAM_Format) return;
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod* params[1];
    params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
        SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(CURSOR_META_SIZE(64, 64), CURSOR_META_SIZE(1, 1), CURSOR_META_SIZE(384, 384)));
    pw_stream_update_params(d->stream, params, 1);
    printf("format negotiated; asked for SPA_META_Cursor\n");
    fflush(stdout);
}

static const struct pw_stream_events events = {PW_VERSION_STREAM_EVENTS, .param_changed = on_param_changed, .process = on_process};

static int quit_cb(void* d) { pw_main_loop_quit(((struct data*)d)->loop); return 0; }

int main(int argc, char** argv) {
    if (argc < 3) { fprintf(stderr, "usage: cursor <node> <seconds>\n"); return 2; }
    pw_init(&argc, &argv);
    struct data d = {0};
    d.last_x = d.last_y = -1;
    d.loop = pw_main_loop_new(NULL);
    d.stream = pw_stream_new_simple(pw_main_loop_get_loop(d.loop), "cursor-spike",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", NULL), &events, &d);
    uint8_t buf[1024];
    struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
    const struct spa_pod* params[1];
    params[0] = spa_pod_builder_add_object(&b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(3, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA));
    pw_stream_connect(d.stream, PW_DIRECTION_INPUT, (uint32_t)atoi(argv[1]), PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, params, 1);
    struct pw_loop* l = pw_main_loop_get_loop(d.loop);
    struct timespec t = {atoi(argv[2]), 0};
    struct spa_source* timer = pw_loop_add_timer(l, (spa_source_timer_func_t)(void*)quit_cb, &d);
    (void)timer;
    pw_loop_update_timer(l, timer, &t, NULL, false);
    pw_main_loop_run(d.loop);
    printf("SUMMARY frames=%d with_cursor_meta=%d bitmaps=%d\n", d.frames, d.with_meta, d.bitmaps);
    pw_stream_destroy(d.stream);
    pw_main_loop_destroy(d.loop);
    return d.with_meta > 0 ? 0 : 1;
}
