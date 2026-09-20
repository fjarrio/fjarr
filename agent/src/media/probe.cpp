#include <fjarr/probe.hpp>

#include <gst/gst.h>

#include "core/glib/raii.hpp"

namespace fjarr {

ProbeResult probe_source(const std::string& spec, std::chrono::milliseconds duration) {
    ProbeResult r;
    r.description = (spec == "test") ? "videotestsrc is-live=true" : spec;
    const std::string launch = r.description + " ! videoconvert ! fakesink name=probe-sink sync=true";
    GError* err = nullptr;
    glib::GstElementPtr pipe = glib::sink_element(gst_parse_launch(launch.c_str(), &err));
    if (err) {
        glib::GErrorPtr e(err);
        r.error = err->message;
        return r;
    }
    glib::GstElementPtr sink = glib::adopt_element(gst_bin_get_by_name(GST_BIN(pipe.get()), "probe-sink"));
    glib::GstPadPtr pad = glib::adopt_pad(gst_element_get_static_pad(sink.get(), "sink"));
    int frames = 0;
    glib::PadProbe counter(
        pad.get(), GST_PAD_PROBE_TYPE_BUFFER,
        [](GstPad*, GstPadProbeInfo*, gpointer d) -> GstPadProbeReturn {
            (*static_cast<int*>(d))++;
            return GST_PAD_PROBE_OK;
        },
        &frames);
    gst_element_set_state(pipe.get(), GST_STATE_PLAYING);
    glib::GstBusPtr bus(gst_element_get_bus(pipe.get()));
    const auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < duration) {
        glib::GstMessagePtr msg(gst_bus_timed_pop_filtered(bus.get(), 100 * GST_MSECOND, static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS)));
        if (!msg) continue;
        if (GST_MESSAGE_TYPE(msg.get()) == GST_MESSAGE_ERROR) {
            GError* e = nullptr;
            gchar* dbg = nullptr;
            gst_message_parse_error(msg.get(), &e, &dbg);
            glib::GErrorPtr ge(e);
            glib::GStrPtr gd(dbg);
            r.error = std::string(GST_OBJECT_NAME(GST_MESSAGE_SRC(msg.get()))) + ": " + (e ? e->message : "error") + (dbg ? std::string(" (") + dbg + ")" : "");
        }
        break;
    }
    glib::GstCapsPtr caps(gst_pad_get_current_caps(pad.get()));
    r.caps = glib::caps_to_string(caps.get());
    counter.remove();
    gst_element_set_state(pipe.get(), GST_STATE_NULL);
    r.frames = frames;
    r.fps = frames / (static_cast<double>(duration.count()) / 1000.0);
    r.ok = r.error.empty() && frames > 0;
    return r;
}

} // namespace fjarr
