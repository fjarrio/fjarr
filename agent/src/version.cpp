#include <fjarr/version.hpp>

#include <gst/gst.h>

#include "core/glib/raii.hpp"

namespace fjarr {

std::string version() { return "0.0.1"; }

std::string short_session_id(const std::string& session_id) { return session_id.size() > 8 ? session_id.substr(session_id.size() - 8) : session_id; }

std::string gstreamer_version() {
    guint major = 0, minor = 0, micro = 0, nano = 0;
    gst_version(&major, &minor, &micro, &nano);
    return std::to_string(major) + "." + std::to_string(minor) + "." +
           std::to_string(micro);
}

bool hardware_encode_available() {
    // Mirror of the doctor's smoke pipeline.
    // spec: docs/12-development-environment.md#doctor
    if (!gst_is_initialized()) {
        gst_init(nullptr, nullptr);
    }
    GError* error = nullptr;
    glib::GstElementPtr pipeline = glib::sink_element(gst_parse_launch("videotestsrc num-buffers=15 ! vapostproc ! vah264enc ! fakesink", &error));
    if (error != nullptr) {
        glib::GErrorPtr e(error);
        return false;
    }
    gst_element_set_state(pipeline.get(), GST_STATE_PLAYING);
    glib::GstBusPtr bus(gst_element_get_bus(pipeline.get()));
    glib::GstMessagePtr msg(gst_bus_timed_pop_filtered(bus.get(), 15 * GST_SECOND, static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR)));
    const bool ok = msg && GST_MESSAGE_TYPE(msg.get()) == GST_MESSAGE_EOS;
    gst_element_set_state(pipeline.get(), GST_STATE_NULL);
    return ok;
}

} // namespace fjarr
