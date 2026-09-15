#include <fjarr/version.hpp>

#include <gst/gst.h>

namespace fjarr {

std::string version() { return "0.0.1"; }

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
    GstElement* pipeline = gst_parse_launch(
        "videotestsrc num-buffers=15 ! vapostproc ! vah264enc ! fakesink",
        &error);
    if (error != nullptr) {
        g_clear_error(&error);
        return false;
    }
    gst_element_set_state(pipeline, GST_STATE_PLAYING);
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* msg = gst_bus_timed_pop_filtered(
        bus, 15 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_EOS | GST_MESSAGE_ERROR));
    const bool ok =
        (msg != nullptr) && GST_MESSAGE_TYPE(msg) == GST_MESSAGE_EOS;
    if (msg != nullptr) {
        gst_message_unref(msg);
    }
    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);
    return ok;
}

} // namespace fjarr
