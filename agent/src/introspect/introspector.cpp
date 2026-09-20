#include "introspector.hpp"

#include <fstream>

#include <gst/webrtc/webrtc.h>

#include "core/glib/raii.hpp"
#include "core/log.hpp"
#include "core/protocol.hpp"
#include "media/pad_counter.hpp"

namespace fjarr::introspect {

namespace {

std::string state_name(GstElement* e) {
    GstState st = GST_STATE_NULL;
    gst_element_get_state(e, &st, nullptr, 0);
    switch (st) {
    case GST_STATE_READY: return "ready";
    case GST_STATE_PAUSED: return "paused";
    case GST_STATE_PLAYING: return "playing";
    default: return "null";
    }
}

std::string factory_name(GstElement* e) {
    GstElementFactory* f = gst_element_get_factory(e);
    return f ? GST_OBJECT_NAME(f) : (GST_IS_PIPELINE(e) ? "pipeline" : "bin");
}

nlohmann::json selected_properties(GstElement* e, const std::string& factory) {
    nlohmann::json props = nlohmann::json::object();
    auto get_bool = [&](const char* n) {
        gboolean v = FALSE;
        g_object_get(e, n, &v, nullptr);
        props[n] = v ? true : false;
    };
    auto get_uint = [&](const char* n) {
        guint v = 0;
        g_object_get(e, n, &v, nullptr);
        props[n] = v;
    };
    auto get_u64 = [&](const char* n) {
        guint64 v = 0;
        g_object_get(e, n, &v, nullptr);
        props[n] = v;
    };
    if (factory == "valve") get_bool("drop");
    else if (factory == "queue") {
        get_uint("current-level-buffers");
        get_uint("current-level-bytes");
        get_u64("current-level-time");
    } else if (factory == "vah264enc" || factory == "openh264enc") {
        get_uint("bitrate");
    } else if (factory == "webrtcbin") {
        for (const char* n : {"connection-state", "ice-connection-state", "ice-gathering-state", "signaling-state"})
            props[n] = glib::enum_prop_nick(e, n);
    } else if (factory == "appsrc") get_bool("is-live");
    else if (factory == "rtph264pay") {
        get_uint("pt");
        get_uint("ssrc");
    } else if (factory == "videotestsrc") props["pattern"] = glib::enum_prop_nick(e, "pattern");
    return props;
}

void walk_element(GstElement* e, nlohmann::json& elements, nlohmann::json& links) {
    nlohmann::json el;
    const std::string name = glib::element_name(e);
    const std::string factory = factory_name(e);
    el["name"] = name;
    el["factory"] = factory;
    el["state"] = state_name(e);
    el["properties"] = selected_properties(e, factory);
    nlohmann::json pads = nlohmann::json::array();
    GstIterator* it = gst_element_iterate_pads(e);
    GValue item = G_VALUE_INIT;
    while (gst_iterator_next(it, &item) == GST_ITERATOR_OK) {
        auto* pad = static_cast<GstPad*>(g_value_get_object(&item));
        nlohmann::json p;
        p["name"] = glib::pad_name(pad);
        p["direction"] = GST_PAD_DIRECTION(pad) == GST_PAD_SRC ? "src" : "sink";
        glib::GstCapsPtr caps(gst_pad_get_current_caps(pad));
        if (caps) p["caps"] = glib::caps_to_string(caps.get());
        else p["caps"] = nullptr;
        if (const auto* c = media::PadCounter::lookup(pad)) {
            p["buffers"] = c->buffers.load();
            p["bytes"] = c->bytes.load();
            const auto pts = c->last_pts_ns.load();
            p["last_pts_ms"] = pts < 0 ? nlohmann::json(nullptr) : nlohmann::json(static_cast<double>(pts) / 1e6);
        }
        if (GST_PAD_DIRECTION(pad) == GST_PAD_SRC) {
            glib::GstPadPtr peer = glib::adopt_pad(gst_pad_get_peer(pad));
            if (peer) {
                glib::GstElementPtr parent = glib::adopt_element(GST_ELEMENT(gst_pad_get_parent(peer.get())));
                if (parent) {
                    links.push_back({{"from", name + ":" + glib::pad_name(pad)}, {"to", glib::element_name(parent.get()) + ":" + glib::pad_name(peer.get())}});
                }
            }
        }
        pads.push_back(std::move(p));
        g_value_reset(&item);
    }
    g_value_unset(&item);
    gst_iterator_free(it);
    el["pads"] = std::move(pads);
    if (GST_IS_BIN(e)) {
        nlohmann::json children = nlohmann::json::array();
        GstIterator* ci = gst_bin_iterate_sorted(GST_BIN(e));
        GValue citem = G_VALUE_INIT;
        std::vector<glib::GstElementPtr> kids;
        while (gst_iterator_next(ci, &citem) == GST_ITERATOR_OK) {
            kids.push_back(glib::ref_element(static_cast<GstElement*>(g_value_get_object(&citem))));
            g_value_reset(&citem);
        }
        g_value_unset(&citem);
        gst_iterator_free(ci);
        // Sorted iteration is sink-first; present source-first.
        for (auto rit = kids.rbegin(); rit != kids.rend(); ++rit) walk_element(rit->get(), children, links);
        el["children"] = std::move(children);
    }
    elements.push_back(std::move(el));
}

void flatten(const nlohmann::json& elements, std::vector<const nlohmann::json*>& out) {
    for (const auto& e : elements) {
        if (e.contains("children")) flatten(e["children"], out);
        else out.push_back(&e);
    }
}

} // namespace

Snapshot walk(GstBin* bin, SnapshotMeta meta) {
    Snapshot s;
    meta.ts = protocol::now_ms();
    meta.state = state_name(GST_ELEMENT(bin));
    s.meta = meta;
    nlohmann::json elements = nlohmann::json::array();
    nlohmann::json links = nlohmann::json::array();
    // Walk the children (the pipeline itself is the snapshot).
    GstIterator* ci = gst_bin_iterate_sorted(bin);
    GValue citem = G_VALUE_INIT;
    std::vector<glib::GstElementPtr> kids;
    while (gst_iterator_next(ci, &citem) == GST_ITERATOR_OK) {
        kids.push_back(glib::ref_element(static_cast<GstElement*>(g_value_get_object(&citem))));
        g_value_reset(&citem);
    }
    g_value_unset(&citem);
    gst_iterator_free(ci);
    for (auto rit = kids.rbegin(); rit != kids.rend(); ++rit) walk_element(rit->get(), elements, links);
    nlohmann::json json{{"v", 1},
                            {"pipeline_id", meta.pipeline_id},
                            {"kind", meta.kind},
                            {"generation", meta.generation},
                            {"seq", meta.seq},
                            {"ts", meta.ts},
                            {"trigger", meta.trigger},
                            {"state", meta.state},
                            {"elements", elements},
                            {"links", links}};
    if (!meta.session_id.empty()) json["session_id"] = meta.session_id;
    if (!meta.robot_id.empty()) json["robot_id"] = meta.robot_id;
    glib::GStrPtr dot(gst_debug_bin_to_dot_data(bin, GST_DEBUG_GRAPH_SHOW_ALL));
    s.dot = "// fjarr snapshot pipeline=" + meta.pipeline_id + " seq=" + std::to_string(meta.seq) + " trigger=" + meta.trigger +
            " ts=" + std::to_string(meta.ts) + (meta.session_id.empty() ? "" : " session=" + meta.session_id) + "\n" +
            (dot ? dot.get() : "");
    s.txt = summarize(json);
    s.json = json.dump();
    return s;
}

std::string summarize(const nlohmann::json& snap) {
    std::vector<const nlohmann::json*> leaves;
    static const nlohmann::json no_elements = nlohmann::json::array();
    const nlohmann::json& elements = snap.contains("elements") && snap["elements"].is_array() ? snap["elements"] : no_elements;
    flatten(elements, leaves); // pointers into `snap`, which outlives this function
    std::string out = snap.value("pipeline_id", "") + " (" + snap.value("state", "") + ", seq " + std::to_string(snap.value("seq", 0)) +
                      ", " + snap.value("trigger", "") + "): ";
    bool first = true;
    for (const auto* e : leaves) {
        if (!first) out += " → ";
        first = false;
        const std::string name = e->value("name", "");
        const auto slash = name.rfind('/');
        out += slash == std::string::npos ? name : name.substr(slash + 1);
        out += "(" + e->value("state", "") + ")";
        const auto& props = (*e)["properties"];
        if (props.is_object() && !props.empty()) {
            out += "[";
            bool fp = true;
            for (auto it = props.begin(); it != props.end(); ++it) {
                if (!fp) out += " ";
                fp = false;
                out += it.key() + "=" + (it.value().is_string() ? it.value().get<std::string>() : it.value().dump());
            }
            out += "]";
        }
    }
    return out;
}

SnapshotStore::SnapshotStore(std::size_t history, std::string dot_dir, Scheduler schedule)
    : history_(history), dot_dir_(std::move(dot_dir)), schedule_(std::move(schedule)) {}

void SnapshotStore::take(GstBin* bin, SnapshotMeta meta, bool force) {
    Ring& ring = rings_[meta.pipeline_id];
    const auto now = std::chrono::steady_clock::now();
    const auto since = now - ring.last;
    if (!force && schedule_ && ring.last != std::chrono::steady_clock::time_point{} && since < std::chrono::milliseconds(250)) {
        // Defer to the end of the window; a later trigger in the same window replaces the pending one.
        if (ring.pending_bin.get() != bin) ring.pending_bin.reset(glib::ref_object(bin));
        ring.pending_meta = meta;
        if (!ring.pending) {
            ring.pending = true;
            const std::string id = meta.pipeline_id;
            ring.pending_timer = schedule_(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::milliseconds(250) - since) + std::chrono::milliseconds(1), [this, id] {
                auto it = rings_.find(id);
                if (it == rings_.end() || !it->second.pending) return;
                Ring& r = it->second;
                r.pending = false;
                glib::GstObjectPtr<GstBin> b = std::move(r.pending_bin);
                if (!b) return;
                take_now(r, b.get(), r.pending_meta);
            });
        }
        return;
    }
    ring.pending_bin.reset();
    ring.pending = false;
    take_now(ring, bin, std::move(meta));
}

void SnapshotStore::take_now(Ring& ring, GstBin* bin, SnapshotMeta meta) {
    ring.last = std::chrono::steady_clock::now();
    // A retired ring stays retired: the session's deferred "closing" snapshot lands after the
    // session-ended event retired it, and un-retiring here kept every closed session's ring
    // for the life of the process (found by fjarr-opsim's soak).
    meta.seq = ++ring.seq;
    auto snap = std::make_shared<Snapshot>(walk(bin, meta));
    ring.items.push_back(snap);
    while (ring.items.size() > history_) ring.items.pop_front();
    // DOT for the most recent DOT_KEPT snapshots only; JSON and the summary for the whole history.
    for (std::size_t i = 0; i + DOT_KEPT < ring.items.size(); i++) {
        if (ring.items[i]->dot.empty()) continue;
        auto trimmed = std::make_shared<Snapshot>(*ring.items[i]);
        trimmed->dot.clear();
        trimmed->dot.shrink_to_fit();
        ring.items[i] = trimmed;
    }
    if (!dot_dir_.empty()) {
        std::string id = meta.pipeline_id;
        for (auto& c : id)
            if (c == '/' || c == ':') c = '_';
        std::ofstream f(dot_dir_ + "/" + id + "-" + std::to_string(meta.seq) + "-" + meta.trigger + ".dot");
        f << snap->dot;
    }
    // Listeners may add or remove themselves while notified: iterate a copy of the ids.
    std::vector<unsigned> ids;
    for (const auto& [id, _] : listeners_) ids.push_back(id);
    for (unsigned id : ids)
        if (auto it = listeners_.find(id); it != listeners_.end()) it->second(*snap);
}

unsigned SnapshotStore::add_listener(std::function<void(const Snapshot&)> fn) {
    const unsigned id = next_listener_++;
    listeners_[id] = std::move(fn);
    return id;
}

void SnapshotStore::remove_listener(unsigned id) { listeners_.erase(id); }

nlohmann::json meta_json(const SnapshotMeta& m) {
    return nlohmann::json{{"pipeline_id", m.pipeline_id}, {"kind", m.kind}, {"session_id", m.session_id}, {"seq", m.seq},
                          {"trigger", m.trigger}, {"state", m.state}, {"ts", m.ts}, {"generation", m.generation}};
}

void SnapshotStore::retire(const std::string& pipeline_id) {
    auto it = rings_.find(pipeline_id);
    if (it == rings_.end()) return;
    it->second.retired = true;
    it->second.retired_at = std::chrono::steady_clock::now();
    while (it->second.items.size() > 8) it->second.items.pop_front();
    // Retention is bounded in total, not only per ring: 200 sessions in ten minutes kept 70 MB
    // (the soak's RSS budget caught it). At most MAX_RETIRED closed rings, oldest evicted first,
    // and a retired ring keeps its DOT only for its last snapshot (JSON has everything the viewer
    // and the tests read; the DOT is the large one).
    for (std::size_t i = 0; i + 1 < it->second.items.size(); i++) {
        auto trimmed = std::make_shared<Snapshot>(*it->second.items[i]);
        trimmed->dot.clear();
        trimmed->dot.shrink_to_fit();
        it->second.items[i] = trimmed;
    }
    std::size_t retired = 0;
    for (const auto& [_, r] : rings_) retired += r.retired ? 1 : 0;
    while (retired > MAX_RETIRED) {
        auto oldest = rings_.end();
        for (auto r = rings_.begin(); r != rings_.end(); ++r)
            if (r->second.retired && (oldest == rings_.end() || r->second.retired_at < oldest->second.retired_at)) oldest = r;
        if (oldest == rings_.end()) break;
        rings_.erase(oldest);
        retired--;
    }
    it->second.pending_timer.cancel(); // a deferred snapshot of a retired ring still lands (below) but never revives it
    if (it->second.pending) {
        it->second.pending = false;
        glib::GstObjectPtr<GstBin> b = std::move(it->second.pending_bin);
        if (b) take_now(it->second, b.get(), it->second.pending_meta); // the "closing" snapshot, now rather than later
    }
}

void SnapshotStore::expire_retired() {
    const auto now = std::chrono::steady_clock::now();
    for (auto it = rings_.begin(); it != rings_.end();) {
        if (it->second.retired && now - it->second.retired_at > std::chrono::minutes(10)) it = rings_.erase(it);
        else ++it;
    }
}

std::vector<SnapshotMeta> SnapshotStore::list() const {
    std::vector<SnapshotMeta> out;
    for (const auto& [_, r] : rings_)
        if (!r.items.empty()) out.push_back(r.items.back()->meta);
    return out;
}

std::shared_ptr<const Snapshot> SnapshotStore::latest(const std::string& pipeline_id) const {
    auto it = rings_.find(pipeline_id);
    if (it == rings_.end() || it->second.items.empty()) return nullptr;
    return it->second.items.back();
}

std::shared_ptr<const Snapshot> SnapshotStore::at(const std::string& pipeline_id, unsigned seq) const {
    auto it = rings_.find(pipeline_id);
    if (it == rings_.end()) return nullptr;
    for (const auto& s : it->second.items)
        if (s->meta.seq == seq) return s;
    return nullptr;
}

std::vector<SnapshotMeta> SnapshotStore::history(const std::string& pipeline_id) const {
    std::vector<SnapshotMeta> out;
    auto it = rings_.find(pipeline_id);
    if (it == rings_.end()) return out;
    for (const auto& s : it->second.items) out.push_back(s->meta);
    return out;
}

} // namespace fjarr::introspect
