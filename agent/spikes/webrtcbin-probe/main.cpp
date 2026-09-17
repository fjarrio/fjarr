// webrtcbin-probe: single-process loopback of two webrtcbin instances
// (A = "agent"/offerer, B = "browser"/answerer) driven entirely from one
// GMainLoop. Answers the questions Q1..Q6 listed in README.md and prints a
// summary table. Standalone spike — not part of libfjarr.

#include <gst/gst.h>
extern bool g_reuse_pads;
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace {

// ---------------------------------------------------------------- RAII ----
template <class T> struct GObjDel { void operator()(T* p) const { if (p) g_object_unref(p); } };
template <class T> using GObj = std::unique_ptr<T, GObjDel<T>>;
struct SdpDel { void operator()(GstWebRTCSessionDescription* d) const { if (d) gst_webrtc_session_description_free(d); } };
using Sdp = std::unique_ptr<GstWebRTCSessionDescription, SdpDel>;
struct GStrDel { void operator()(gchar* s) const { g_free(s); } };
using GStr = std::unique_ptr<gchar, GStrDel>;
using Reply = std::shared_ptr<GstStructure>;  // copy of a GstPromise reply

// ------------------------------------------------------- logging/results ----
const auto t0 = std::chrono::steady_clock::now();
double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); }
void logf(const char* fmt, ...) {
  va_list ap; va_start(ap, fmt);
  std::printf("[%7.3f] ", now_s()); std::vprintf(fmt, ap); std::printf("\n"); std::fflush(stdout);
  va_end(ap);
}
struct Result { std::string id, verdict, note; };
std::vector<Result> results;
void record(const std::string& id, const char* verdict, const std::string& note) {
  results.push_back({id, verdict, note});
  logf("RESULT %-8s %-7s %s", id.c_str(), verdict, note.c_str());
}
void record(const std::string& id, bool pass, const std::string& note) { record(id, pass ? "PASS" : "FAIL", note); }

std::string enum_nick(GType t, gint v) {
  auto* k = static_cast<GEnumClass*>(g_type_class_ref(t));
  GEnumValue* e = g_enum_get_value(k, v);
  std::string s = e ? e->value_nick : "?";
  g_type_class_unref(k);
  return s;
}
gint enum_prop(gpointer obj, const char* name) { gint v = -1; g_object_get(obj, name, &v, nullptr); return v; }
guint uint_prop(gpointer obj, const char* name) { guint v = 0; g_object_get(obj, name, &v, nullptr); return v; }
std::string str_prop(gpointer obj, const char* name) {
  if (!g_object_class_find_property(G_OBJECT_GET_CLASS(obj), name)) return "<no-such-property>";
  gchar* s = nullptr; g_object_get(obj, name, &s, nullptr);
  GStr guard(s);
  return s ? s : "(null)";
}
std::string list_props(GType type) {
  auto* k = static_cast<GObjectClass*>(g_type_class_ref(type));
  guint n = 0; GParamSpec** ps = g_object_class_list_properties(k, &n);
  std::string out;
  for (guint i = 0; i < n; i++) {
    out += ps[i]->name; out += "("; out += g_type_name(ps[i]->value_type);
    out += (ps[i]->flags & G_PARAM_WRITABLE) ? ",rw) " : ",ro) ";
  }
  g_free(ps); g_type_class_unref(k);
  return out;
}
std::string list_signals(GType type) {
  guint n = 0; guint* ids = g_signal_list_ids(type, &n);
  std::string out;
  for (guint i = 0; i < n; i++) { out += g_signal_name(ids[i]); out += " "; }
  g_free(ids);
  return out;
}

// ------------------------------------------------- main-loop marshaling ----
GMainLoop* loop = nullptr;
void post(std::function<void()> fn) {
  auto* f = new std::function<void()>(std::move(fn));
  g_idle_add_full(G_PRIORITY_DEFAULT,
      [](gpointer d) -> gboolean { (*static_cast<std::function<void()>*>(d))(); return G_SOURCE_REMOVE; },
      f, [](gpointer d) { delete static_cast<std::function<void()>*>(d); });
}
// Poll `pred` every 20 ms on the main loop; done(true) when it holds, done(false) on timeout.
void wait_until(std::function<bool()> pred, int timeout_ms, std::function<void(bool)> done) {
  struct W { std::function<bool()> pred; std::function<void(bool)> done; int left; };
  auto* w = new W{std::move(pred), std::move(done), timeout_ms};
  g_timeout_add_full(G_PRIORITY_DEFAULT, 20, [](gpointer d) -> gboolean {
      auto* w = static_cast<W*>(d);
      bool ok = w->pred();
      w->left -= 20;
      if (!ok && w->left > 0) return G_SOURCE_CONTINUE;
      auto done = std::move(w->done); delete w; done(ok);
      return G_SOURCE_REMOVE; }, w, nullptr);
}
void after(int ms, std::function<void()> fn) { wait_until([] { return false; }, ms, [fn](bool) { fn(); }); }

// Emit an action signal taking (arg0, GstPromise*); the reply is copied and
// delivered on the main loop. The promise ref is dropped in the change func
// (same convention as the upstream webrtc-sendrecv example).
void emit_async(GstElement* w, const char* signal, gpointer arg0, std::function<void(Reply)> then) {
  auto* cb = new std::function<void(Reply)>(std::move(then));
  GstPromise* p = gst_promise_new_with_change_func([](GstPromise* pr, gpointer d) {
      auto* cb = static_cast<std::function<void(Reply)>*>(d);
      const GstStructure* r = gst_promise_get_reply(pr);
      Reply copy(r ? gst_structure_copy(r) : nullptr, [](GstStructure* s) { if (s) gst_structure_free(s); });
      gst_promise_unref(pr);
      post([cb, copy] { (*cb)(copy); delete cb; });
  }, cb, nullptr);
  g_signal_emit_by_name(w, signal, arg0, p);
}
std::string reply_error(const Reply& r) {
  if (!r) return "<null reply>";
  GError* e = nullptr;
  if (gst_structure_get(r.get(), "error", G_TYPE_ERROR, &e, nullptr) && e) { std::string m = e->message; g_error_free(e); return m; }
  return "";
}

// ------------------------------------------------------------- SDP utils ----
std::string sdp_text(const GstWebRTCSessionDescription* d) { GStr s(gst_sdp_message_as_text(d->sdp)); return s.get(); }
std::string media_attr(const GstWebRTCSessionDescription* d, guint i, const char* key) {
  if (!d || i >= gst_sdp_message_medias_len(d->sdp)) return "<no-media>";
  const gchar* v = gst_sdp_media_get_attribute_val(gst_sdp_message_get_media(d->sdp, i), key);
  if (!v) v = gst_sdp_message_get_attribute_val(d->sdp, key);
  return v ? v : "<none>";
}
std::string media_dir(const GstWebRTCSessionDescription* d, guint i) {
  for (const char* k : {"sendrecv", "sendonly", "recvonly", "inactive"})
    if (gst_sdp_media_get_attribute_val(gst_sdp_message_get_media(d->sdp, i), k)) return k;
  return "<none>";
}
std::string sdp_summary(const GstWebRTCSessionDescription* d) {
  std::string out;
  for (guint i = 0; i < gst_sdp_message_medias_len(d->sdp); i++) {
    const GstSDPMedia* m = gst_sdp_message_get_media(d->sdp, i);
    out += " m=" + std::to_string(i) + ":" + gst_sdp_media_get_media(m) + " mid=" + media_attr(d, i, "mid") +
           " " + media_dir(d, i) + " ufrag=" + media_attr(d, i, "ice-ufrag") + " port=" + std::to_string(gst_sdp_media_get_port(m));
  }
  return out;
}

// ------------------------------------------------------------------ Peer ----
struct RxTrack {
  std::string pad, mid; guint mline{};
  std::atomic<uint64_t> count{0};
  std::atomic<int64_t> last_us{0}, max_gap_us{0};
  std::atomic<bool> watch{false};
};
struct TxTrack {
  GstPad* sinkpad{};                  // webrtcbin sink_%u (owned by webrtcbin)
  GstElement* valve{};                // pay ! valve ! webrtcbin (owned by the pipeline)
  std::string caps;                   // fixed RTP caps as seen on the payloader src pad
  std::atomic<bool> caps_ready{false};
};

struct Peer {
  std::string name;
  GObj<GstElement> pipeline;
  GstElement* webrtc{};
  Peer* remote{};
  bool remote_desc_set = false;
  std::vector<std::pair<guint, std::string>> pending_cands;
  std::atomic<int> negotiation_needed{0}, new_transceiver{0}, pads_added{0}, dc_open{0}, ice_state_changes{0};
  std::atomic<GstWebRTCDataChannel*> dc{nullptr};   // B: received via on-data-channel; A: created
  std::atomic<uint64_t> dc_bytes{0};
  std::vector<std::string> dc_messages;             // main loop only
  std::mutex mu;
  std::map<std::string, std::unique_ptr<RxTrack>> rx;  // by src pad name

  Peer(std::string n, GstWebRTCBundlePolicy bp) : name(std::move(n)) {
    pipeline.reset(gst_pipeline_new(("pipe-" + name).c_str()));
    webrtc = gst_element_factory_make("webrtcbin", ("webrtc-" + name).c_str());
    g_object_set(webrtc, "bundle-policy", bp, nullptr);   // no stun-server: host candidates only
    // 1.26+: keep source pads across renegotiation instead of sending EOS on
    // an inactive transceiver (the 1.24 answerer stall — ADR-0022).
    if (g_reuse_pads && g_object_class_find_property(G_OBJECT_GET_CLASS(webrtc), "reuse-source-pads"))
      g_object_set(webrtc, "reuse-source-pads", TRUE, nullptr);
    gst_bin_add(GST_BIN(pipeline.get()), webrtc);
    g_signal_connect(webrtc, "on-negotiation-needed", G_CALLBACK(+[](GstElement*, gpointer d) {
        auto* p = static_cast<Peer*>(d); int n = ++p->negotiation_needed;
        logf("%s: on-negotiation-needed #%d", p->name.c_str(), n); }), this);
    g_signal_connect(webrtc, "on-ice-candidate", G_CALLBACK(+[](GstElement*, guint mline, gchar* c, gpointer d) {
        auto* p = static_cast<Peer*>(d); std::string cand(c);
        post([p, mline, cand] { p->remote->add_candidate(mline, cand); }); }), this);
    g_signal_connect(webrtc, "on-new-transceiver", G_CALLBACK(+[](GstElement*, GstWebRTCRTPTransceiver* t, gpointer d) {
        auto* p = static_cast<Peer*>(d); ++p->new_transceiver;
        logf("%s: on-new-transceiver mline=%u mid=%s dir=%s", p->name.c_str(), uint_prop(t, "mlineindex"), str_prop(t, "mid").c_str(),
             enum_nick(GST_TYPE_WEBRTC_RTP_TRANSCEIVER_DIRECTION, enum_prop(t, "direction")).c_str()); }), this);
    g_signal_connect(webrtc, "pad-added", G_CALLBACK(+[](GstElement*, GstPad* pad, gpointer d) {
        static_cast<Peer*>(d)->on_pad_added(pad); }), this);
    g_signal_connect(webrtc, "on-data-channel", G_CALLBACK(+[](GstElement*, GstWebRTCDataChannel* ch, gpointer d) {
        static_cast<Peer*>(d)->on_data_channel(ch); }), this);
    for (const char* prop : {"ice-connection-state", "connection-state", "ice-gathering-state", "signaling-state"})
      g_signal_connect(webrtc, (std::string("notify::") + prop).c_str(), G_CALLBACK(+[](GObject* o, GParamSpec* ps, gpointer d) {
          auto* p = static_cast<Peer*>(d);
          if (!std::strcmp(ps->name, "ice-connection-state")) ++p->ice_state_changes;
          logf("%s: %s -> %s", p->name.c_str(), ps->name,
               enum_nick(G_PARAM_SPEC_VALUE_TYPE(ps), enum_prop(o, ps->name)).c_str()); }), this);
    GstBus* bus = gst_pipeline_get_bus(GST_PIPELINE(pipeline.get()));
    gst_bus_add_watch(bus, [](GstBus*, GstMessage* m, gpointer d) -> gboolean {
        auto* p = static_cast<Peer*>(d);
        if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR || GST_MESSAGE_TYPE(m) == GST_MESSAGE_WARNING) {
          GError* e = nullptr; gchar* dbg = nullptr;
          if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) gst_message_parse_error(m, &e, &dbg); else gst_message_parse_warning(m, &e, &dbg);
          logf("%s: BUS %s from %s: %s (%s)", p->name.c_str(), GST_MESSAGE_TYPE_NAME(m), GST_OBJECT_NAME(m->src), e->message, dbg ? dbg : "");
          g_error_free(e); g_free(dbg);
        }
        return TRUE; }, this);
    gst_object_unref(bus);
  }
  ~Peer() { gst_element_set_state(pipeline.get(), GST_STATE_NULL); }

  void add_candidate(guint mline, const std::string& c) {
    if (!remote_desc_set) { pending_cands.emplace_back(mline, c); return; }
    g_signal_emit_by_name(webrtc, "add-ice-candidate", mline, c.c_str());
  }
  void flush_candidates() {
    remote_desc_set = true;
    for (auto& [m, c] : pending_cands) g_signal_emit_by_name(webrtc, "add-ice-candidate", m, c.c_str());
    if (!pending_cands.empty()) logf("%s: flushed %zu queued remote candidates", name.c_str(), pending_cands.size());
    pending_cands.clear();
  }
  // Streaming-thread callback: attach queue ! fakesink and count buffers.
  void on_pad_added(GstPad* pad) {
    if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC) return;
    GStr pname(gst_pad_get_name(pad));
    GstCaps* caps = gst_pad_get_current_caps(pad);
    GStr cs(caps ? gst_caps_to_string(caps) : g_strdup("<none>"));
    if (caps) gst_caps_unref(caps);
    // Map the src pad back to its transceiver (pad property) -> mid / mlineindex.
    GstWebRTCRTPTransceiver* tr = nullptr; g_object_get(pad, "transceiver", &tr, nullptr);
    GObj<GstWebRTCRTPTransceiver> trg(tr);
    std::string mid = tr ? str_prop(tr, "mid") : "<no transceiver>"; guint mline = tr ? uint_prop(tr, "mlineindex") : 999;
    logf("%s: pad-added %s (#%d) transceiver mid=%s mlineindex=%u caps=%.90s", name.c_str(), pname.get(), pads_added.load() + 1, mid.c_str(), mline, cs.get());
    GstElement* q = gst_element_factory_make("queue", nullptr);
    GstElement* sink = gst_element_factory_make("fakesink", nullptr);
    g_object_set(sink, "sync", FALSE, "async", FALSE, nullptr);
    gst_bin_add_many(GST_BIN(pipeline.get()), q, sink, nullptr);
    gst_element_link(q, sink);
    gst_element_sync_state_with_parent(q);
    gst_element_sync_state_with_parent(sink);
    GObj<GstPad> qsink(gst_element_get_static_pad(q, "sink"));
    gst_pad_link(pad, qsink.get());
    auto rt = std::make_unique<RxTrack>();
    rt->pad = pname.get(); rt->mid = mid; rt->mline = mline;
    GObj<GstPad> ssink(gst_element_get_static_pad(sink, "sink"));
    gst_pad_add_probe(ssink.get(), GST_PAD_PROBE_TYPE_BUFFER, [](GstPad*, GstPadProbeInfo*, gpointer d) -> GstPadProbeReturn {
        auto* t = static_cast<RxTrack*>(d);
        int64_t now = g_get_monotonic_time(), last = t->last_us.exchange(now);
        if (t->watch && last) { int64_t gap = now - last, m = t->max_gap_us; while (gap > m && !t->max_gap_us.compare_exchange_weak(m, gap)) {} }
        ++t->count;
        return GST_PAD_PROBE_OK; }, rt.get(), nullptr);
    std::lock_guard lk(mu);
    rx[pname.get()] = std::move(rt);
    ++pads_added;
  }
  RxTrack* track(guint mline) { std::lock_guard lk(mu); for (auto& [k, t] : rx) if (t->mline == mline) return t.get(); return nullptr; }
  uint64_t rx_count(guint mline) { auto* t = track(mline); return t ? t->count.load() : 0; }
  std::string rx_pad(guint mline) { auto* t = track(mline); return t ? t->pad + "(mid=" + t->mid + ")" : "<no pad for mline " + std::to_string(mline) + ">"; }

  void on_data_channel(GstWebRTCDataChannel* ch) {
    logf("%s: on-data-channel label=%s id=%d ordered=%d protocol=%s", name.c_str(), str_prop(ch, "label").c_str(),
         enum_prop(ch, "id"), enum_prop(ch, "ordered"), str_prop(ch, "protocol").c_str());
    dc.store(static_cast<GstWebRTCDataChannel*>(g_object_ref(ch)));
    hook_channel(ch);
  }
  void hook_channel(GstWebRTCDataChannel* ch) {
    g_signal_connect(ch, "on-open", G_CALLBACK(+[](GstWebRTCDataChannel* c, gpointer d) {
        auto* p = static_cast<Peer*>(d); ++p->dc_open;
        logf("%s: datachannel on-open ready-state=%s", p->name.c_str(), enum_nick(GST_TYPE_WEBRTC_DATA_CHANNEL_STATE, enum_prop(c, "ready-state")).c_str()); }), this);
    g_signal_connect(ch, "on-message-string", G_CALLBACK(+[](GstWebRTCDataChannel*, gchar* s, gpointer d) {
        auto* p = static_cast<Peer*>(d); std::string m(s);
        post([p, m] { logf("%s: datachannel on-message-string \"%s\"", p->name.c_str(), m.c_str()); p->dc_messages.push_back(m); }); }), this);
    g_signal_connect(ch, "on-message-data", G_CALLBACK(+[](GstWebRTCDataChannel*, GBytes* b, gpointer d) {
        static_cast<Peer*>(d)->dc_bytes += g_bytes_get_size(b); }), this);
    g_signal_connect(ch, "on-error", G_CALLBACK(+[](GstWebRTCDataChannel*, GError* e, gpointer d) {
        logf("%s: datachannel on-error %s", static_cast<Peer*>(d)->name.c_str(), e->message); }), this);
  }
  GObj<GstWebRTCRTPTransceiver> transceiver(guint idx) {
    GstWebRTCRTPTransceiver* t = nullptr;
    g_signal_emit_by_name(webrtc, "get-transceiver", idx, &t);
    return GObj<GstWebRTCRTPTransceiver>(t);
  }
  Sdp local_description() { GstWebRTCSessionDescription* d = nullptr; g_object_get(webrtc, "local-description", &d, nullptr); return Sdp(d); }
  std::string conn_state() { return enum_nick(GST_TYPE_WEBRTC_PEER_CONNECTION_STATE, enum_prop(webrtc, "connection-state")); }
  std::string ice_state() { return enum_nick(GST_TYPE_WEBRTC_ICE_CONNECTION_STATE, enum_prop(webrtc, "ice-connection-state")); }
};

// Diagnostic: count buffers on every pad of selected elements inside a webrtcbin
// (nicesrc/nicesink/dtls/funnel) to localize where packets stop.
struct PadCounter { std::string key; std::atomic<uint64_t> n{0}; };
std::vector<std::unique_ptr<PadCounter>> counters;
void install_counters(Peer& p) {
  GstIterator* it = gst_bin_iterate_recurse(GST_BIN(p.webrtc));
  GValue v = G_VALUE_INIT;
  while (gst_iterator_next(it, &v) == GST_ITERATOR_OK) {
    auto* e = GST_ELEMENT(g_value_get_object(&v));
    GstElementFactory* f = gst_element_get_factory(e);
    std::string fname = f ? GST_OBJECT_NAME(f) : "";
    if (fname == "nicesrc" || fname == "nicesink" || fname == "dtlssrtpdec" || fname == "dtlssrtpenc" || fname == "rtpfunnel") {
      GstIterator* pit = gst_element_iterate_pads(e);
      GValue pv = G_VALUE_INIT;
      while (gst_iterator_next(pit, &pv) == GST_ITERATOR_OK) {
        auto* pad = GST_PAD(g_value_get_object(&pv));
        auto c = std::make_unique<PadCounter>(); c->key = p.name + ":" + fname + ":" + GST_PAD_NAME(pad);
        gst_pad_add_probe(pad, (GstPadProbeType)(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_BUFFER_LIST), [](GstPad*, GstPadProbeInfo* info, gpointer d) -> GstPadProbeReturn {
            auto* c = static_cast<PadCounter*>(d);
            if (info->type & GST_PAD_PROBE_TYPE_BUFFER_LIST) c->n += gst_buffer_list_length(GST_PAD_PROBE_INFO_BUFFER_LIST(info)); else ++c->n;
            return GST_PAD_PROBE_OK; }, c.get(), nullptr);
        counters.push_back(std::move(c));
        g_value_unset(&pv);
      }
      gst_iterator_free(pit);
    }
    g_value_unset(&v);
  }
  gst_iterator_free(it);
}
std::map<std::string, uint64_t> snapshot_counters() { std::map<std::string, uint64_t> m; for (auto& c : counters) m[c->key] = c->n; return m; }
std::string counters_delta(const std::map<std::string, uint64_t>& before) {
  std::string out; for (auto& c : counters) { uint64_t d = c->n - before.at(c->key); if (d) out += c->key + "=+" + std::to_string(d) + " "; }
  return out.empty() ? "(no buffers on any nice/dtls/funnel pad)" : out;
}

// videotestsrc ! capsfilter ! vp8enc ! rtpvp8pay ! webrtcbin.sink_%u, with a
// caps probe on the payloader src pad (the "caps gate").
std::unique_ptr<TxTrack> add_video_track(Peer& a, const char* pattern, guint pt) {
  auto t = std::make_unique<TxTrack>();
  GstElement* src = gst_element_factory_make("videotestsrc", nullptr);
  g_object_set(src, "is-live", TRUE, nullptr);
  gst_util_set_object_arg(G_OBJECT(src), "pattern", pattern);
  GstElement* cf = gst_element_factory_make("capsfilter", nullptr);
  GstCaps* caps = gst_caps_from_string("video/x-raw,width=320,height=240,framerate=30/1");
  g_object_set(cf, "caps", caps, nullptr); gst_caps_unref(caps);
  GstElement* enc = gst_element_factory_make("vp8enc", nullptr);
  g_object_set(enc, "deadline", (gint64)1, "cpu-used", 4, "target-bitrate", 300000, "keyframe-max-dist", 30, nullptr);
  GstElement* pay = gst_element_factory_make("rtpvp8pay", nullptr);
  g_object_set(pay, "pt", pt, nullptr);
  t->valve = gst_element_factory_make("valve", nullptr);
  gst_bin_add_many(GST_BIN(a.pipeline.get()), src, cf, enc, pay, t->valve, nullptr);
  gst_element_link_many(src, cf, enc, pay, t->valve, nullptr);
  GObj<GstPad> paysrc(gst_element_get_static_pad(pay, "src"));
  GObj<GstPad> valvesrc(gst_element_get_static_pad(t->valve, "src"));
  gst_pad_add_probe(paysrc.get(), GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM, [](GstPad*, GstPadProbeInfo* info, gpointer d) -> GstPadProbeReturn {
      GstEvent* e = GST_PAD_PROBE_INFO_EVENT(info);
      if (GST_EVENT_TYPE(e) != GST_EVENT_CAPS) return GST_PAD_PROBE_OK;
      GstCaps* c = nullptr; gst_event_parse_caps(e, &c);
      const GstStructure* s = gst_caps_get_structure(c, 0);
      auto* t = static_cast<TxTrack*>(d);
      GStr cs(gst_caps_to_string(c));
      if (gst_caps_is_fixed(c) && gst_structure_has_field(s, "payload") && gst_structure_has_field(s, "ssrc")) {
        t->caps = cs.get(); t->caps_ready = true;
        logf("caps-gate: fixed RTP caps on payloader src: %s", cs.get());
      } else logf("caps-gate: non-fixed / incomplete caps event: %s", cs.get());
      return GST_PAD_PROBE_OK; }, t.get(), nullptr);
  t->sinkpad = gst_element_request_pad_simple(a.webrtc, "sink_%u");   // owned by webrtcbin; ref dropped below
  gst_object_unref(t->sinkpad);
  logf("%s: requested %s, link=%d", a.name.c_str(), GST_PAD_NAME(t->sinkpad), gst_pad_link(valvesrc.get(), t->sinkpad));
  for (GstElement* e : {src, cf, enc, pay, t->valve}) gst_element_sync_state_with_parent(e);
  return t;
}

// ------------------------------------------------------------ negotiate ----
struct Hooks { std::function<void(const GstWebRTCSessionDescription*)> on_offer_created, on_local_set; };
// Full offer/answer round trip A -> B -> A over the main loop. `options` is
// passed to create-offer (ownership stays with the caller).
void negotiate(Peer& a, Peer& b, GstStructure* options, Hooks hooks, std::function<void(Sdp, Sdp)> done) {
  auto d = std::make_shared<std::function<void(Sdp, Sdp)>>(std::move(done));
  auto h = std::make_shared<Hooks>(std::move(hooks));
  logf("negotiate: create-offer on %s (options=%s)", a.name.c_str(), options ? GStr(gst_structure_to_string(options)).get() : "NULL");
  emit_async(a.webrtc, "create-offer", options, [&a, &b, d, h](Reply r) {
    GstWebRTCSessionDescription* o = nullptr;
    if (r) gst_structure_get(r.get(), "offer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &o, nullptr);
    if (!o) { logf("negotiate: create-offer FAILED: %s", reply_error(r).c_str()); (*d)(nullptr, nullptr); return; }
    auto offer = std::make_shared<Sdp>(o);
    logf("negotiate: offer created:%s", sdp_summary(o).c_str());
    if (h->on_offer_created) h->on_offer_created(o);
    emit_async(a.webrtc, "set-local-description", o, [&a, &b, d, h, offer](Reply) {
      if (h->on_local_set) h->on_local_set(offer->get());
      emit_async(b.webrtc, "set-remote-description", offer->get(), [&a, &b, d, offer](Reply) {
        b.flush_candidates();
        emit_async(b.webrtc, "create-answer", nullptr, [&a, &b, d, offer](Reply r) {
          GstWebRTCSessionDescription* an = nullptr;
          if (r) gst_structure_get(r.get(), "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &an, nullptr);
          if (!an) { logf("negotiate: create-answer FAILED: %s", reply_error(r).c_str()); (*d)(std::move(*offer), nullptr); return; }
          auto answer = std::make_shared<Sdp>(an);
          logf("negotiate: answer created:%s", sdp_summary(an).c_str());
          emit_async(b.webrtc, "set-local-description", an, [&a, d, offer, answer](Reply) {
            emit_async(a.webrtc, "set-remote-description", answer->get(), [&a, d, offer, answer](Reply) {
              a.flush_candidates();
              logf("negotiate: done, signaling-state A=%s", enum_nick(GST_TYPE_WEBRTC_SIGNALING_STATE, enum_prop(a.webrtc, "signaling-state")).c_str());
              (*d)(std::move(*offer), std::move(*answer));
            });
          });
        });
      });
    });
  });
}

GstElement* ctx_A_webrtc(); GstElement* ctx_B_webrtc();

// ---------------------------------------------------------------- stats ----
struct StatsSnap { std::set<std::string> types; std::map<guint, std::pair<guint64, guint64>> outbound, inbound; std::string sample; double t{}; };
StatsSnap parse_stats(const Reply& r) {
  StatsSnap s; s.t = now_s();
  if (!r) return s;
  gst_structure_foreach(r.get(), [](GQuark, const GValue* v, gpointer d) -> gboolean {
      if (!GST_VALUE_HOLDS_STRUCTURE(v)) return TRUE;
      auto* s = static_cast<StatsSnap*>(d);
      const GstStructure* st = gst_value_get_structure(v);
      gint type = 0; gst_structure_get(st, "type", GST_TYPE_WEBRTC_STATS_TYPE, &type, nullptr);
      s->types.insert(enum_nick(GST_TYPE_WEBRTC_STATS_TYPE, type));
      if (type == GST_WEBRTC_STATS_OUTBOUND_RTP) {
        guint ssrc = 0; guint64 bytes = 0, packets = 0;
        gst_structure_get_uint(st, "ssrc", &ssrc);
        gst_structure_get_uint64(st, "bytes-sent", &bytes);
        gst_structure_get_uint64(st, "packets-sent", &packets);
        s->outbound[ssrc] = {bytes, packets};
        if (s->sample.empty()) s->sample = GStr(gst_structure_to_string(st)).get();
      }
      if (type == GST_WEBRTC_STATS_INBOUND_RTP) {
        guint ssrc = 0; guint64 bytes = 0, packets = 0;
        gst_structure_get_uint(st, "ssrc", &ssrc);
        gst_structure_get_uint64(st, "bytes-received", &bytes);
        gst_structure_get_uint64(st, "packets-received", &packets);
        s->inbound[ssrc] = {bytes, packets};
      }
      return TRUE; }, &s);
  return s;
}

// packets-sent (A, outbound-rtp) / packets-received (B, inbound-rtp) per ssrc over `ms`.
void stats_delta(int ms, std::function<void(std::string)> cb) {
  emit_async(ctx_A_webrtc(), "get-stats", nullptr, [ms, cb](Reply ra) {
    auto a1 = std::make_shared<StatsSnap>(parse_stats(ra));
    emit_async(ctx_B_webrtc(), "get-stats", nullptr, [ms, cb, a1](Reply rb) {
      auto b1 = std::make_shared<StatsSnap>(parse_stats(rb));
      after(ms, [cb, a1, b1] {
        emit_async(ctx_A_webrtc(), "get-stats", nullptr, [cb, a1, b1](Reply ra) {
          auto a2 = std::make_shared<StatsSnap>(parse_stats(ra));
          emit_async(ctx_B_webrtc(), "get-stats", nullptr, [cb, a1, b1, a2](Reply rb) {
            StatsSnap b2 = parse_stats(rb);
            std::string out = "A packets-sent:";
            for (auto& [ssrc, v] : a2->outbound) out += " ssrc" + std::to_string(ssrc) + "=+" + std::to_string(v.second - a1->outbound[ssrc].second);
            out += " | B packets-received:";
            for (auto& [ssrc, v] : b2.inbound) out += " ssrc" + std::to_string(ssrc) + "=+" + std::to_string(v.second - b1->inbound[ssrc].second);
            cb(out);
          });
        });
      });
    });
  });
}

// Kernel UDP counters for this network namespace (/proc/net/snmp), to see whether
// datagrams leave/enter the process at all.
std::map<std::string, long long> udp_counters() {
  std::map<std::string, long long> m;
  FILE* f = std::fopen("/proc/net/snmp", "r");
  if (!f) return m;
  char l1[512], l2[512];
  while (std::fgets(l1, sizeof l1, f)) {
    if (std::strncmp(l1, "Udp:", 4) || !std::fgets(l2, sizeof l2, f)) continue;
    std::vector<std::string> k, v; char* sp = nullptr;
    for (char* t = strtok_r(l1 + 4, " \n", &sp); t; t = strtok_r(nullptr, " \n", &sp)) k.push_back(t);
    for (char* t = strtok_r(l2 + 4, " \n", &sp); t; t = strtok_r(nullptr, " \n", &sp)) v.push_back(t);
    for (size_t i = 0; i < k.size() && i < v.size(); i++) m[k[i]] = std::atoll(v[i].c_str());
    break;
  }
  std::fclose(f);
  return m;
}
std::string udp_delta(const std::map<std::string, long long>& a) {
  auto b = udp_counters(); std::string out;
  for (const char* k : {"InDatagrams", "OutDatagrams", "InErrors", "RcvbufErrors", "SndbufErrors", "NoPorts"}) out += std::string(k) + "=+" + std::to_string(b[k] - (a.count(k) ? a.at(k) : 0)) + " ";
  return out;
}
enum class RemoveMode { Inactive, SendOnly, ReleasePad };
RemoveMode remove_mode = RemoveMode::Inactive;

// ------------------------------------------------------------- sequence ----
struct Ctx {
  std::unique_ptr<Peer> A, B;
  std::unique_ptr<TxTrack> track1, track2;
  GObj<GstWebRTCDataChannel> dc;            // A's channel
  std::atomic<int> ba_notify{0}, ba_low{0}; std::atomic<guint64> ba_max{0};
  std::string mid0_after_offer1, ufrag0_before_restart, pwd0_before_restart;
  StatsSnap stats1;
  uint64_t t1_before_reneg{}, t2_pad_count_before{};
  guint track2_mline{99};                    // m-line index of track 2
};
Ctx ctx;
GstElement* ctx_A_webrtc() { return ctx.A->webrtc; }
GstElement* ctx_B_webrtc() { return ctx.B->webrtc; }

void finish(int code) {
  std::printf("\n===== SUMMARY (bundle-policy=%s) =====\n", enum_nick(GST_TYPE_WEBRTC_BUNDLE_POLICY, enum_prop(ctx.A->webrtc, "bundle-policy")).c_str());
  std::printf("%-9s %-8s %s\n", "ID", "VERDICT", "NOTE");
  for (auto& r : results) std::printf("%-9s %-8s %s\n", r.id.c_str(), r.verdict.c_str(), r.note.c_str());
  std::fflush(stdout);
  ctx.A.reset(); ctx.B.reset();
  g_main_loop_quit(loop);
  ctx.dc.reset();
  std::exit(code);
}
using Step = std::function<void(std::function<void()>)>;
std::vector<Step> steps;
void run_steps(size_t i) { if (i >= steps.size()) { finish(0); return; } steps[i]([i] { run_steps(i + 1); }); }

bool connected() { return ctx.A->conn_state() == "connected" && ctx.B->conn_state() == "connected"; }

void build_steps(GstWebRTCBundlePolicy bp) {
  // ---- setup
  steps.push_back([bp](auto next) {
    ctx.A = std::make_unique<Peer>("A", bp); ctx.B = std::make_unique<Peer>("B", bp);
    ctx.A->remote = ctx.B.get(); ctx.B->remote = ctx.A.get();
    logf("Q6: webrtcbin props: latency=%d ms, bundle-policy=%s, ice-transport-policy=%s", enum_prop(ctx.A->webrtc, "latency"),
         enum_nick(GST_TYPE_WEBRTC_BUNDLE_POLICY, enum_prop(ctx.A->webrtc, "bundle-policy")).c_str(),
         enum_nick(GST_TYPE_WEBRTC_ICE_TRANSPORT_POLICY, enum_prop(ctx.A->webrtc, "ice-transport-policy")).c_str());
    {  // TURN API check on a throwaway element so it cannot slow down ICE below
      GObj<GstElement> w(gst_element_factory_make("webrtcbin", nullptr));
      gst_object_ref_sink(w.get());
      g_object_set(w.get(), "turn-server", "turn://user:s3cret@turn.example.org:3478?transport=tcp", nullptr);
      gboolean ok1 = FALSE, ok2 = FALSE, ok3 = FALSE;
      g_signal_emit_by_name(w.get(), "add-turn-server", "turn://user:s3cret@10.0.0.1:3478", &ok1);
      g_signal_emit_by_name(w.get(), "add-turn-server", "turns://user:s3cret@turn.example.org:5349", &ok2);
      g_signal_emit_by_name(w.get(), "add-turn-server", "http://not-a-turn-url", &ok3);
      record("Q6-turn", ok1 && ok2 && !ok3, "turn-server prop readback=" + str_prop(w.get(), "turn-server") +
             "; add-turn-server(turn://...)=" + std::to_string(ok1) + " (turns://...)=" + std::to_string(ok2) + " (http://...)=" + std::to_string(ok3));
    }
    logf("GstWebRTCDataChannel props: %s", list_props(GST_TYPE_WEBRTC_DATA_CHANNEL).c_str());
    logf("GstWebRTCDataChannel signals: %s", list_signals(GST_TYPE_WEBRTC_DATA_CHANNEL).c_str());
    logf("GstWebRTCRTPTransceiver props: %s", list_props(GST_TYPE_WEBRTC_RTP_TRANSCEIVER).c_str());
    // Q1: data channel BEFORE any offer (webrtcbin still in NULL state).
    GstWebRTCDataChannel* ch = nullptr;
    g_signal_emit_by_name(ctx.A->webrtc, "create-data-channel", "control", nullptr, &ch);
    logf("A: create-data-channel in NULL state -> %p (CRITICAL above = webrtcbin must be >= READY)", (void*)ch);
    ctx.track1 = add_video_track(*ctx.A, "ball", 96);
    gst_element_set_state(ctx.B->pipeline.get(), GST_STATE_PLAYING);
    gst_element_set_state(ctx.A->pipeline.get(), GST_STATE_PLAYING);
    if (!ch) {
      g_signal_emit_by_name(ctx.A->webrtc, "create-data-channel", "control", nullptr, &ch);
      logf("A: create-data-channel after PLAYING -> %p", (void*)ch);
    }
    ctx.dc.reset(ch);
    if (ch) {
      ctx.A->hook_channel(ch);
      g_signal_connect(ch, "notify::buffered-amount", G_CALLBACK(+[](GObject* o, GParamSpec*, gpointer) {
          guint64 v = 0; g_object_get(o, "buffered-amount", &v, nullptr);
          ++ctx.ba_notify; guint64 m = ctx.ba_max; while (v > m && !ctx.ba_max.compare_exchange_weak(m, v)) {} }), nullptr);
      g_signal_connect(ch, "on-buffered-amount-low", G_CALLBACK(+[](GstWebRTCDataChannel*, gpointer) { ++ctx.ba_low; logf("A: on-buffered-amount-low"); }), nullptr);
    }
    next();
  });
  // ---- Q2: caps gate, mid lifecycle, first negotiation
  steps.push_back([](auto next) {
    wait_until([] { return ctx.track1->caps_ready.load(); }, 5000, [next](bool ok) {
      record("Q2-caps", ok, ok ? "fixed caps (payload+ssrc) seen on payloader src pad before create-offer: " + ctx.track1->caps.substr(0, 120) : "no fixed caps within 5 s");
      auto t = ctx.A->transceiver(0);
      std::string mid_pre = t ? str_prop(t.get(), "mid") : "<no transceiver>";
      logf("Q2: mid before create-offer = %s (negotiation-needed so far: %d)", mid_pre.c_str(), ctx.A->negotiation_needed.load());
      auto mid_after_create = std::make_shared<std::string>(), mid_after_local = std::make_shared<std::string>();
      Hooks h;
      h.on_offer_created = [mid_after_create](const GstWebRTCSessionDescription*) { auto t = ctx.A->transceiver(0); *mid_after_create = str_prop(t.get(), "mid"); };
      h.on_local_set = [mid_after_local](const GstWebRTCSessionDescription*) { auto t = ctx.A->transceiver(0); *mid_after_local = str_prop(t.get(), "mid"); };
      negotiate(*ctx.A, *ctx.B, nullptr, std::move(h), [next, mid_pre, mid_after_create, mid_after_local](Sdp offer, Sdp answer) {
        if (!offer || !answer) { record("Q1-offer", false, "negotiation failed"); finish(1); }
        std::string sdp = sdp_text(offer.get());
        record("Q1-offer", sdp.find("m=application") != std::string::npos, "offer m-sections:" + sdp_summary(offer.get()));
        auto t = ctx.A->transceiver(0);
        std::string mid_after_answer = str_prop(t.get(), "mid");
        ctx.mid0_after_offer1 = mid_after_answer;
        record("Q2-mid", mid_after_answer != "(null)",
               "mid: before create-offer=" + mid_pre + " | after create-offer(before SLD)=" + *mid_after_create +
               " | after set-local=" + *mid_after_local + " | after answer applied=" + mid_after_answer +
               " (offer m=0 a=mid:" + media_attr(offer.get(), 0, "mid") + ")");
        next();
      });
    });
  });
  // ---- wait connected + data channel open, send message
  steps.push_back([](auto next) {
    wait_until([] { return connected() && ctx.B->dc.load() && ctx.A->dc_open > 0; }, 15000, [next](bool ok) {
      record("Q1-ondc", ctx.B->dc.load() != nullptr, "B on-data-channel fired: " + std::string(ctx.B->dc.load() ? "yes" : "no") +
             "; A connection-state=" + ctx.A->conn_state() + " B=" + ctx.B->conn_state() + " (connected+open within 15 s: " + (ok ? "yes" : "NO") + ")");
      if (!ok) { finish(1); }
      GError* err = nullptr;
      gboolean sent = gst_webrtc_data_channel_send_string_full(ctx.dc.get(), "hello from A", &err);
      logf("A: send_string_full -> %d %s", sent, err ? err->message : "");
      if (err) g_error_free(err);
      wait_until([] { return !ctx.B->dc_messages.empty(); }, 3000, [next](bool ok) {
        record("Q1-msg", ok && ctx.B->dc_messages[0] == "hello from A", ok ? "B received \"" + ctx.B->dc_messages[0] + "\" via on-message-string" : "no message within 3 s");
        // buffered-amount: threshold + burst of binary messages
        g_object_set(ctx.dc.get(), "buffered-amount-low-threshold", (guint64)4096, nullptr);
        guint64 thr = 0;
        g_object_get(ctx.dc.get(), "buffered-amount-low-threshold", &thr, nullptr);
        logf("A: default buffered-amount-low-threshold=%" G_GUINT64_FORMAT ", on-buffered-amount-low fired %d times before any burst", thr, ctx.ba_low.load());
        guint64 mms = 0;
        { GstWebRTCSCTPTransport* sctp = nullptr; g_object_get(ctx.A->webrtc, "sctp-transport", &sctp, nullptr);
          if (sctp) { g_object_get(sctp, "max-message-size", &mms, nullptr); g_object_unref(sctp); } }
        const gsize chunk = 32 * 1024; const int n = 64;
        for (int i = 0; i < n; i++) {
          auto* buf = static_cast<guint8*>(g_malloc(chunk)); std::memset(buf, i, chunk);
          GBytes* b = g_bytes_new_take(buf, chunk);
          GError* e = nullptr;
          if (!gst_webrtc_data_channel_send_data_full(ctx.dc.get(), b, &e)) { logf("A: send_data_full failed at %d: %s", i, e ? e->message : "?"); if (e) g_error_free(e); }
          g_bytes_unref(b);
        }
        guint64 ba_now = 0; g_object_get(ctx.dc.get(), "buffered-amount", &ba_now, nullptr);
        logf("A: buffered-amount right after burst = %" G_GUINT64_FORMAT " (max-message-size=%" G_GUINT64_FORMAT ")", ba_now, mms);
        wait_until([n, chunk] { return ctx.B->dc_bytes >= (guint64)n * chunk; }, 10000, [next, ba_now, n, chunk](bool ok) {
          guint64 ba_end = 0; g_object_get(ctx.dc.get(), "buffered-amount", &ba_end, nullptr);
          record("Q1-bufamt", ok && ctx.ba_notify > 0 && ctx.ba_low > 0 && ctx.ba_max > 0,
                 "B received " + std::to_string(ctx.B->dc_bytes.load()) + "/" + std::to_string((guint64)n * chunk) + " bytes; buffered-amount after burst=" +
                 std::to_string(ba_now) + " max seen=" + std::to_string(ctx.ba_max.load()) + " end=" + std::to_string(ba_end) +
                 "; notify::buffered-amount x" + std::to_string(ctx.ba_notify.load()) + "; on-buffered-amount-low x" + std::to_string(ctx.ba_low.load()));
          next();
        });
      });
    });
  });
  // ---- media flowing + Q5 stats
  steps.push_back([](auto next) {
    wait_until([] { return ctx.B->rx_count(0) > 30; }, 10000, [next](bool ok) {
      record("media-1", ok, "B " + ctx.B->rx_pad(0) + " buffers=" + std::to_string(ctx.B->rx_count(0)) + " pads_added=" + std::to_string(ctx.B->pads_added.load()) +
             " on-new-transceiver=" + std::to_string(ctx.B->new_transceiver.load()));
      if (!ok) finish(1);
      emit_async(ctx.A->webrtc, "get-stats", nullptr, [next](Reply r) {
        ctx.stats1 = parse_stats(r);
        after(1000, [next] {
          emit_async(ctx.A->webrtc, "get-stats", nullptr, [next](Reply r) {
            StatsSnap s2 = parse_stats(r);
            std::string types; for (auto& t : s2.types) types += t + " ";
            std::string rates; bool have = false;
            for (auto& [ssrc, bp] : s2.outbound) {
              auto it = ctx.stats1.outbound.find(ssrc); if (it == ctx.stats1.outbound.end()) continue;
              double dt = s2.t - ctx.stats1.t;
              rates += "ssrc=" + std::to_string(ssrc) + ": " + std::to_string((guint64)((bp.first - it->second.first) * 8 / dt / 1000)) + " kbit/s, " +
                       std::to_string((guint64)((bp.second - it->second.second) / dt)) + " pkt/s; ";
              have = bp.first > it->second.first;
            }
            logf("Q5: sample outbound-rtp: %.600s", s2.sample.c_str());
            record("Q5-stats", have, "types={" + types + "} outbound-rtp deltas over 1 s: " + rates);
            next();
          });
        });
      });
    });
  });
  // ---- Q3: add second track, renegotiate
  steps.push_back([](auto next) {
    auto* t1 = ctx.B->track(0);
    t1->max_gap_us = 0; t1->watch = true;
    ctx.t1_before_reneg = t1->count;
    int nn_before = ctx.A->negotiation_needed;
    ctx.track2 = add_video_track(*ctx.A, "smpte", 97);
    wait_until([nn_before] { return ctx.A->negotiation_needed > nn_before && ctx.track2->caps_ready.load(); }, 5000, [next, nn_before](bool ok) {
      record("Q3-onn", ctx.A->negotiation_needed > nn_before, "on-negotiation-needed after requesting " + std::string(GST_PAD_NAME(ctx.track2->sinkpad)) + ": " +
             std::to_string(ctx.A->negotiation_needed.load() - nn_before) + " firing(s); caps ready=" + std::to_string(ctx.track2->caps_ready.load()));
      (void)ok;
      negotiate(*ctx.A, *ctx.B, nullptr, {}, [next](Sdp offer, Sdp answer) {
        if (!offer || !answer) { record("Q3-mids", false, "renegotiation failed"); next(); return; }
        guint n = gst_sdp_message_medias_len(offer->sdp);
        // find the m-section for pt 97 (track 2)
        int m2 = -1;
        for (guint i = 0; i < n; i++) { const GstSDPMedia* m = gst_sdp_message_get_media(offer->sdp, i); if (!std::strcmp(gst_sdp_media_get_media(m), "video") && (int)i != 0) m2 = (int)i; }
        std::string mid0 = media_attr(offer.get(), 0, "mid"), mid2 = m2 >= 0 ? media_attr(offer.get(), m2, "mid") : "<none>";
        record("Q3-mids", mid0 == ctx.mid0_after_offer1 && m2 >= 0 && mid2 != mid0,
               "offer#2 m=0 mid=" + mid0 + " (was " + ctx.mid0_after_offer1 + "), new video m=" + std::to_string(m2) + " mid=" + mid2 + " ; all:" + sdp_summary(offer.get()));
        ctx.track2_mline = m2 >= 0 ? (guint)m2 : 99;
        wait_until([] { return ctx.B->rx_count(ctx.track2_mline) > 30; }, 8000, [next](bool ok) {
          record("Q3-track2", ok, "B pad for m=" + std::to_string(ctx.track2_mline) + " is " + ctx.B->rx_pad(ctx.track2_mline) + " buffers=" + std::to_string(ctx.B->rx_count(ctx.track2_mline)) + " pads_added=" + std::to_string(ctx.B->pads_added.load()) +
                 " on-new-transceiver(B)=" + std::to_string(ctx.B->new_transceiver.load()));
          after(1000, [next] {
            auto* t1 = ctx.B->track(0); t1->watch = false;
            uint64_t after_c = t1->count; double gap_ms = t1->max_gap_us / 1000.0;
            record("Q3-cont", after_c > ctx.t1_before_reneg && gap_ms < 200.0,
                   "track1 buffers before=" + std::to_string(ctx.t1_before_reneg) + " after=" + std::to_string(after_c) + " max inter-buffer gap during renegotiation=" +
                   std::to_string(gap_ms) + " ms");
            next();
          });
        });
      });
    });
  });
  // ---- Q3: remove track 2 (--remove=inactive|sendonly|release-pad) and renegotiate
  steps.push_back([](auto next) {
    const char* mode = remove_mode == RemoveMode::Inactive ? "direction=inactive" : remove_mode == RemoveMode::SendOnly ? "direction=sendonly" : "release_request_pad";
    int nn_before = ctx.A->negotiation_needed;
    GObj<GstWebRTCRTPTransceiver> t2;
    GArray* arr = nullptr; g_signal_emit_by_name(ctx.A->webrtc, "get-transceivers", &arr);
    for (guint i = 0; arr && i < arr->len; i++) {
      auto* t = g_array_index(arr, GstWebRTCRTPTransceiver*, i);
      if (uint_prop(t, "mlineindex") != 0 && enum_prop(t, "kind") == GST_WEBRTC_KIND_VIDEO) t2.reset(static_cast<GstWebRTCRTPTransceiver*>(g_object_ref(t)));
    }
    if (arr) g_array_unref(arr);
    if (!t2) { record("Q3-remove", "UNCLEAR", "could not find transceiver for track 2"); next(); return; }
    guint mline2 = uint_prop(t2.get(), "mlineindex");
    logf("A: track2 transceiver mlineindex=%u mid=%s dir=%s current-direction=%s -> %s", mline2, str_prop(t2.get(), "mid").c_str(),
         enum_nick(GST_TYPE_WEBRTC_RTP_TRANSCEIVER_DIRECTION, enum_prop(t2.get(), "direction")).c_str(),
         enum_nick(GST_TYPE_WEBRTC_RTP_TRANSCEIVER_DIRECTION, enum_prop(t2.get(), "current-direction")).c_str(), mode);
    if (remove_mode == RemoveMode::Inactive) g_object_set(t2.get(), "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_INACTIVE, nullptr);
    else if (remove_mode == RemoveMode::SendOnly) g_object_set(t2.get(), "direction", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_SENDONLY, nullptr);
    else {  // stop the source, unlink, release the request pad
      g_object_set(ctx.track2->valve, "drop", TRUE, nullptr);
      GObj<GstPad> peer(gst_pad_get_peer(ctx.track2->sinkpad));
      if (peer) gst_pad_unlink(peer.get(), ctx.track2->sinkpad);
      gst_element_release_request_pad(ctx.A->webrtc, ctx.track2->sinkpad);
      ctx.track2->sinkpad = nullptr;
      logf("A: released sink pad; transceiver direction now %s", enum_nick(GST_TYPE_WEBRTC_RTP_TRANSCEIVER_DIRECTION, enum_prop(t2.get(), "direction")).c_str());
    }
    wait_until([nn_before] { return ctx.A->negotiation_needed > nn_before; }, 2000, [next, mode, mline2](bool onn) {
      logf("Q3-remove: on-negotiation-needed after %s: %s", mode, onn ? "yes" : "NO");
      negotiate(*ctx.A, *ctx.B, nullptr, {}, [next, mode, onn, mline2](Sdp offer_in, Sdp answer) {
        if (!offer_in || !answer) { record("Q3-remove", false, "renegotiation failed"); next(); return; }
        auto offer = std::make_shared<Sdp>(std::move(offer_in));
        std::string dir = media_dir(offer->get(), mline2), adir = media_dir(answer.get(), mline2);
        uint64_t c0 = ctx.B->rx_count(ctx.track2_mline), t1a = ctx.B->rx_count(0);
        after(1500, [next, mode, onn, dir, adir, c0, t1a, offer] {
          uint64_t c1 = ctx.B->rx_count(ctx.track2_mline), t1b = ctx.B->rx_count(0);
          bool t2_stopped = c1 == c0, t1_flows = t1b > t1a + 15;
          const char* want_dir = remove_mode == RemoveMode::SendOnly ? "sendonly" : "inactive";
          record("Q3-remove", dir == want_dir && (remove_mode == RemoveMode::SendOnly || t2_stopped) && t1_flows,
                 std::string("[") + mode + "] on-negotiation-needed=" + (onn ? "yes" : "no") + "; offer#3 track2 dir=" + dir + " answer dir=" + adir +
                 "; track2 buffers over 1.5 s: +" + std::to_string(c1 - c0) + "; track1 over 1.5 s: +" + std::to_string(t1b - t1a) + "; mids:" + sdp_summary(offer->get()));
          if (ctx.track2->sinkpad)
            logf("Q3-remove: A sink pads blocking? %s=%d %s=%d", GST_PAD_NAME(ctx.track1->sinkpad), gst_pad_is_blocking(ctx.track1->sinkpad),
                 GST_PAD_NAME(ctx.track2->sinkpad), gst_pad_is_blocking(ctx.track2->sinkpad));
          // Localize: rtpsession stats on both sides, buffer counters on the nice/dtls pads, kernel UDP counters.
          if (counters.empty()) { install_counters(*ctx.A); install_counters(*ctx.B); }
          auto snap = std::make_shared<std::map<std::string, uint64_t>>(snapshot_counters());
          auto udp = std::make_shared<std::map<std::string, long long>>(udp_counters());
          stats_delta(1000, [next, snap, udp](std::string d1) {
            logf("Q3-remove: kernel UDP counters over 1 s: %s", udp_delta(*udp).c_str());
            logf("Q3-remove: rtpsession stats over 1 s (track2 source still pushing): %s", d1.c_str());
            logf("Q3-remove: transport pad counters over 1 s: %s", counters_delta(*snap).c_str());
            // Bidirectional liveness probe over the data channel (shares the bundled ICE/DTLS transport).
            size_t a_msgs = ctx.A->dc_messages.size(), b_msgs = ctx.B->dc_messages.size();
            if (ctx.B->dc.load()) gst_webrtc_data_channel_send_string_full(ctx.B->dc.load(), "ping B->A after remove", nullptr);
            gst_webrtc_data_channel_send_string_full(ctx.dc.get(), "ping A->B after remove", nullptr);
            after(1000, [next, a_msgs, b_msgs, d1] {
              bool a_ok = ctx.A->dc_messages.size() > a_msgs, b_ok = ctx.B->dc_messages.size() > b_msgs;
              record("Q3-rmdc", a_ok && b_ok, std::string("datachannel after removal: B->A ") + (a_ok ? "arrived" : "LOST") + ", A->B " + (b_ok ? "arrived" : "LOST"));
              // Recovery attempt: stop pushing into the removed track's sink pad via the valve.
              g_object_set(ctx.track2->valve, "drop", TRUE, nullptr);
              uint64_t t1c = ctx.B->rx_count(0);
              auto udp2 = std::make_shared<std::map<std::string, long long>>(udp_counters());
              stats_delta(1500, [next, t1c, d1, udp2](std::string d2) {
                uint64_t t1d = ctx.B->rx_count(0);
                logf("Q3-rmvalve: kernel UDP counters over 1.5 s: %s", udp_delta(*udp2).c_str());
                record("Q3-rmvalve", t1d > t1c + 15, "after valve drop=true on track2: track1 buffers over 1.5 s: +" + std::to_string(t1d - t1c) + "; stats " + d2 +
                       " (before valve: " + d1 + ")");
                next();
              });
            });
          });
        });
      });
    });
  });
  // ---- Q4: ICE restart via create-offer options
  steps.push_back([](auto next) {
    Sdp cur = ctx.A->local_description();
    ctx.ufrag0_before_restart = media_attr(cur.get(), 0, "ice-ufrag"); ctx.pwd0_before_restart = media_attr(cur.get(), 0, "ice-pwd");
    int ice_changes_before = ctx.A->ice_state_changes;
    uint64_t t1a = ctx.B->rx_count(0);
    GstStructure* opts = gst_structure_new("options", "ice-restart", G_TYPE_BOOLEAN, TRUE, "iceRestart", G_TYPE_BOOLEAN, TRUE, nullptr);
    negotiate(*ctx.A, *ctx.B, opts, {}, [next, ice_changes_before, t1a](Sdp offer, Sdp answer) {
      if (!offer || !answer) { record("Q4-restart", "UNCLEAR", "create-offer/answer with ice-restart failed: see log"); next(); return; }
      std::string uf = media_attr(offer.get(), 0, "ice-ufrag"), pw = media_attr(offer.get(), 0, "ice-pwd");
      bool changed = uf != ctx.ufrag0_before_restart && pw != ctx.pwd0_before_restart;
      after(2000, [next, changed, uf, pw, ice_changes_before, t1a] {
        uint64_t t1b = ctx.B->rx_count(0);
        record("Q4-restart", changed ? "PASS" : "FAIL",
               "offer ufrag before=" + ctx.ufrag0_before_restart + " after=" + uf + " (pwd changed=" + std::string(pw != ctx.pwd0_before_restart ? "yes" : "no") +
               "); ice-connection-state changes during=" + std::to_string(ctx.A->ice_state_changes - ice_changes_before) + " now A=" + ctx.A->ice_state() +
               " B=" + ctx.B->ice_state() + "; track1 buffers over 2 s: +" + std::to_string(t1b - t1a));
        next();
      });
    });
    gst_structure_free(opts);
  });
}

}  // namespace

bool g_reuse_pads = false;

int main(int argc, char** argv) {
  gst_init(&argc, &argv);
  GstWebRTCBundlePolicy bp = GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE;
  for (int i = 1; i < argc; i++) {
    if (!std::strcmp(argv[i], "--bundle=none")) bp = GST_WEBRTC_BUNDLE_POLICY_NONE;
    else if (!std::strcmp(argv[i], "--bundle=max-bundle")) bp = GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE;
    else if (!std::strcmp(argv[i], "--bundle=balanced")) bp = GST_WEBRTC_BUNDLE_POLICY_BALANCED;
    else if (!std::strcmp(argv[i], "--remove=inactive")) remove_mode = RemoveMode::Inactive;
    else if (!std::strcmp(argv[i], "--remove=sendonly")) remove_mode = RemoveMode::SendOnly;
    else if (!std::strcmp(argv[i], "--remove=release-pad")) remove_mode = RemoveMode::ReleasePad;
    else if (!std::strcmp(argv[i], "--reuse-pads")) g_reuse_pads = true;
    else { std::fprintf(stderr, "usage: %s [--bundle=none|balanced|max-bundle] [--remove=inactive|sendonly|release-pad] [--reuse-pads]\n", argv[0]); return 2; }
  }
  logf("GStreamer %s, webrtcbin bundle-policy=%s reuse-source-pads=%s", gst_version_string(), enum_nick(GST_TYPE_WEBRTC_BUNDLE_POLICY, bp).c_str(), g_reuse_pads ? "true" : "false");
  loop = g_main_loop_new(nullptr, FALSE);
  build_steps(bp);
  g_timeout_add(120000, [](gpointer) -> gboolean { record("WATCHDOG", "FAIL", "120 s global timeout"); finish(3); return G_SOURCE_REMOVE; }, nullptr);
  post([] { run_steps(0); });
  g_main_loop_run(loop);
  return 0;
}
