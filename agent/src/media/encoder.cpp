#include "encoder.hpp"

#include <algorithm>

#include <gst/allocators/gstdmabuf.h>
#include <gst/video/video.h>

#include <fjarr/errors.hpp>
#include <fjarr/version.hpp>

#include "core/glib/raii.hpp"
#include "core/log.hpp"

namespace fjarr::media {

EncoderChoice resolve_encoder(const std::string& policy) {
    const bool has_va = static_cast<bool>(glib::GstObjectPtr<GstElementFactory>(gst_element_factory_find("vah264enc")));
    if (policy == "software") {
        if (!glib::GstObjectPtr<GstElementFactory>(gst_element_factory_find("openh264enc"))) throw FjarrError("config", "media.encoder = software but openh264enc is missing");
        log::warn("media", "software encoder selected (openh264enc): CPU encode, no hardware fallback in play");
        return {EncoderKind::Software, "software"};
    }
    if (policy == "vaapi") {
        if (!has_va || !hardware_encode_available())
            throw FjarrError("config", "media.encoder = vaapi but the VA-API smoke pipeline fails (vah264enc); set media.encoder = \"software\" to trade CPU for portability, or fix /dev/dri");
        return {EncoderKind::VaApi, "vaapi"};
    }
    // auto
    if (has_va && hardware_encode_available()) return {EncoderKind::VaApi, "vaapi"};
    throw FjarrError("config", "media.encoder = auto and VA-API H.264 encode is unavailable on this machine: choose media.encoder = \"vaapi\" (fix /dev/dri, intel-media-va-driver) or \"software\" (openh264enc; CPU) — there is no silent fallback (docs/23)");
}

SourceMemory memory_of_caps(const std::string& caps) {
    if (caps.empty()) return SourceMemory::System;
    glib::GstCapsPtr c(gst_caps_from_string(caps.c_str()));
    if (!c) return SourceMemory::System;
    for (guint i = 0; i < gst_caps_get_size(c.get()); i++) {
        const GstCapsFeatures* f = gst_caps_get_features(c.get(), i);
        if (f && gst_caps_features_contains(f, GST_CAPS_FEATURE_MEMORY_DMABUF)) return SourceMemory::DmaBuf;
    }
    return SourceMemory::System;
}

namespace {
// The 8-bit RGB layouts mutter records a monitor in (docs/23#desktop-capture-memory).
bool mutter_rgb(std::uint32_t fourcc) {
    return fourcc == GST_MAKE_FOURCC('X', 'R', '2', '4') || fourcc == GST_MAKE_FOURCC('A', 'R', '2', '4') ||
           fourcc == GST_MAKE_FOURCC('X', 'B', '2', '4') || fourcc == GST_MAKE_FOURCC('A', 'B', '2', '4');
}
void add_drm_format(const GValue* v, std::vector<DmaBufFormat>& out) {
    if (!G_VALUE_HOLDS_STRING(v)) return;
    guint64 modifier = 0;
    const guint32 fourcc = gst_video_dma_drm_fourcc_from_string(g_value_get_string(v), &modifier);
    if (fourcc == 0 /* DRM_FORMAT_INVALID */ || modifier == 0x00ffffffffffffffULL /* DRM_FORMAT_MOD_INVALID */ || !mutter_rgb(fourcc)) return;
    const DmaBufFormat f{fourcc, modifier};
    if (std::find(out.begin(), out.end(), f) == out.end()) out.push_back(f);
}
} // namespace

std::vector<DmaBufFormat> dmabuf_imports_from_caps(const GstCaps* caps) {
    std::vector<DmaBufFormat> out;
    if (!caps) return out;
    for (guint i = 0; i < gst_caps_get_size(caps); i++) {
        const GstCapsFeatures* f = gst_caps_get_features(caps, i);
        if (!f || !gst_caps_features_contains(f, GST_CAPS_FEATURE_MEMORY_DMABUF)) continue;
        const GValue* v = gst_structure_get_value(gst_caps_get_structure(caps, i), "drm-format");
        if (!v) continue;
        if (GST_VALUE_HOLDS_LIST(v)) {
            for (guint k = 0; k < gst_value_list_get_size(v); k++) add_drm_format(gst_value_list_get_value(v, k), out);
        } else {
            add_drm_format(v, out);
        }
    }
    return out;
}

std::vector<DmaBufFormat> dmabuf_imports(EncoderKind kind, const std::function<GstCaps*(const char*)>& sink_caps) {
    // Only vaapi has a path from a DMA-BUF into the encoder without a copy (ADR-0041): software
    // encodes from system memory, and nvcodec/nvv4l2 get one of their own when they need it.
    if (kind != EncoderKind::VaApi) return {};
    auto from_template = [](const char* factory) -> GstCaps* {
        glib::GstElementPtr e = glib::adopt_element(gst_element_factory_make(factory, nullptr));
        if (!e) return nullptr;
        glib::GstPadPtr pad = glib::adopt_pad(gst_element_get_static_pad(e.get(), "sink"));
        return pad ? gst_pad_get_pad_template_caps(pad.get()) : nullptr;
    };
    glib::GstCapsPtr caps(sink_caps ? sink_caps("vapostproc") : from_template("vapostproc"));
    return dmabuf_imports_from_caps(caps.get());
}

std::string describe_imports(const std::vector<DmaBufFormat>& list) {
    std::string out;
    for (const auto& f : list) {
        glib::GStrPtr s(gst_video_dma_drm_fourcc_to_string(f.drm_fourcc, f.modifier));
        if (!out.empty()) out += ", ";
        out += s ? s.get() : "?";
    }
    return out.empty() ? "none" : out;
}

std::string encode_description(const EncoderChoice& enc, const TierProfile& tier, const std::string& prefix, SourceMemory memory) {
    // Explicit capsfilters everywhere: bare caps strings in a bin description do not parse reliably.
    auto q = [](const std::string& v) { return "\"" + v + "\""; };
    std::string desc;
    if (memory == SourceMemory::DmaBuf) {
        // A tier of the device-memory head (docs/23#encoders-and-tiers, ADR-0041): the head's one
        // vapostproc imported the DMA-BUF and converted it to NV12 in VA memory before the tee, so a
        // tier is the rate filter on that memory, vapostproc again only to scale the thumbnail, and
        // the encoder. No videoscale, no system-memory caps, no download.
        if (enc.kind != EncoderKind::VaApi)
            throw FjarrError("media", "a DMA-BUF source needs the vaapi encoder family; " + enc.name + " imports none (docs/23#desktop-capture-memory)");
        desc += "videorate name=" + q(prefix + "/rate") + " drop-only=true ! capsfilter name=" + q(prefix + "/ratecaps") + " caps=" +
                q("video/x-raw(memory:VAMemory),framerate=(fraction)[0/1," + std::to_string(tier.fps) + "/1]") + " ! ";
        if (tier.width > 0 && tier.height > 0)
            desc += "vapostproc name=" + q(prefix + "/convert") + " ! capsfilter name=" + q(prefix + "/scalecaps") + " caps=" +
                    q("video/x-raw(memory:VAMemory),format=NV12,width=" + std::to_string(tier.width) + ",height=" + std::to_string(tier.height)) + " ! ";
        desc += "vah264enc name=" + q(prefix + "/encoder") + " rate-control=cbr bitrate=" + std::to_string(tier.kbps) + " key-int-max=" +
                std::to_string(tier.gop_frames) + " b-frames=0 target-usage=6 ! capsfilter name=" + q(prefix + "/enccaps") + " caps=" +
                q("video/x-h264,profile=main") + " ! ";
    } else {
        // A tier's fps is a CEILING, not a target (docs/23: "the source size at <= 30 fps"). The filter
        // must therefore be a range: `videorate drop-only=true` can only drop frames, so demanding an
        // exact `fps/1` makes every source slower than the tier unlinkable — a 15 fps camera could not
        // stream at all (found by the slice-7a ladder test, which ran its source at 15 fps).
        desc += "videorate name=" + q(prefix + "/rate") + " drop-only=true ! capsfilter name=" + q(prefix + "/ratecaps") +
                " caps=" + q("video/x-raw,framerate=(fraction)[0/1," + std::to_string(tier.fps) + "/1]") + " ! ";
        if (tier.width > 0 && tier.height > 0) {
            desc += "videoscale name=" + q(prefix + "/scale") + " ! capsfilter name=" + q(prefix + "/scalecaps") + " caps=" +
                    q("video/x-raw,width=" + std::to_string(tier.width) + ",height=" + std::to_string(tier.height)) + " ! ";
        }
        if (enc.kind == EncoderKind::VaApi) {
            desc += "vapostproc name=" + q(prefix + "/convert") + " ! vah264enc name=" + q(prefix + "/encoder") +
                    " rate-control=cbr bitrate=" + std::to_string(tier.kbps) + " key-int-max=" + std::to_string(tier.gop_frames) +
                    " b-frames=0 target-usage=6 ! capsfilter name=" + q(prefix + "/enccaps") + " caps=" + q("video/x-h264,profile=main") + " ! ";
        } else {
            desc += "videoconvert name=" + q(prefix + "/convert") + " ! openh264enc name=" + q(prefix + "/encoder") +
                    " rate-control=bitrate bitrate=" + std::to_string(tier.kbps * 1000) + " gop-size=" + std::to_string(tier.gop_frames) +
                    " complexity=low usage-type=camera ! capsfilter name=" + q(prefix + "/enccaps") + " caps=" +
                    q("video/x-h264,profile=constrained-baseline") + " ! ";
        }
    }
    desc += "h264parse name=" + q(prefix + "/parser") + " config-interval=-1 ! capsfilter name=" + q(prefix + "/outcaps") + " caps=" +
            q("video/x-h264,stream-format=byte-stream,alignment=au");
    return desc;
}

GstElement* make_encode_bin(const EncoderChoice& enc, const TierProfile& tier, const std::string& prefix, SourceMemory memory) {
    const std::string desc = encode_description(enc, tier, prefix, memory);
    GError* err = nullptr;
    GstElement* bin = gst_parse_bin_from_description(desc.c_str(), TRUE, &err);
    if (err) {
        glib::GErrorPtr guard(err);
        if (bin) [[maybe_unused]] auto released = glib::sink_element(bin); // a recoverable parse error still returns a (floating) bin: release it
        throw FjarrError("media", std::string("encode branch failed: ") + err->message + " [" + desc + "]");
    }
    return bin;
}

GstElement* make_passthrough_bin(const std::string& prefix) {
    auto q = [](const std::string& v) { return "\"" + v + "\""; };
    const std::string desc = "h264parse name=" + q(prefix + "/parser") + " config-interval=-1 ! capsfilter name=" + q(prefix + "/outcaps") +
                             " caps=" + q("video/x-h264,stream-format=byte-stream,alignment=au");
    GError* err = nullptr;
    GstElement* bin = gst_parse_bin_from_description(desc.c_str(), TRUE, &err);
    if (err) {
        glib::GErrorPtr guard(err);
        if (bin) [[maybe_unused]] auto released = glib::sink_element(bin);
        throw FjarrError("media", std::string("passthrough branch failed: ") + err->message + " [" + desc + "]");
    }
    return bin;
}

GstElement* make_opus_bin(const std::string& prefix, bool music) {
    auto q = [](const std::string& v) { return "\"" + v + "\""; };
    // perfect-timestamp: output timestamps follow the samples, not when each input buffer arrived. A
    // desktop's PipeWire buffers arrive unevenly, and those timestamps reached the RTP clock, which a
    // browser hears as speeding up and slowing down (mini-PC, 2026-10-12). A gap over 100 ms (the
    // sink fell silent) still resyncs.
    // Music (a desktop's sound) is not speech: voice tuning at 32 kbps made it muffled and compressed
    // (mini-PC, 2026-10-12), so it gets the general-audio mode at 64 kbps, docs/16's top for a downlink.
    const std::string desc = "opusenc name=" + q(prefix + "/encoder") + (music ? " bitrate=64000 audio-type=generic" : " bitrate=32000 audio-type=voice") +
                             " frame-size=20 inband-fec=true perfect-timestamp=true tolerance=100000000";
    GError* err = nullptr;
    GstElement* bin = gst_parse_bin_from_description(desc.c_str(), TRUE, &err);
    if (err) {
        glib::GErrorPtr guard(err);
        if (bin) [[maybe_unused]] auto released = glib::sink_element(bin);
        throw FjarrError("media", std::string("opus branch failed: ") + err->message + " [" + desc + "]");
    }
    return bin;
}

} // namespace fjarr::media
