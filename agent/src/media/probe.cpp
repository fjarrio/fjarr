#include <fjarr/probe.hpp>

#include <gst/gst.h>

#include <functional>

#include <toml++/toml.hpp>

#include <fjarr/errors.hpp>

#include "core/glib/raii.hpp"
#include "media/sources.hpp"

namespace fjarr {

namespace {

/// `{type = "…", …}` → JSON through toml++ (the same table `fjarr.toml` holds).
bool parse_inline_table(const std::string& spec, nlohmann::json& out, std::string& error) {
    try {
        const toml::table t = toml::parse("source = " + spec);
        const toml::table* src = t["source"].as_table();
        if (!src) {
            error = "not a table";
            return false;
        }
        std::function<nlohmann::json(const toml::node&)> conv = [&](const toml::node& n) -> nlohmann::json {
            if (auto* v = n.as_table()) {
                nlohmann::json o = nlohmann::json::object();
                for (const auto& [k, val] : *v) o[std::string(k.str())] = conv(val);
                return o;
            }
            if (auto* v = n.as_array()) {
                nlohmann::json a = nlohmann::json::array();
                for (const auto& val : *v) a.push_back(conv(val));
                return a;
            }
            if (auto* v = n.as_string()) return v->get();
            if (auto* v = n.as_integer()) return v->get();
            if (auto* v = n.as_floating_point()) return v->get();
            if (auto* v = n.as_boolean()) return v->get();
            return nullptr;
        };
        out = conv(*src);
        return true;
    } catch (const toml::parse_error& e) {
        error = std::string(e.description());
        return false;
    }
}

std::string memory_of(const GstCaps* caps) {
    if (!caps || gst_caps_get_size(caps) == 0) return "";
    GstCapsFeatures* f = gst_caps_get_features(caps, 0);
    if (!f || gst_caps_features_is_any(f) || gst_caps_features_get_size(f) == 0) return "system";
    std::string out;
    for (guint i = 0; i < gst_caps_features_get_size(f); i++) {
        const char* n = gst_caps_features_get_nth(f, i);
        if (!n) continue;
        std::string name = n;
        if (name == "memory:SystemMemory") return "system";
        if (name.rfind("memory:", 0) == 0) name = name.substr(7);
        out += (out.empty() ? "" : "+") + name;
    }
    return out.empty() ? "system" : out;
}

} // namespace

ProbeResult probe_source(const std::string& spec, std::chrono::milliseconds duration) {
    const media::SourceRegistry builtins; // no core loop: hot-plug callbacks are not part of a probe
    return probe_source(builtins, spec, duration);
}

ProbeResult probe_source(const SourceFactory& sources, const std::string& spec, std::chrono::milliseconds duration) {
    ProbeResult r;
    glib::GstElementPtr pipe;
    std::unique_ptr<VideoSource> typed;
    std::string trimmed = spec;
    while (!trimmed.empty() && trimmed.front() == ' ') trimmed.erase(0, 1);
    if (!trimmed.empty() && trimmed.front() == '{') {
        nlohmann::json cfg;
        std::string perr;
        if (!parse_inline_table(trimmed, cfg, perr)) {
            r.error = "source table: " + perr;
            return r;
        }
        try {
            typed = sources.create(cfg);
        } catch (const FjarrError& e) {
            r.error = e.message();
            return r;
        }
        r.description = cfg.dump();
        if (!typed->available()) {
            r.error = "unavailable: " + typed->describe().identity;
            return r;
        }
        GstBin* bin = typed->create_bin();
        if (!bin) {
            r.error = "the source's bin failed to build";
            return r;
        }
        pipe = glib::sink_element(gst_pipeline_new("probe"));
        glib::GstElementPtr sbin = glib::sink_element(GST_ELEMENT(bin));
        glib::GstElementPtr conv = glib::make_element("videoconvert", "probe-convert");
        glib::GstElementPtr sink = glib::make_element("fakesink", "probe-sink");
        g_object_set(sink.get(), "sync", TRUE, nullptr);
        gst_bin_add_many(GST_BIN(pipe.get()), sbin.get(), conv.get(), sink.get(), nullptr);
        glib::GstPadPtr src = glib::adopt_pad(gst_element_get_static_pad(sbin.get(), "src"));
        glib::GstPadPtr csink = glib::adopt_pad(gst_element_get_static_pad(conv.get(), "sink"));
        if (!src || gst_pad_link(src.get(), csink.get()) != GST_PAD_LINK_OK || !gst_element_link(conv.get(), sink.get())) {
            r.error = "cannot link the source's src pad";
            return r;
        }
    } else {
        r.description = (spec == "test") ? "videotestsrc is-live=true" : spec;
        const std::string launch = r.description + " ! videoconvert ! fakesink name=probe-sink sync=true";
        GError* err = nullptr;
        pipe = glib::sink_element(gst_parse_launch(launch.c_str(), &err));
        if (err) {
            glib::GErrorPtr e(err);
            r.error = err->message;
            return r;
        }
    }
    glib::GstElementPtr sink = glib::adopt_element(gst_bin_get_by_name(GST_BIN(pipe.get()), "probe-sink"));
    glib::GstPadPtr pad = glib::adopt_pad(gst_element_get_static_pad(sink.get(), "sink"));
    // The memory type is read where the source hands frames over (its src pad), before videoconvert.
    glib::GstPadPtr source_pad;
    {
        glib::GstElementPtr conv = glib::adopt_element(gst_bin_get_by_name(GST_BIN(pipe.get()), "probe-convert"));
        if (!conv) {
            GstIterator* it = gst_bin_iterate_elements(GST_BIN(pipe.get()));
            GValue v = G_VALUE_INIT;
            while (gst_iterator_next(it, &v) == GST_ITERATOR_OK) {
                auto* e = static_cast<GstElement*>(g_value_get_object(&v));
                if (gst_element_get_factory(e) && std::string(GST_OBJECT_NAME(gst_element_get_factory(e))) == "videoconvert") conv = glib::ref_element(e);
                g_value_unset(&v);
            }
            gst_iterator_free(it);
        }
        if (conv) source_pad = glib::adopt_pad(gst_element_get_static_pad(conv.get(), "sink"));
    }
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
    if (source_pad) {
        glib::GstCapsPtr scaps(gst_pad_get_current_caps(source_pad.get()));
        r.memory = memory_of(scaps.get());
        if (scaps) r.caps = glib::caps_to_string(scaps.get()); // what the source produces, not videoconvert's output
    }
    counter.remove();
    gst_element_set_state(pipe.get(), GST_STATE_NULL);
    r.frames = frames;
    r.fps = frames / (static_cast<double>(duration.count()) / 1000.0);
    r.ok = r.error.empty() && frames > 0;
    return r;
}

} // namespace fjarr
