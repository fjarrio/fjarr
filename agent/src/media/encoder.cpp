#include "encoder.hpp"

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

GstElement* make_encode_bin(const EncoderChoice& enc, const TierProfile& tier, const std::string& prefix) {
    // Explicit capsfilters everywhere: bare caps strings in a bin description do not parse reliably.
    auto q = [](const std::string& v) { return "\"" + v + "\""; };
    std::string desc;
    desc += "videorate name=" + q(prefix + "/rate") + " drop-only=true ! capsfilter name=" + q(prefix + "/ratecaps") +
            " caps=" + q("video/x-raw,framerate=" + std::to_string(tier.fps) + "/1") + " ! ";
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
    desc += "h264parse name=" + q(prefix + "/parser") + " config-interval=-1 ! capsfilter name=" + q(prefix + "/outcaps") + " caps=" +
            q("video/x-h264,stream-format=byte-stream,alignment=au");
    GError* err = nullptr;
    GstElement* bin = gst_parse_bin_from_description(desc.c_str(), TRUE, &err);
    if (err) {
        glib::GErrorPtr guard(err);
        if (bin) [[maybe_unused]] auto released = glib::sink_element(bin); // a recoverable parse error still returns a (floating) bin: release it
        throw FjarrError("media", std::string("encode branch failed: ") + err->message + " [" + desc + "]");
    }
    return bin;
}

} // namespace fjarr::media
