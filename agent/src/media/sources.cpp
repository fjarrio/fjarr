#include "sources.hpp"

#include <sstream>

#include <fjarr/errors.hpp>
#include <nlohmann/json-schema.hpp>

#include "core/glib/raii.hpp"
#include "core/log.hpp"

namespace fjarr::media {

// ------------------------------------------------------ description

GstDescriptionSource::GstDescriptionSource(std::string description, std::string identity, bool passthrough)
    : description_(std::move(description)), identity_(identity.empty() ? description_ : std::move(identity)), passthrough_(passthrough) {}

std::string GstDescriptionSource::first_factory(const std::string& description) {
    std::istringstream ss(description);
    std::string tok;
    ss >> tok;
    return tok;
}

SourceInfo GstDescriptionSource::describe() const {
    return SourceInfo{identity_, {SourceOutput{"src", TrackKind::Video, passthrough_ ? "video/x-h264" : "video/x-raw"}}};
}

GstBin* GstDescriptionSource::create_bin() {
    GError* err = nullptr;
    GstElement* bin = gst_parse_bin_from_description(description_.c_str(), TRUE, &err);
    if (err) {
        glib::GErrorPtr guard(err);
        if (bin) [[maybe_unused]] auto released = glib::sink_element(bin); // partial bin from a recoverable error: released
        last_error_ = err->message;
        log::error("source", "description failed", {{"description", description_}, {"error", last_error_}});
        return nullptr;
    }
    return GST_BIN(bin);
}

bool GstDescriptionSource::available() const {
    const std::string factory = first_factory(description_);
    if (factory.empty()) return false;
    glib::GstObjectPtr<GstElementFactory> f(gst_element_factory_find(factory.c_str()));
    if (!f) {
        last_error_ = "element missing: " + factory;
        return false;
    }
    return true;
}

void GstDescriptionSource::on_availability_changed(std::function<void(bool)> cb) { cb_ = std::move(cb); }

// ------------------------------------------------------- test pattern

TestPatternSource::TestPatternSource(std::string pattern, int width, int height, int fps, std::string identity)
    : pattern_(std::move(pattern)), width_(width), height_(height), fps_(fps),
      identity_(identity.empty() ? "test:" + pattern_ : std::move(identity)) {}

SourceInfo TestPatternSource::describe() const {
    return SourceInfo{identity_, {SourceOutput{"src", TrackKind::Video, "video/x-raw,format=I420"}}};
}

GstBin* TestPatternSource::create_bin() {
    // An explicit capsfilter: a bare caps string at the end of a bin description does not parse.
    const std::string desc = "videotestsrc pattern=" + pattern_ + " is-live=true ! capsfilter caps=\"video/x-raw,format=I420,width=" +
                             std::to_string(width_) + ",height=" + std::to_string(height_) + ",framerate=" +
                             std::to_string(fps_) + "/1\"";
    GError* err = nullptr;
    GstElement* bin = gst_parse_bin_from_description(desc.c_str(), TRUE, &err);
    if (err) {
        glib::GErrorPtr guard(err);
        if (bin) [[maybe_unused]] auto released = glib::sink_element(bin);
        log::error("source", "test pattern failed", {{"error", err->message}});
        return nullptr;
    }
    return GST_BIN(bin);
}

// ----------------------------------------------------------- registry

// ------------------------------------------------------- v4l2

V4l2Source::V4l2Source(Params p, GMainContext* ctx) : p_(std::move(p)), ctx_(ctx) {}
V4l2Source::~V4l2Source() = default;

std::string V4l2Source::device_path() const {
    if (!p_.device.empty() && p_.device[0] == '/') return p_.device;
    return by_id_dir_ + "/" + p_.device;
}

std::string V4l2Source::description() const {
    // Always an explicit capsfilter: a bare caps string at the end of a description does not parse
    // (the same trap TestPatternSource documents).
    std::string d = "v4l2src device=" + device_path() + " ! ";
    std::string size;
    if (p_.width > 0) size += ",width=" + std::to_string(p_.width);
    if (p_.height > 0) size += ",height=" + std::to_string(p_.height);
    if (p_.fps > 0) size += ",framerate=" + std::to_string(p_.fps) + "/1";
    if (p_.format == "mjpeg") d += "capsfilter caps=\"image/jpeg" + size + "\" ! jpegdec";
    else if (p_.format == "yuyv") d += "capsfilter caps=\"video/x-raw,format=YUY2" + size + "\"";
    else d += "capsfilter caps=\"video/x-raw" + size + "\""; // auto: the device's preferred raw format in system memory
    // (never decodebin: a webcam's first choice through it was a DMABuf-only DRM format the software
    // conversion path cannot take; cameras that only offer MJPEG say so in --probe-source and take format = "mjpeg")
    return d;
}

SourceInfo V4l2Source::describe() const { return SourceInfo{"v4l2:" + p_.device, {SourceOutput{"src", TrackKind::Video, "video/x-raw"}}}; }

GstBin* V4l2Source::create_bin() {
    const std::string desc = description();
    GError* err = nullptr;
    GstElement* bin = gst_parse_bin_from_description(desc.c_str(), TRUE, &err);
    if (err) {
        glib::GErrorPtr guard(err);
        if (bin) [[maybe_unused]] auto released = glib::sink_element(bin);
        last_error_ = err->message;
        log::error("source", "v4l2 description failed", {{"description", desc}, {"error", last_error_}});
        return nullptr;
    }
    return GST_BIN(bin);
}

bool V4l2Source::available() const {
    if (!glib::GstObjectPtr<GstElementFactory>(gst_element_factory_find("v4l2src"))) {
        last_error_ = "element missing: v4l2src (gstreamer1.0-plugins-good)";
        return false;
    }
    if (p_.device.empty()) {
        last_error_ = "device is empty";
        return false;
    }
    const std::string path = device_path();
    if (!g_file_test(path.c_str(), G_FILE_TEST_EXISTS)) {
        last_error_ = "no such device: " + path + (p_.device[0] == '/' ? "" : " (not in " + by_id_dir_ + ")");
        return false;
    }
    return true;
}

void V4l2Source::on_availability_changed(std::function<void(bool)> cb) {
    cb_ = std::move(cb);
    last_available_ = available();
    watch();
}

void V4l2Source::watch() {
    // The monitors are created with the core context as thread-default, so their `changed` signal
    // is dispatched on the core loop (docs/23: source callbacks are marshaled by the core).
    if (ctx_) g_main_context_push_thread_default(ctx_);
    auto make = [this](const std::string& dir, glib::GObjectPtr<GFileMonitor>& mon, glib::SignalConnection& conn) {
        glib::GObjectPtr<GFile> f(g_file_new_for_path(dir.c_str()));
        GError* err = nullptr;
        mon.reset(g_file_monitor_directory(f.get(), G_FILE_MONITOR_NONE, nullptr, &err));
        if (err) {
            glib::GErrorPtr e(err);
            log::warn("source", "v4l2 hot-plug watch unavailable", {{"dir", dir}, {"error", err->message}});
            return;
        }
        conn = glib::SignalConnection(mon.get(), "changed", G_CALLBACK((+[](GFileMonitor*, GFile*, GFile*, GFileMonitorEvent ev, gpointer d) {
                                          if (ev != G_FILE_MONITOR_EVENT_CREATED && ev != G_FILE_MONITOR_EVENT_DELETED && ev != G_FILE_MONITOR_EVENT_MOVED_IN &&
                                              ev != G_FILE_MONITOR_EVENT_MOVED_OUT && ev != G_FILE_MONITOR_EVENT_CHANGES_DONE_HINT)
                                              return;
                                          auto* self = static_cast<V4l2Source*>(d);
                                          const bool now = self->available();
                                          if (now == self->last_available_) return;
                                          self->last_available_ = now;
                                          log::info("source", now ? "v4l2 device arrived" : "v4l2 device left", {{"device", self->device_path()}});
                                          try { // docs/23: nothing unwinds through a GLib signal emission
                                              if (self->cb_) self->cb_(now);
                                          } catch (const std::exception& e) {
                                              log::error("source", "hot-plug callback threw", {{"device", self->device_path()}, {"error", e.what()}});
                                          }
                                      })),
                                      this);
    };
    make(by_id_dir_, monitor_by_id_, changed_by_id_);
    // A plain /dev/videoN path: udev creates the by-id link and the node together; watching /dev
    // catches the node itself (the by-id dir may not exist on a machine without cameras).
    const std::string path = device_path();
    const auto slash = path.rfind('/');
    const std::string dir = slash == std::string::npos ? "/dev" : path.substr(0, slash);
    if (dir != by_id_dir_) make(dir, monitor_dev_, changed_dev_);
    if (ctx_) g_main_context_pop_thread_default(ctx_);
}

// ------------------------------------------------------- rtsp

RtspSource::RtspSource(Params p) : p_(std::move(p)) {}

std::string RtspSource::description(const std::string& output) const {
    const std::string url = output == "thumbnail" ? p_.thumbnail_url : p_.url;
    std::string d = "rtspsrc location=" + url + " latency=" + std::to_string(p_.latency_ms);
    if (p_.protocols == "tcp") d += " protocols=tcp";
    else if (p_.protocols == "udp") d += " protocols=udp";
    // Passthrough (docs/06): the camera's own H.264, depayloaded and parsed — never decoded, so the
    // core builds no encoder for the track. rtspsrc's pad is delayed either way.
    if (p_.passthrough) return d + " ! rtph264depay ! h264parse name=fjarr-rtsp-parse-" + output + " config-interval=-1";
    return d + " ! decodebin name=fjarr-rtsp-decode";
}

SourceInfo RtspSource::describe() const {
    const char* caps = p_.passthrough ? "video/x-h264" : "video/x-raw";
    SourceInfo info{"rtsp:" + p_.url, {SourceOutput{"src", TrackKind::Video, caps}}};
    if (p_.passthrough && !p_.thumbnail_url.empty()) info.outputs.push_back(SourceOutput{"thumbnail", TrackKind::Video, caps});
    return info;
}

void select_video_streams(GstBin* bin, std::vector<glib::SignalConnection>& out) {
    GstIterator* it = gst_bin_iterate_recurse(bin);
    GValue v = G_VALUE_INIT;
    while (gst_iterator_next(it, &v) == GST_ITERATOR_OK) {
        auto* e = static_cast<GstElement*>(g_value_get_object(&v));
        if (gst_element_get_factory(e) && std::string(GST_OBJECT_NAME(gst_element_get_factory(e))) == "rtspsrc")
            out.emplace_back(e, "select-stream", G_CALLBACK((+[](GstElement*, guint, GstCaps* caps, gpointer) -> gboolean {
                                 const GstStructure* st = caps ? gst_caps_get_structure(caps, 0) : nullptr;
                                 const char* media = st ? gst_structure_get_string(st, "media") : nullptr;
                                 return media == nullptr || g_strcmp0(media, "video") == 0;
                             })),
                             nullptr);
        g_value_unset(&v);
    }
    gst_iterator_free(it);
}

GstBin* RtspSource::build_stream(GstBin* into, const std::string& output, std::string* error) {
    // One chain per stream, ghosted as `src` / `src_<output>` (docs/09 multi-output sources).
    const std::string desc = description(output);
    const std::string pad = output == "src" ? "src" : "src_" + output;
    if (p_.passthrough) {
        // FALSE: with automatic ghosting the parser ghosts the depayloader's *sink* pad (unlinked at
        // parse time, because rtspsrc's pad is delayed), which counts as linked and makes rtspsrc's
        // delayed link fail — the same trap make_late_ghost_bin documents for decodebin (slice 6b).
        GError* err = nullptr;
        GstElement* chain = gst_parse_bin_from_description(desc.c_str(), FALSE, &err);
        if (err) {
            glib::GErrorPtr guard(err);
            if (chain) [[maybe_unused]] auto released = glib::sink_element(chain);
            if (error) *error = err->message;
            return nullptr;
        }
        gst_object_set_name(GST_OBJECT(chain), ("rtsp-" + output).c_str());
        gst_bin_add(into, chain);
        // The parser's src pad is the stream; ghost it by name, since nothing was ghosted for us.
        glib::GstElementPtr parse = glib::adopt_element(gst_bin_get_by_name(GST_BIN(chain), ("fjarr-rtsp-parse-" + output).c_str()));
        glib::GstPadPtr src = parse ? glib::adopt_pad(gst_element_get_static_pad(parse.get(), "src")) : nullptr;
        if (!src) {
            if (error) *error = "no h264parse src pad in the rtsp chain";
            return nullptr;
        }
        GstPad* inner = gst_ghost_pad_new("src", src.get()); // the chain bin's own pad…
        gst_pad_set_active(inner, TRUE);
        gst_element_add_pad(chain, inner);
        GstPad* ghost = gst_ghost_pad_new(pad.c_str(), inner); // …and the outer bin's
        gst_pad_set_active(ghost, TRUE);
        gst_element_add_pad(GST_ELEMENT(into), ghost);
        return into;
    }
    // Decoding: decodebin's pad arrives late, so the chain is its own late-ghost bin.
    GstBin* chain = make_late_ghost_bin(desc, "fjarr-rtsp-decode", output == "thumbnail" ? rtsp_pad_added_thumb_ : rtsp_pad_added_, error);
    if (!chain) return nullptr;
    gst_object_set_name(GST_OBJECT(chain), ("rtsp-" + output).c_str());
    gst_bin_add(into, GST_ELEMENT(chain));
    glib::GstPadPtr src = glib::adopt_pad(gst_element_get_static_pad(GST_ELEMENT(chain), "src"));
    GstPad* ghost = gst_ghost_pad_new(pad.c_str(), src.get());
    gst_pad_set_active(ghost, TRUE);
    gst_element_add_pad(GST_ELEMENT(into), ghost);
    return into;
}

GstBin* RtspSource::create_bin() {
    rtsp_select_streams_.clear();
    // Owned while it is built, handed over still floating: `create_bin()` returns a floating ref
    // and the caller sinks it (docs/09; the leaks gate caught the double reference in slice 6b).
    glib::GstElementPtr outer = glib::adopt_element(gst_bin_new("rtsp"));
    std::string err;
    for (const auto& o : describe().outputs) {
        if (!build_stream(GST_BIN(outer.get()), o.name, &err)) {
            log::error("source", "rtsp description failed", {{"description", description(o.name)}, {"error", err}});
            return nullptr;
        }
    }
    select_video_streams(GST_BIN(outer.get()), rtsp_select_streams_);
    return GST_BIN(outer.release());
}

// ------------------------------------------------------- late-ghost bins

GstBin* make_late_ghost_bin(const std::string& desc, const std::string& decodebin_name, glib::SignalConnection& pad_added, std::string* error) {
    GError* err = nullptr;
    GstElement* raw = gst_parse_bin_from_description(desc.c_str(), FALSE /* the parser would ghost decodebin's internals */, &err);
    if (err) {
        glib::GErrorPtr guard(err);
        if (raw) [[maybe_unused]] auto released = glib::sink_element(raw);
        if (error) *error = err->message;
        return nullptr;
    }
    GstBin* bin = GST_BIN(raw);
    GstPad* ghost = gst_ghost_pad_new_no_target("src", GST_PAD_SRC);
    gst_pad_set_active(ghost, TRUE);
    gst_element_add_pad(raw, ghost); // the bin owns the ghost pad; the handler below borrows it for the bin's lifetime
    glib::GstElementPtr dec = glib::adopt_element(gst_bin_get_by_name(bin, decodebin_name.c_str()));
    if (!dec) {
        if (error) *error = "no decodebin named " + decodebin_name + " in the description";
        [[maybe_unused]] auto released = glib::sink_element(raw);
        return nullptr;
    }
    pad_added = glib::SignalConnection(dec.get(), "pad-added", G_CALLBACK((+[](GstElement*, GstPad* pad, gpointer d) {
                                           auto* ghost = static_cast<GstPad*>(d);
                                           glib::GstCapsPtr caps(gst_pad_get_current_caps(pad));
                                           if (!caps) caps.reset(gst_pad_query_caps(pad, nullptr));
                                           const GstStructure* st = caps ? gst_caps_get_structure(caps.get(), 0) : nullptr;
                                           if (!st || !g_str_has_prefix(gst_structure_get_name(st), "video/")) return;
                                           glib::GstPadPtr current(gst_ghost_pad_get_target(GST_GHOST_PAD(ghost)));
                                           // The first video pad wins — unless the current target is a pad decodebin already
                                           // removed (a NULL→PLAYING cycle rebuilds them): then this is the new first pad.
                                           if (current) {
                                               glib::GstObjectPtr<GstObject> parent(gst_object_get_parent(GST_OBJECT(current.get())));
                                               if (parent && gst_pad_is_active(current.get())) return;
                                           }
                                           gst_ghost_pad_set_target(GST_GHOST_PAD(ghost), pad);
                                       })),
                                       ghost);
    return bin;
}

// ------------------------------------------------------- registry

namespace {
nlohmann::json int_prop() { return nlohmann::json{{"type", "integer"}, {"minimum", 0}}; }
} // namespace

SourceRegistry::SourceRegistry(GMainContext* ctx) : ctx_(ctx) {
    add(SourceType{"gst",
                   nlohmann::json{{"type", "object"},
                                  {"properties", {{"description", {{"type", "string"}}}, {"passthrough", {{"type", "boolean"}}}}},
                                  {"required", {"description"}}},
                   [](const nlohmann::json& p) -> std::unique_ptr<VideoSource> {
                       return std::make_unique<GstDescriptionSource>(p.at("description").get<std::string>(), "", p.value("passthrough", false));
                   }});
    add(SourceType{"test",
                   nlohmann::json{{"type", "object"},
                                  {"properties",
                                   {{"pattern", {{"type", "string"}}}, {"width", int_prop()}, {"height", int_prop()}, {"fps", int_prop()}}}},
                   [](const nlohmann::json& p) -> std::unique_ptr<VideoSource> {
                       return std::make_unique<TestPatternSource>(p.value("pattern", "smpte"), p.value("width", 1280),
                                                                  p.value("height", 720), p.value("fps", 30));
                   }});
    add(SourceType{"v4l2",
                   nlohmann::json{{"type", "object"},
                                  {"properties",
                                   {{"device", {{"type", "string"}, {"minLength", 1}}},
                                    {"format", {{"type", "string"}, {"enum", {"auto", "mjpeg", "yuyv"}}}},
                                    {"width", int_prop()},
                                    {"height", int_prop()},
                                    {"fps", int_prop()}}},
                                  {"required", {"device"}}},
                   [ctx](const nlohmann::json& p) -> std::unique_ptr<VideoSource> {
                       V4l2Source::Params vp;
                       vp.device = p.at("device").get<std::string>();
                       vp.format = p.value("format", "auto");
                       vp.width = p.value("width", 0);
                       vp.height = p.value("height", 0);
                       vp.fps = p.value("fps", 0);
                       return std::make_unique<V4l2Source>(vp, ctx);
                   }});
    add(SourceType{"rtsp",
                   nlohmann::json{{"type", "object"},
                                  {"properties",
                                   {{"url", {{"type", "string"}}},
                                    {"latency", int_prop()},
                                    {"protocols", {{"type", "string"}, {"enum", {"auto", "tcp", "udp"}}}},
                                    {"passthrough", {{"type", "boolean"}}},
                                    {"thumbnail_url", {{"type", "string"}}}}},
                                  {"required", {"url"}}},
                   [](const nlohmann::json& p) -> std::unique_ptr<VideoSource> {
                       RtspSource::Params rp;
                       rp.url = p.at("url").get<std::string>();
                       rp.latency_ms = p.value("latency", 200);
                       rp.protocols = p.value("protocols", "auto");
                       rp.passthrough = p.value("passthrough", false);
                       rp.thumbnail_url = p.value("thumbnail_url", "");
                       if (!rp.thumbnail_url.empty() && !rp.passthrough)
                           throw FjarrError("config", "rtsp: thumbnail_url is the passthrough track's lower tier; set passthrough = true (docs/06)");
                       return std::make_unique<RtspSource>(rp);
                   }});
}

void SourceRegistry::add(SourceType type) { types_[type.name] = std::move(type); }
bool SourceRegistry::has(const std::string& type) const { return types_.count(type) > 0; }
std::vector<std::string> SourceRegistry::types() const {
    std::vector<std::string> out;
    for (const auto& [k, _] : types_) out.push_back(k);
    return out;
}

std::unique_ptr<VideoSource> SourceRegistry::create(const nlohmann::json& config) const {
    if (config.is_string()) return std::make_unique<GstDescriptionSource>(config.get<std::string>());
    if (!config.is_object() || !config.contains("type") || !config["type"].is_string())
        throw FjarrError("config", "source must be a description string or {type = …}");
    const std::string type = config["type"];
    auto it = types_.find(type);
    if (it == types_.end()) {
        std::string known;
        for (const auto& [k, _] : types_) known += (known.empty() ? "" : ", ") + k;
        throw FjarrError("config", "unknown source type: " + type + " (registered: " + known + ")");
    }
    nlohmann::json params = config;
    params.erase("type");
    if (it->second.params_schema.is_object()) {
        try {
            nlohmann::json_schema::json_validator validator;
            validator.set_root_schema(it->second.params_schema);
            validator.validate(params);
        } catch (const std::exception& e) {
            throw FjarrError("config", "source type " + type + ": " + e.what());
        }
    }
    return it->second.create(params);
}

} // namespace fjarr::media

namespace fjarr {
std::unique_ptr<SourceFactory> builtin_source_factory() { return std::make_unique<media::SourceRegistry>(); }
} // namespace fjarr
