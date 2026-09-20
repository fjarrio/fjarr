// fjarr-opsim — the operator simulator: speaks the operator side of docs/08
// against the real fjarr-server, answers the agent's offer with its own
// webrtcbin (the spike's answerer), decodes the received video and reads the
// frame stamp, and runs named fault/acceptance scenarios.
// spec: docs/23-agent-core-architecture.md#fjarr-opsim-the-operator-simulator
// spec: docs/08-protocol.md#signaling · #datachannel-topology · #fjarr-core · #track-control · #renegotiation
// spec: docs/06-capabilities.md#fjarrtest--the-built-in-test-capability-slice-3
// spec: docs/25-browser-lab.md#frame-stamp-the-latency-harnesss-oracle
//
// Threading: a CoreLoop (private GMainContext) on its own thread owns the
// signaling socket (libsoup) and every timer; the scenario runs on the main
// thread as a straight-line script that blocks on a condition variable over
// the shared observation state. GStreamer callbacks arrive on streaming /
// webrtcbin threads and only ever record observations under the lock.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include <gio/gio.h>
#include <glib.h>
#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/video/video.h>
#include <gst/webrtc/webrtc.h>
#include <libsoup/soup.h>

#include <nlohmann/json.hpp>

#include <fjarr/capability.hpp>

#include "core/glib/raii.hpp"
#include "core/loop.hpp"
#include "core/protocol.hpp"

using nlohmann::json;
using fjarr::Envelope;
namespace glib = fjarr::glib;
namespace protocol = fjarr::protocol;

namespace {

// ------------------------------------------------------------------ options

struct Options {
    std::string server = "ws://fjarr-server:8080/ws";
    std::string robot;
    std::string grant_secret;
    std::string scenario;
    std::string json_out;
    std::string ice_policy = "all";
    std::string introspect; // http://127.0.0.1:7381
    int timeout_s = 60;
    bool verbose = false;
};

void usage() {
    std::fprintf(stderr,
                 "usage: fjarr-opsim --server ws://host:8080/ws --robot <id> --grant-secret <secret> --scenario <name>\n"
                 "                   [--json out.json] [--timeout 60] [--ice-policy all|relay]\n"
                 "                   [--introspect http://127.0.0.1:7381] [--verbose]\n"
                 "scenarios: smoke toggle hotplug silent-operator no-answer socket-drop ice-restart deadman relay-only\n"
                 "           (soak, netem-<profile>: slice 3c)\n"
                 "exit: 0 all assertions pass, 1 any fail, 2 usage, 3 timeout / not in this slice\n");
}

bool parse_args(int argc, char** argv, Options& o) {
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto need = [&](std::string& out) {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                return false;
            }
            out = argv[++i];
            return true;
        };
        std::string v;
        if (a == "--server") {
            if (!need(o.server)) return false;
        } else if (a == "--robot") {
            if (!need(o.robot)) return false;
        } else if (a == "--grant-secret") {
            if (!need(o.grant_secret)) return false;
        } else if (a == "--scenario") {
            if (!need(o.scenario)) return false;
        } else if (a == "--json") {
            if (!need(o.json_out)) return false;
        } else if (a == "--ice-policy") {
            if (!need(o.ice_policy)) return false;
        } else if (a == "--introspect") {
            if (!need(o.introspect)) return false;
        } else if (a == "--timeout") {
            if (!need(v)) return false;
            o.timeout_s = std::atoi(v.c_str());
        } else if (a == "--verbose" || a == "-v") {
            o.verbose = true;
        } else if (a == "--help" || a == "-h") {
            usage();
            std::exit(0);
        } else {
            std::fprintf(stderr, "unknown argument %s\n", a.c_str());
            return false;
        }
    }
    if (o.robot.empty() || o.scenario.empty()) return false;
    if (o.grant_secret.empty()) {
        if (const char* e = std::getenv("FJARR_GRANT_HS256_SECRET")) o.grant_secret = e;
    }
    if (o.grant_secret.empty()) {
        std::fprintf(stderr, "--grant-secret (or FJARR_GRANT_HS256_SECRET) is required\n");
        return false;
    }
    if (o.ice_policy != "all" && o.ice_policy != "relay") {
        std::fprintf(stderr, "--ice-policy must be all|relay\n");
        return false;
    }
    if (o.timeout_s <= 0) o.timeout_s = 60;
    while (!o.introspect.empty() && o.introspect.back() == '/') o.introspect.pop_back();
    return true;
}

// -------------------------------------------------------------------- log

std::atomic<bool> g_verbose{false};
const std::int64_t g_t0_us = g_get_monotonic_time();

double elapsed_s() { return static_cast<double>(g_get_monotonic_time() - g_t0_us) / 1e6; }

void logf(const char* fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[opsim %8.3f] %s\n", elapsed_s(), buf);
    std::fflush(stderr);
}
void vlogf(const char* fmt, ...) {
    if (!g_verbose) return;
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::fprintf(stderr, "[opsim %8.3f] %s\n", elapsed_s(), buf);
    std::fflush(stderr);
}

// ------------------------------------------------------------------ grants

std::string b64url(const guchar* data, gsize len) {
    glib::GStrPtr enc(g_base64_encode(data, len));
    std::string s = enc.get();
    for (auto& c : s) {
        if (c == '+') c = '-';
        else if (c == '/') c = '_';
    }
    while (!s.empty() && s.back() == '=') s.pop_back();
    return s;
}
std::string b64url(const std::string& s) { return b64url(reinterpret_cast<const guchar*>(s.data()), s.size()); }

/// HS256 session grant with the claims fjarr-server's Hs256GrantVerifier reads
/// (hooks.rs GrantClaims; web/e2e/src/grant.ts is the TS twin).
std::string mint_grant(const std::string& secret, const std::string& robot_id) {
    const std::string header = b64url(json{{"alg", "HS256"}, {"typ", "JWT"}}.dump());
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    json claims{{"iss", "fjarr-opsim"},
                {"aud", "fjarr"},
                {"exp", now + 300},
                {"tenant", "lab"},
                {"robot_id", robot_id},
                {"operator", {{"id", "opsim@fjarr.test"}, {"label", "opsim"}}},
                {"capabilities", json::array({{{"name", "fjarr.test"}}})}};
    const std::string body = b64url(claims.dump());
    const std::string input = header + "." + body;
    GHmac* h = g_hmac_new(G_CHECKSUM_SHA256, reinterpret_cast<const guchar*>(secret.data()), secret.size());
    g_hmac_update(h, reinterpret_cast<const guchar*>(input.data()), static_cast<gssize>(input.size()));
    guint8 digest[32];
    gsize len = sizeof digest;
    g_hmac_get_digest(h, digest, &len);
    g_hmac_unref(h);
    return input + "." + b64url(digest, len);
}

// ------------------------------------------------------------- frame stamp

/// Decode the docs/25 stamp from a decoded 8-bit-luma frame: 96 blocks,
/// max(4, width/128) px wide, 16 px tall, sampled as a 3×3 mean at each
/// block's centre; sync 0xA5, 32-bit counter, 48-bit ms, XOR of the 11 bytes.
bool decode_stamp(const GstVideoFrame& f, std::uint32_t& counter, std::uint64_t& ts_ms) {
    const int w = GST_VIDEO_FRAME_WIDTH(&f), h = GST_VIDEO_FRAME_HEIGHT(&f);
    const int bw = std::max(4, w / 128);
    if (w < bw * 96 || h < 16) return false;
    if (GST_VIDEO_FRAME_COMP_DEPTH(&f, 0) != 8) return false;
    const auto* data = static_cast<const std::uint8_t*>(GST_VIDEO_FRAME_PLANE_DATA(&f, 0));
    const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&f, 0);
    const int pstride = GST_VIDEO_FRAME_COMP_PSTRIDE(&f, 0);
    const int offset = GST_VIDEO_FRAME_COMP_OFFSET(&f, 0);
    std::uint8_t bytes[12] = {0};
    for (int i = 0; i < 96; i++) {
        const int cx = i * bw + bw / 2, cy = 8;
        int sum = 0;
        for (int dy = -1; dy <= 1; dy++)
            for (int dx = -1; dx <= 1; dx++) sum += data[(cy + dy) * stride + offset + (cx + dx) * pstride];
        if (sum / 9 > 128) bytes[i >> 3] |= static_cast<std::uint8_t>(1 << (7 - (i & 7)));
    }
    if (bytes[0] != 0xA5) return false;
    std::uint8_t x = 0;
    for (int i = 0; i < 11; i++) x ^= bytes[i];
    if (x != bytes[11]) return false;
    counter = (std::uint32_t(bytes[1]) << 24) | (std::uint32_t(bytes[2]) << 16) | (std::uint32_t(bytes[3]) << 8) | bytes[4];
    ts_ms = 0;
    for (int i = 5; i <= 10; i++) ts_ms = (ts_ms << 8) | bytes[i];
    return true;
}

// ------------------------------------------------------------------ report

struct Timeout : std::exception {
    const char* what() const noexcept override { return "scenario timeout"; }
};
struct Abort : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct Result {
    std::string name;
    bool pass;
    std::string detail;
};

struct Report {
    std::vector<Result> results;
    void check(const std::string& name, bool pass, const std::string& detail) {
        results.push_back({name, pass, detail});
        std::printf("%s %s: %s\n", pass ? "PASS" : "FAIL", name.c_str(), detail.c_str());
        std::fflush(stdout);
    }
    int failed() const {
        return static_cast<int>(std::count_if(results.begin(), results.end(), [](const Result& r) { return !r.pass; }));
    }
};

// ------------------------------------------------------- observed state

struct TrackRx {
    std::string pad;    // webrtcbin src_<n>
    std::string tr_mid; // the pad's transceiver mid (docs/23 answerer facts)
    int pt = 0;         // payload type from the pad caps
    GstVideoInfo info{};
    bool have_info = false;
    std::uint64_t frames = 0, keyframes = 0, bad_stamps = 0;
    bool have_counter = false;
    std::uint32_t last_counter = 0;
    std::uint64_t last_stamp_ms = 0;
    std::uint32_t max_delta = 0; // largest counter step between consecutive decoded frames since watch reset
    std::uint32_t watch_first = 0, watch_last = 0;
    std::int64_t first_frame_us = 0, last_frame_us = 0;
};

struct Captured {
    double t;
    std::string dir; // in|out
    std::string channel; // signaling | fjarr:control | fjarr:realtime
    json body;
};

/// Everything the scenario thread waits on. One lock, one condition variable.
struct Shared {
    std::mutex mu;
    std::condition_variable cv;
    // signaling
    bool ws_open = false, ws_closed = false;
    int ws_close_code = 0;
    std::string ws_error;
    std::vector<json> sig_in; // every parsed inbound signaling message, in order
    std::string session_id;
    std::optional<json> turn;
    // peer
    std::string conn_state = "new", ice_state = "new", gathering_state = "new";
    std::map<std::string, bool> channels; // label -> open
    std::vector<Envelope> inbox; // every inbound envelope, in order
    std::map<std::string, TrackRx> tracks; // by pad name
    std::map<std::string, std::string> mid_by_track; // from the latest manifest
    std::map<std::string, int> pt_by_track;          // from the latest manifest
    std::vector<std::string> mid_mismatches; // pads whose transceiver mid disagrees with the manifest (by pt)
    unsigned manifest_version = 0;
    // heartbeat
    std::int64_t last_ping_us = 0, last_drive_us = 0;
    std::int64_t last_ping_t0 = 0;
    int pongs = 0;
    double best_rtt_ms = -1;
    std::string pong_defect; // first pong whose t0 is not the ping's t0 (docs/08#fjarr-core)
    // capture for --json
    std::vector<Captured> captured;

    void notify() { cv.notify_all(); }
    void capture(const char* dir, const char* channel, json body) {
        captured.push_back({elapsed_s(), dir, channel, std::move(body)});
    }
    std::string mid_of(const std::string& track_id) const {
        auto it = mid_by_track.find(track_id);
        return it == mid_by_track.end() ? "" : it->second;
    }
    /// The received pad carrying `track_id`: by transceiver mid (docs/08), falling back
    /// to the manifest's pt when the answerer routed the stream to the wrong m-line.
    TrackRx* track(const std::string& track_id) {
        const std::string mid = mid_of(track_id);
        auto pit = pt_by_track.find(track_id);
        const int pt = pit == pt_by_track.end() ? -1 : pit->second;
        if (mid.empty()) return nullptr;
        TrackRx* by_mid = nullptr;
        TrackRx* by_pt = nullptr;
        for (auto& [_, t] : tracks) {
            if (t.tr_mid == mid && t.pt == pt) return &t;
            if (t.pt == pt) by_pt = &t;
            if (t.tr_mid == mid && !by_mid) by_mid = &t;
        }
        return by_pt ? by_pt : by_mid;
    }
};

// --------------------------------------------------------------- signaling

/// The operator's WebSocket to fjarr-server (libsoup-3), owned by the loop thread.
class Signaling {
  public:
    Signaling(fjarr::CoreLoop& loop, Shared& sh, std::function<void(const json&)> on_message)
        : loop_(loop), sh_(sh), on_message_(std::move(on_message)) {}
    ~Signaling() { close(false); }

    void connect(const std::string& url) {
        loop_.call_sync([this, url] {
            session_.reset(soup_session_new()); // thread-default context = the core loop
            glib::GObjectPtr<SoupMessage> msg(soup_message_new(SOUP_METHOD_GET, url.c_str()));
            if (!msg) {
                std::lock_guard<std::mutex> lk(sh_.mu);
                sh_.ws_error = "invalid url " + url;
                sh_.ws_closed = true;
                sh_.notify();
                return;
            }
            soup_session_websocket_connect_async(
                session_.get(), msg.get(), nullptr, nullptr, G_PRIORITY_DEFAULT, nullptr,
                [](GObject* src, GAsyncResult* res, gpointer data) {
                    auto* self = static_cast<Signaling*>(data);
                    GError* err = nullptr;
                    SoupWebsocketConnection* conn = soup_session_websocket_connect_finish(SOUP_SESSION(src), res, &err);
                    if (!conn) {
                        glib::GErrorPtr e(err);
                        std::lock_guard<std::mutex> lk(self->sh_.mu);
                        self->sh_.ws_error = err ? err->message : "connect failed";
                        self->sh_.ws_closed = true;
                        self->sh_.notify();
                        return;
                    }
                    self->conn_.reset(conn);
                    self->on_open();
                },
                this);
        });
    }

    void send(json msg) {
        {
            std::lock_guard<std::mutex> lk(sh_.mu);
            sh_.capture("out", "signaling", msg);
        }
        loop_.post([this, msg = std::move(msg)] {
            if (!conn_ || soup_websocket_connection_get_state(conn_.get()) != SOUP_WEBSOCKET_STATE_OPEN) {
                logf("signaling: send dropped (socket not open): %s", msg.value("type", "?").c_str());
                return;
            }
            const std::string text = msg.dump();
            soup_websocket_connection_send_text(conn_.get(), text.c_str());
        });
    }

    /// graceful = WebSocket close handshake; otherwise the TCP stream is torn down
    /// underneath the connection (the "socket drop" fault).
    void close(bool graceful) {
        loop_.call_sync([this, graceful] {
            if (!conn_) return;
            if (graceful) {
                if (soup_websocket_connection_get_state(conn_.get()) == SOUP_WEBSOCKET_STATE_OPEN)
                    soup_websocket_connection_close(conn_.get(), SOUP_WEBSOCKET_CLOSE_NORMAL, "opsim-done");
            } else {
                GIOStream* io = soup_websocket_connection_get_io_stream(conn_.get());
                if (io) g_io_stream_close(io, nullptr, nullptr);
            }
        });
    }

  private:
    void on_open() {
        signals_.emplace_back(conn_.get(), "message", G_CALLBACK((+[](SoupWebsocketConnection*, gint type, GBytes* data, gpointer user) {
                                  if (type != SOUP_WEBSOCKET_DATA_TEXT) return;
                                  gsize len = 0;
                                  const char* p = static_cast<const char*>(g_bytes_get_data(data, &len));
                                  static_cast<Signaling*>(user)->on_text(std::string(p, len));
                              })),
                              this);
        signals_.emplace_back(conn_.get(), "closed", G_CALLBACK((+[](SoupWebsocketConnection* c, gpointer user) {
                                  auto* self = static_cast<Signaling*>(user);
                                  const int code = static_cast<int>(soup_websocket_connection_get_close_code(c));
                                  logf("signaling: socket closed (code %d)", code);
                                  std::lock_guard<std::mutex> lk(self->sh_.mu);
                                  self->sh_.ws_closed = true;
                                  self->sh_.ws_close_code = code;
                                  self->sh_.notify();
                              })),
                              this);
        signals_.emplace_back(conn_.get(), "error", G_CALLBACK((+[](SoupWebsocketConnection*, GError* err, gpointer user) {
                                  auto* self = static_cast<Signaling*>(user);
                                  logf("signaling: socket error: %s", err ? err->message : "?");
                                  std::lock_guard<std::mutex> lk(self->sh_.mu);
                                  self->sh_.ws_error = err ? err->message : "error";
                                  self->sh_.notify();
                              })),
                              this);
        soup_websocket_connection_set_max_incoming_payload_size(conn_.get(), 4 * 1024 * 1024);
        soup_websocket_connection_set_keepalive_interval(conn_.get(), 20);
        std::lock_guard<std::mutex> lk(sh_.mu);
        sh_.ws_open = true;
        sh_.notify();
    }

    void on_text(const std::string& text) {
        auto msg = protocol::parse_signaling(text);
        if (!msg) {
            logf("signaling: ignored unparseable message: %.120s", text.c_str());
            return;
        }
        {
            std::lock_guard<std::mutex> lk(sh_.mu);
            sh_.capture("in", "signaling", msg->body);
            sh_.sig_in.push_back(msg->body);
            sh_.notify();
        }
        on_message_(msg->body);
    }

    fjarr::CoreLoop& loop_;
    Shared& sh_;
    std::function<void(const json&)> on_message_;
    glib::GObjectPtr<SoupSession> session_;
    glib::GObjectPtr<SoupWebsocketConnection> conn_;
    std::vector<glib::SignalConnection> signals_;
};

// -------------------------------------------------------------------- peer

/// The answerer: one GstPipeline with a webrtcbin (reuse-source-pads=TRUE,
/// bundle-policy=max-bundle), decode branches per received track, data
/// channels received from the agent. spec: docs/23 "Answerer facts for fjarr-opsim".
class Peer {
  public:
    Peer(fjarr::CoreLoop& loop, Shared& sh, const std::string& ice_policy, const std::optional<json>& turn,
         std::function<void(unsigned, const std::string&)> send_candidate)
        : loop_(loop), sh_(sh), send_candidate_(std::move(send_candidate)) {
        pipeline_ = glib::sink_element(gst_pipeline_new("opsim"));
        webrtc_ = glib::make_element("webrtcbin", "opsim/webrtc");
        if (!webrtc_) throw Abort("webrtcbin element missing (gstreamer1.0-nice?)");
        g_object_set(webrtc_.get(), "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, "latency", 100u, nullptr);
        if (!g_object_class_find_property(G_OBJECT_GET_CLASS(webrtc_.get()), "reuse-source-pads"))
            throw Abort("webrtcbin has no reuse-source-pads (GStreamer >= 1.26 required, docs/23)");
        g_object_set(webrtc_.get(), "reuse-source-pads", TRUE, nullptr);
        if (ice_policy == "relay") g_object_set(webrtc_.get(), "ice-transport-policy", GST_WEBRTC_ICE_TRANSPORT_POLICY_RELAY, nullptr);
        if (turn) add_turn(*turn);
        gst_bin_add(GST_BIN(pipeline_.get()), webrtc_.get()); // the bin takes its own ref

        glib::GstBusPtr bus(gst_pipeline_get_bus(GST_PIPELINE(pipeline_.get())));
        GSource* watch = gst_bus_create_watch(bus.get());
        g_source_set_callback(watch, G_SOURCE_FUNC(&Peer::on_bus), this, nullptr);
        g_source_attach(watch, loop_.context());
        bus_watch_ = glib::SourceGuard::attached(watch);

        GstElement* w = webrtc_.get();
        signals_.emplace_back(w, "on-ice-candidate", G_CALLBACK((+[](GstElement*, guint mline, gchar* c, gpointer d) {
                                  auto* self = static_cast<Peer*>(d);
                                  std::string cand = c ? c : "";
                                  self->loop_.post([self, mline, cand] { self->send_candidate_(mline, cand); });
                              })),
                              this);
        signals_.emplace_back(w, "pad-added", G_CALLBACK((+[](GstElement*, GstPad* pad, gpointer d) { static_cast<Peer*>(d)->on_pad_added(pad); })), this);
        signals_.emplace_back(w, "on-data-channel", G_CALLBACK((+[](GstElement*, GstWebRTCDataChannel* ch, gpointer d) {
                                  static_cast<Peer*>(d)->on_data_channel(ch);
                              })),
                              this);
        for (const char* prop : {"notify::connection-state", "notify::ice-connection-state", "notify::ice-gathering-state"})
            signals_.emplace_back(w, prop, G_CALLBACK(&Peer::on_notify_state), this);
        if (gst_element_set_state(pipeline_.get(), GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE) throw Abort("answerer pipeline refused PLAYING");
    }

    ~Peer() { stop(); }

    void stop() {
        if (stopped_) return;
        stopped_ = true;
        signals_.clear();
        dc_signals_.clear();
        gst_element_set_state(pipeline_.get(), GST_STATE_NULL);
        probes_.clear();
        bus_watch_.cancel();
        channels_.clear();
        tracks_.clear();
    }

    /// set-remote-description(offer) → create-answer → set-local-description; returns the answer SDP.
    std::string answer(const std::string& offer_sdp) {
        GstSDPMessage* msg = nullptr;
        if (gst_sdp_message_new_from_text(offer_sdp.c_str(), &msg) != GST_SDP_OK) throw Abort("offer SDP unparseable");
        glib::SdpPtr offer(gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, msg));
        emit_wait("set-remote-description", offer.get());
        {
            std::lock_guard<std::mutex> lk(cand_mu_);
            remote_described_ = true;
            for (auto& [m, c] : pending_) g_signal_emit_by_name(webrtc_.get(), "add-ice-candidate", m, c.c_str());
            if (!pending_.empty()) vlogf("peer: flushed %zu queued remote candidates", pending_.size());
            pending_.clear();
        }
        auto reply = emit_wait("create-answer", nullptr);
        GstWebRTCSessionDescription* an = nullptr;
        if (reply) gst_structure_get(reply.get(), "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &an, nullptr);
        glib::SdpPtr answer(an);
        if (!answer) {
            std::string why = "create-answer failed";
            if (reply) {
                GError* e = nullptr;
                gst_structure_get(reply.get(), "error", G_TYPE_ERROR, &e, nullptr);
                glib::GErrorPtr err(e);
                if (err) why += ": " + std::string(err->message);
            }
            throw Abort(why);
        }
        emit_wait("set-local-description", answer.get());
        glib::GStrPtr text(gst_sdp_message_as_text(answer->sdp));
        return text.get();
    }

    void add_remote_candidate(unsigned mline, const std::string& cand) {
        if (cand.empty()) return; // end-of-candidates: nothing to add on webrtcbin
        std::lock_guard<std::mutex> lk(cand_mu_);
        if (!remote_described_) {
            pending_.emplace_back(mline, cand);
            return;
        }
        g_signal_emit_by_name(webrtc_.get(), "add-ice-candidate", mline, cand.c_str());
    }

    bool send(const std::string& label, const std::string& text) {
        GstWebRTCDataChannel* dc = nullptr;
        {
            std::lock_guard<std::mutex> lk(dc_mu_);
            auto it = channels_.find(label);
            if (it != channels_.end()) dc = it->second.get();
        }
        if (!dc) return false;
        GError* err = nullptr;
        if (!gst_webrtc_data_channel_send_string_full(dc, text.c_str(), &err)) {
            glib::GErrorPtr e(err);
            logf("peer: send on %s failed: %s", label.c_str(), err ? err->message : "?");
            return false;
        }
        return true;
    }

    /// get-stats → candidate types of the selected pair(s): {local, remote} per pair.
    std::vector<std::pair<std::string, std::string>> selected_candidate_types() {
        auto reply = emit_wait("get-stats", nullptr);
        std::vector<std::pair<std::string, std::string>> out;
        if (!reply) return out;
        std::map<std::string, std::string> cand_type; // id -> candidate-type
        std::vector<std::pair<std::string, std::string>> pairs; // local-id, remote-id
        struct Ctx {
            std::map<std::string, std::string>* types;
            std::vector<std::pair<std::string, std::string>>* pairs;
        } ctx{&cand_type, &pairs};
        gst_structure_foreach(
            reply.get(),
            [](GQuark, const GValue* v, gpointer d) -> gboolean {
                if (!GST_VALUE_HOLDS_STRUCTURE(v)) return TRUE;
                auto* c = static_cast<Ctx*>(d);
                const GstStructure* st = gst_value_get_structure(v);
                gint type = 0;
                gst_structure_get(st, "type", GST_TYPE_WEBRTC_STATS_TYPE, &type, nullptr);
                const gchar* id = gst_structure_get_string(st, "id");
                if (type == GST_WEBRTC_STATS_LOCAL_CANDIDATE || type == GST_WEBRTC_STATS_REMOTE_CANDIDATE) {
                    const gchar* ct = gst_structure_get_string(st, "candidate-type");
                    if (id && ct) (*c->types)[id] = ct;
                } else if (type == GST_WEBRTC_STATS_CANDIDATE_PAIR) {
                    const gchar* l = gst_structure_get_string(st, "local-candidate-id");
                    const gchar* r = gst_structure_get_string(st, "remote-candidate-id");
                    if (l && r) c->pairs->emplace_back(l, r);
                }
                return TRUE;
            },
            &ctx);
        for (auto& [l, r] : pairs) out.emplace_back(cand_type.count(l) ? cand_type[l] : "?", cand_type.count(r) ? cand_type[r] : "?");
        return out;
    }

  private:
    void add_turn(const json& turn) {
        if (!turn.is_object() || !turn.contains("urls")) return;
        const std::string user = turn.value("username", ""), cred = turn.value("credential", "");
        for (const auto& u : turn["urls"]) {
            if (!u.is_string()) continue;
            std::string url = u.get<std::string>();
            const auto pos = url.find("://");
            if (pos == std::string::npos) continue;
            const std::string scheme = url.substr(0, pos);
            if (scheme != "turn" && scheme != "turns") continue;
            glib::GStrPtr eu(g_uri_escape_string(user.c_str(), nullptr, FALSE));
            glib::GStrPtr ep(g_uri_escape_string(cred.c_str(), nullptr, FALSE));
            const std::string full = scheme + "://" + eu.get() + ":" + ep.get() + "@" + url.substr(pos + 3);
            gboolean ok = FALSE;
            g_signal_emit_by_name(webrtc_.get(), "add-turn-server", full.c_str(), &ok);
            logf("peer: add-turn-server %s -> %s", url.c_str(), ok ? "ok" : "rejected");
        }
    }

    glib::GstStructurePtr emit_wait(const char* signal, gpointer arg) {
        auto p = glib::adopt_promise(gst_promise_new());
        g_signal_emit_by_name(webrtc_.get(), signal, arg, p.get());
        gst_promise_wait(p.get());
        const GstStructure* r = gst_promise_get_reply(p.get());
        return glib::GstStructurePtr(r ? gst_structure_copy(r) : nullptr);
    }

    static gboolean on_bus(GstBus*, GstMessage* m, gpointer) {
        if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR || GST_MESSAGE_TYPE(m) == GST_MESSAGE_WARNING) {
            GError* e = nullptr;
            gchar* dbg = nullptr;
            if (GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR) gst_message_parse_error(m, &e, &dbg);
            else gst_message_parse_warning(m, &e, &dbg);
            glib::GErrorPtr err(e);
            glib::GStrPtr debug(dbg);
            logf("peer: BUS %s from %s: %s", GST_MESSAGE_TYPE_NAME(m), GST_OBJECT_NAME(m->src), err ? err->message : "?");
        }
        return TRUE;
    }

    static void on_notify_state(GObject* o, GParamSpec* ps, gpointer d) {
        auto* self = static_cast<Peer*>(d);
        const std::string nick = glib::enum_prop_nick(o, ps->name);
        logf("peer: %s -> %s", ps->name, nick.c_str());
        std::lock_guard<std::mutex> lk(self->sh_.mu);
        if (!std::strcmp(ps->name, "connection-state")) self->sh_.conn_state = nick;
        else if (!std::strcmp(ps->name, "ice-connection-state")) self->sh_.ice_state = nick;
        else if (!std::strcmp(ps->name, "ice-gathering-state")) {
            self->sh_.gathering_state = nick;
            if (nick == "complete") self->loop_.post([self] { self->send_candidate_(0, ""); }); // end-of-candidates (docs/08)
        }
        self->sh_.notify();
    }

    // Streaming thread: fires on the first RTP packet of a transceiver (spike Q3c);
    // pads are src_<n>, the transceiver property gives the mid.
    void on_pad_added(GstPad* pad) {
        if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC) return;
        GstWebRTCRTPTransceiver* tr = nullptr;
        g_object_get(pad, "transceiver", &tr, nullptr);
        glib::GObjectPtr<GstWebRTCRTPTransceiver> trg(tr);
        const std::string mid = tr ? glib::str_prop(tr, "mid") : "";
        const std::string pname = glib::pad_name(pad);
        glib::GstCapsPtr caps(gst_pad_get_current_caps(pad));
        logf("peer: pad-added %s mid=%s caps=%.80s", pname.c_str(), mid.c_str(), glib::caps_to_string(caps.get()).c_str());
        if (mid.empty()) return;
        std::string encoding;
        if (caps)
            if (const gchar* en = gst_structure_get_string(gst_caps_get_structure(caps.get(), 0), "encoding-name")) encoding = en;
        if (encoding != "H264") {
            logf("peer: track mid=%s has encoding %s, not decoded", mid.c_str(), encoding.c_str());
            return;
        }
        int pt = 0;
        if (caps) gst_structure_get_int(gst_caps_get_structure(caps.get(), 0), "payload", &pt);
        auto q = glib::make_element("queue", "rx-" + pname + "-queue");
        auto depay = glib::make_element("rtph264depay", "rx-" + pname + "-depay");
        auto parse = glib::make_element("h264parse", "rx-" + pname + "-parse");
        auto dec = glib::make_element("avdec_h264", "rx-" + pname + "-dec");
        auto conv = glib::make_element("videoconvert", "rx-" + pname + "-conv");
        auto cf = glib::make_element("capsfilter", "rx-" + pname + "-caps");
        auto sink = glib::make_element("fakesink", "rx-" + pname + "-sink");
        if (!q || !depay || !parse || !dec || !conv || !cf || !sink) {
            logf("peer: decode branch elements missing (rtph264depay/h264parse/avdec_h264/videoconvert)");
            return;
        }
        g_object_set(dec.get(), "output-corrupt", FALSE, nullptr); // frames only after a valid keyframe
        glib::GstCapsPtr i420(gst_caps_from_string("video/x-raw,format=I420"));
        g_object_set(cf.get(), "caps", i420.get(), nullptr);
        g_object_set(sink.get(), "sync", FALSE, "async", FALSE, nullptr);
        GstElement* els[] = {q.get(), depay.get(), parse.get(), dec.get(), conv.get(), cf.get(), sink.get()};
        for (GstElement* e : els) gst_bin_add(GST_BIN(pipeline_.get()), e); // the bin takes its own ref
        gst_element_link_many(q.get(), depay.get(), parse.get(), dec.get(), conv.get(), cf.get(), sink.get(), nullptr);
        TrackRx* rx = nullptr;
        {
            std::lock_guard<std::mutex> lk(sh_.mu);
            rx = &sh_.tracks[pname];
            rx->pad = pname;
            rx->tr_mid = mid;
            rx->pt = pt;
            // docs/08: receivers key on transceiver mid. Cross-check with the manifest's pt.
            for (const auto& [id, mpt] : sh_.pt_by_track)
                if (mpt == pt && sh_.mid_by_track[id] != mid)
                    sh_.mid_mismatches.push_back(pname + " pt=" + std::to_string(pt) + " transceiver mid=" + mid + " but manifest v" +
                                                 std::to_string(sh_.manifest_version) + " says " + id + " mid=" + sh_.mid_by_track[id]);
        }
        glib::GstPadPtr parse_src = glib::adopt_pad(gst_element_get_static_pad(parse.get(), "src"));
        probes_.emplace_back(parse_src.get(), GST_PAD_PROBE_TYPE_BUFFER, &Peer::on_encoded, new std::pair<Peer*, TrackRx*>(this, rx),
                             [](gpointer d) { delete static_cast<std::pair<Peer*, TrackRx*>*>(d); });
        glib::GstPadPtr sink_pad = glib::adopt_pad(gst_element_get_static_pad(sink.get(), "sink"));
        probes_.emplace_back(sink_pad.get(), static_cast<GstPadProbeType>(GST_PAD_PROBE_TYPE_BUFFER | GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM),
                             &Peer::on_decoded, new std::pair<Peer*, TrackRx*>(this, rx),
                             [](gpointer d) { delete static_cast<std::pair<Peer*, TrackRx*>*>(d); });
        for (GstElement* e : els) gst_element_sync_state_with_parent(e);
        glib::GstPadPtr qsink = glib::adopt_pad(gst_element_get_static_pad(q.get(), "sink"));
        const GstPadLinkReturn lr = gst_pad_link(pad, qsink.get());
        if (lr != GST_PAD_LINK_OK) logf("peer: linking %s failed (%d)", pname.c_str(), lr);
        // The elements are owned by the pipeline now; drop our census-counted refs.
        for (auto* p : {&q, &depay, &parse, &dec, &conv, &cf, &sink}) p->reset();
        tracks_.push_back(pname);
    }

    static GstPadProbeReturn on_encoded(GstPad*, GstPadProbeInfo* info, gpointer user) {
        auto* p = static_cast<std::pair<Peer*, TrackRx*>*>(user);
        GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
        if (!buf || GST_BUFFER_FLAG_IS_SET(buf, GST_BUFFER_FLAG_DELTA_UNIT)) return GST_PAD_PROBE_OK;
        std::lock_guard<std::mutex> lk(p->first->sh_.mu);
        p->second->keyframes++;
        p->first->sh_.notify();
        return GST_PAD_PROBE_OK;
    }

    static GstPadProbeReturn on_decoded(GstPad* pad, GstPadProbeInfo* info, gpointer user) {
        auto* p = static_cast<std::pair<Peer*, TrackRx*>*>(user);
        Peer* self = p->first;
        TrackRx* rx = p->second;
        if (GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_EVENT_DOWNSTREAM) {
            GstEvent* ev = GST_PAD_PROBE_INFO_EVENT(info);
            if (GST_EVENT_TYPE(ev) == GST_EVENT_CAPS) {
                GstCaps* caps = nullptr;
                gst_event_parse_caps(ev, &caps);
                std::lock_guard<std::mutex> lk(self->sh_.mu);
                rx->have_info = caps && gst_video_info_from_caps(&rx->info, caps);
            }
            return GST_PAD_PROBE_OK;
        }
        if (!(GST_PAD_PROBE_INFO_TYPE(info) & GST_PAD_PROBE_TYPE_BUFFER)) return GST_PAD_PROBE_OK;
        GstBuffer* buf = GST_PAD_PROBE_INFO_BUFFER(info);
        GstVideoInfo vinfo;
        {
            std::lock_guard<std::mutex> lk(self->sh_.mu);
            if (!rx->have_info) {
                glib::GstCapsPtr caps(gst_pad_get_current_caps(pad));
                rx->have_info = caps && gst_video_info_from_caps(&rx->info, caps.get());
                if (!rx->have_info) return GST_PAD_PROBE_OK;
            }
            vinfo = rx->info;
        }
        std::uint32_t counter = 0;
        std::uint64_t ts = 0;
        bool ok = false;
        GstVideoFrame frame;
        if (gst_video_frame_map(&frame, &vinfo, buf, GST_MAP_READ)) {
            ok = decode_stamp(frame, counter, ts);
            gst_video_frame_unmap(&frame);
        }
        const std::int64_t now = g_get_monotonic_time();
        std::lock_guard<std::mutex> lk(self->sh_.mu);
        rx->frames++;
        if (!rx->first_frame_us) rx->first_frame_us = now;
        rx->last_frame_us = now;
        if (!ok) rx->bad_stamps++;
        else {
            if (rx->have_counter && counter > rx->last_counter) {
                const std::uint32_t delta = counter - rx->last_counter;
                if (delta > rx->max_delta) rx->max_delta = delta;
                rx->watch_last = counter;
            }
            rx->have_counter = true;
            rx->last_counter = counter;
            rx->last_stamp_ms = ts;
        }
        self->sh_.notify();
        return GST_PAD_PROBE_OK;
    }

    void on_data_channel(GstWebRTCDataChannel* ch) {
        const std::string label = glib::str_prop(ch, "label");
        gboolean ordered = FALSE;
        gint retransmits = -1;
        g_object_get(ch, "ordered", &ordered, "max-retransmits", &retransmits, nullptr);
        logf("peer: on-data-channel %s ordered=%d max-retransmits=%d", label.c_str(), ordered, retransmits);
        {
            std::lock_guard<std::mutex> lk(dc_mu_);
            channels_[label] = glib::GObjectPtr<GstWebRTCDataChannel>(static_cast<GstWebRTCDataChannel*>(g_object_ref(ch)));
        }
        auto* boxed = new std::pair<Peer*, std::string>(this, label);
        dc_signals_.emplace_back(
            ch, "on-open",
            G_CALLBACK((+[](GstWebRTCDataChannel*, gpointer d) {
                auto* p = static_cast<std::pair<Peer*, std::string>*>(d);
                p->first->mark_open(p->second);
            })),
            boxed, [](gpointer d, GClosure*) { delete static_cast<std::pair<Peer*, std::string>*>(d); });
        dc_signals_.emplace_back(
            ch, "on-message-string",
            G_CALLBACK((+[](GstWebRTCDataChannel*, gchar* s, gpointer d) {
                auto* p = static_cast<std::pair<Peer*, std::string>*>(d);
                p->first->on_dc_text(p->second, s ? s : "");
            })),
            new std::pair<Peer*, std::string>(this, label), [](gpointer d, GClosure*) { delete static_cast<std::pair<Peer*, std::string>*>(d); });
        dc_signals_.emplace_back(ch, "on-error", G_CALLBACK((+[](GstWebRTCDataChannel*, GError* e, gpointer) {
                                     logf("peer: datachannel error: %s", e ? e->message : "?");
                                 })),
                                 nullptr);
        gint state = 0;
        g_object_get(ch, "ready-state", &state, nullptr);
        if (state == GST_WEBRTC_DATA_CHANNEL_STATE_OPEN) mark_open(label);
    }

    void mark_open(const std::string& label) {
        std::lock_guard<std::mutex> lk(sh_.mu);
        if (sh_.channels[label]) return;
        logf("peer: channel %s open", label.c_str());
        sh_.channels[label] = true;
        sh_.notify();
    }

    void on_dc_text(const std::string& label, const std::string& text) {
        auto env = protocol::parse_envelope(text);
        if (!env) {
            logf("peer: dropped invalid envelope on %s: %.100s", label.c_str(), text.c_str());
            return;
        }
        const bool noisy = env->type == "pong" || env->type == "bandwidth-stats";
        if (!noisy || g_verbose) vlogf("<- %s %s/%s %s %s", label.c_str(), env->cap.c_str(), env->type.c_str(), env->kind.c_str(), env->payload.dump().c_str());
        std::lock_guard<std::mutex> lk(sh_.mu);
        sh_.capture("in", label.c_str(), protocol::envelope_to_json(*env));
        if (env->cap == "fjarr.core" && env->type == "pong" && env->kind == "result") {
            const std::int64_t t3 = protocol::now_ms();
            const auto t0 = env->payload.value("t0", 0LL), t1 = env->payload.value("t1", 0LL), t2 = env->payload.value("t2", 0LL);
            const double rtt = static_cast<double>((t3 - t0) - (t2 - t1));
            if (sh_.best_rtt_ms < 0 || rtt < sh_.best_rtt_ms) sh_.best_rtt_ms = rtt;
            sh_.pongs++;
            if (t0 != sh_.last_ping_t0 && sh_.pong_defect.empty())
                sh_.pong_defect = "pong t0=" + std::to_string(t0) + " but the ping sent t0=" + std::to_string(sh_.last_ping_t0) + " (32-bit truncation?)";
        }
        sh_.inbox.push_back(std::move(*env));
        sh_.notify();
    }

    fjarr::CoreLoop& loop_;
    Shared& sh_;
    std::function<void(unsigned, const std::string&)> send_candidate_;
    glib::GstElementPtr pipeline_, webrtc_;
    glib::SourceGuard bus_watch_;
    std::vector<glib::SignalConnection> signals_, dc_signals_;
    std::vector<glib::PadProbe> probes_;
    std::mutex dc_mu_;
    std::map<std::string, glib::GObjectPtr<GstWebRTCDataChannel>> channels_;
    std::mutex cand_mu_;
    bool remote_described_ = false;
    std::vector<std::pair<unsigned, std::string>> pending_;
    std::vector<std::string> tracks_;
    bool stopped_ = false;
};

// ---------------------------------------------------------------- operator

struct Deadline {
    std::chrono::steady_clock::time_point at;
    bool passed() const { return std::chrono::steady_clock::now() >= at; }
};

/// One operator session: signaling + peer + heartbeat + the request/response
/// and waiting primitives the scenarios are written with.
class Operator {
  public:
    Operator(const Options& opts, fjarr::CoreLoop& loop, Shared& sh, Report& report, Deadline deadline)
        : opts_(opts), loop_(loop), sh_(sh), report_(report), deadline_(deadline) {}
    ~Operator() { teardown(); }

    // ------------------------------------------------------------ waiting

    /// Wait until pred() holds (called with the lock held). false on the local
    /// timeout; throws Timeout when the scenario deadline passes.
    bool wait_for(std::function<bool()> pred, int timeout_ms) {
        std::unique_lock<std::mutex> lk(sh_.mu);
        const auto local = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
        const auto until = std::min(local, deadline_.at);
        const bool ok = sh_.cv.wait_until(lk, until, pred);
        if (!ok && deadline_.passed()) throw Timeout();
        return ok;
    }
    void sleep_ms(int ms) { wait_for([] { return false; }, ms); }

    template <class T> T locked(std::function<T()> fn) {
        std::lock_guard<std::mutex> lk(sh_.mu);
        return fn();
    }

    // ---------------------------------------------------------- signaling

    /// Connect the socket and say hello; returns once hello-ack is in (session_id set).
    void open_signaling() {
        signaling_ = std::make_unique<Signaling>(loop_, sh_, [this](const json& m) { on_signaling(m); });
        signaling_->connect(opts_.server);
        if (!wait_for([this] { return sh_.ws_open || sh_.ws_closed; }, 10000) || !sh_.ws_open)
            throw Abort("websocket connect failed: " + locked<std::string>([this] { return sh_.ws_error; }));
        json hello = protocol::signaling_base("hello");
        hello["role"] = "operator";
        hello["auth"] = {{"scheme", "grant"}, {"jwt", mint_grant(opts_.grant_secret, opts_.robot)}};
        hello["client_info"] = {{"client", "fjarr-opsim"}, {"scenario", opts_.scenario}};
        hello["proto_versions"] = {protocol::PROTO_VERSION};
        const std::size_t mark = sig_mark();
        signaling_->send(hello);
        auto ack = wait_signal(mark, "hello-ack", 10000, "error");
        if (!ack) throw Abort("no hello-ack within 10 s");
        if (ack->value("type", "") == "error")
            throw Abort("hello rejected: " + ack->value("code", "?") + " (" + ack->value("message", "") + ")");
        bool turn = false;
        {
            std::lock_guard<std::mutex> lk(sh_.mu);
            sh_.session_id = ack->value("session_id", "");
            if (ack->contains("turn") && ack->at("turn").is_object()) sh_.turn = ack->at("turn");
            turn = sh_.turn.has_value();
        }
        if (session_id().empty()) throw Abort("hello-ack carries no session_id");
        logf("signaling: hello-ack session=%s turn=%s", sid8().c_str(), turn ? "yes" : "no");
    }

    std::size_t sig_mark() { return locked<std::size_t>([this] { return sh_.sig_in.size(); }); }

    /// The first inbound signaling message of `type` (or `alt`) at or after `mark`.
    std::optional<json> wait_signal(std::size_t mark, const std::string& type, int timeout_ms, const std::string& alt = "",
                                    std::function<bool(const json&)> pred = nullptr) {
        std::optional<json> found;
        wait_for(
            [&] {
                for (std::size_t i = mark; i < sh_.sig_in.size(); i++) {
                    const auto& m = sh_.sig_in[i];
                    const std::string t = m.value("type", "");
                    if ((t == type || (!alt.empty() && t == alt)) && (!pred || pred(m))) {
                        found = m;
                        return true;
                    }
                }
                return false;
            },
            timeout_ms);
        return found;
    }

    void send_signal(json m) {
        m["session_id"] = session_id();
        signaling_->send(std::move(m));
    }

    std::string session_id() { return locked<std::string>([this] { return sh_.session_id; }); }
    std::string sid8() { return session_id().substr(0, 8); }

    // ------------------------------------------------------------- session

    /// Full bring-up: socket, hello, peer, first offer answered, connected, heartbeat running.
    void connect() {
        connect_started_us_ = g_get_monotonic_time();
        open_signaling();
        finish_connect();
    }
    /// The rest of connect() after open_signaling(): peer, answer, connected, heartbeat.
    void finish_connect() {
        if (!connect_started_us_) connect_started_us_ = g_get_monotonic_time();
        make_peer();
        const std::size_t mark = 0;
        auto offer = wait_signal(mark, "offer", 15000, "session-close");
        if (!offer) throw Abort("no offer within 15 s of hello");
        if (offer->value("type", "") == "session-close") throw Abort("session closed before the offer: " + offer->value("reason", "?"));
        answer_offer(*offer);
        if (!wait_for([this] { return sh_.conn_state == "connected" && sh_.channels["fjarr:control"]; }, 15000))
            throw Abort("not connected within 15 s (connection-state=" + locked<std::string>([this] { return sh_.conn_state; }) +
                        ", control open=" + (locked<bool>([this] { return sh_.channels["fjarr:control"]; }) ? "yes" : "no") + ")");
        connected_ms_ = static_cast<int>((g_get_monotonic_time() - connect_started_us_) / 1000);
        start_pings();
        std::string tracks;
        for (const auto& [id, mid] : locked<std::map<std::string, std::string>>([this] { return sh_.mid_by_track; })) tracks += id + "(mid=" + mid + ") ";
        logf("connected: session=%s in %d ms, manifest v%u: %s", sid8().c_str(), connected_ms_, manifest_version(), tracks.c_str());
    }

    void make_peer() {
        std::optional<json> turn = locked<std::optional<json>>([this] { return sh_.turn; });
        peer_ = std::make_unique<Peer>(loop_, sh_, opts_.ice_policy, turn, [this](unsigned mline, const std::string& cand) {
            json m = protocol::signaling_base("ice");
            m["session_id"] = session_id();
            m["candidate"] = cand;
            m["sdp_mline_index"] = mline;
            signaling_->send(std::move(m));
        });
        std::vector<std::pair<unsigned, std::string>> early;
        {
            std::lock_guard<std::mutex> lk(early_mu_);
            early.swap(early_candidates_);
            peer_ready_ = true;
        }
        for (auto& [m, c] : early) peer_->add_remote_candidate(m, c);
    }

    /// Apply an offer (initial or renegotiation): record the manifest, answer, send the answer.
    void answer_offer(const json& offer) {
        {
            std::lock_guard<std::mutex> lk(sh_.mu);
            sh_.mid_by_track.clear();
            sh_.pt_by_track.clear();
            for (const auto& t : offer.value("tracks", json::array()))
                if (t.is_object()) {
                    sh_.mid_by_track[t.value("track_id", "")] = t.value("mid", "");
                    sh_.pt_by_track[t.value("track_id", "")] = t.value("pt", 0);
                }
            sh_.manifest_version = offer.value("manifest_version", 0u);
        }
        const std::string sdp = peer_->answer(offer.value("sdp", ""));
        json m = protocol::signaling_base("answer");
        m["session_id"] = session_id();
        m["sdp"] = sdp;
        signaling_->send(std::move(m));
        logf("answered offer manifest_version=%u", manifest_version());
    }

    unsigned manifest_version() { return locked<unsigned>([this] { return sh_.manifest_version; }); }
    int connected_ms() const { return connected_ms_; }

    void start_pings() {
        pings_enabled_ = true;
        send_ping();
        ping_timer_ = loop_.add_timeout(std::chrono::milliseconds(5000), [this] {
            if (pings_enabled_) send_ping();
            return true;
        });
    }
    void stop_pings() { pings_enabled_ = false; }
    std::int64_t last_ping_us() { return locked<std::int64_t>([this] { return sh_.last_ping_us; }); }

    void send_ping() {
        const std::int64_t t0 = protocol::now_ms();
        {
            std::lock_guard<std::mutex> lk(sh_.mu);
            sh_.last_ping_t0 = t0;
            sh_.last_ping_us = g_get_monotonic_time();
        }
        send_envelope("fjarr:control", protocol::make_envelope("fjarr.core", "ping", "request", json{{"t0", t0}}));
    }
    std::string pong_defect() { return locked<std::string>([this] { return sh_.pong_defect; }); }

    /// `drive` events at 20 Hz on the realtime channel (the deadman-armed consumer).
    void start_drive() {
        drive_seq_ = 0;
        drive_timer_ = loop_.add_timeout(std::chrono::milliseconds(50), [this] {
            Envelope e = protocol::make_envelope("fjarr.test", "drive", "event", json{{"v", 1.0}, {"seq", ++drive_seq_}});
            send_envelope("fjarr:realtime", e);
            std::lock_guard<std::mutex> lk(sh_.mu);
            sh_.last_drive_us = g_get_monotonic_time();
            return true;
        });
    }
    void stop_drive() { drive_timer_.cancel(); }
    std::int64_t last_drive_us() { return locked<std::int64_t>([this] { return sh_.last_drive_us; }); }

    bool send_envelope(const std::string& label, const Envelope& e) {
        const bool noisy = e.type == "ping" || e.type == "drive";
        if (!noisy || g_verbose) vlogf("-> %s %s/%s %s %s", label.c_str(), e.cap.c_str(), e.type.c_str(), e.kind.c_str(), e.payload.dump().c_str());
        {
            std::lock_guard<std::mutex> lk(sh_.mu);
            sh_.capture("out", label.c_str(), protocol::envelope_to_json(e));
        }
        return peer_ && peer_->send(label, protocol::serialize_envelope(e));
    }

    std::size_t inbox_mark() { return locked<std::size_t>([this] { return sh_.inbox.size(); }); }

    /// Request on the control channel; the matching result (by event_id) or nullopt on timeout.
    std::optional<Envelope> request(const std::string& cap, const std::string& type, json payload, int timeout_ms = 5000) {
        Envelope e = protocol::make_envelope(cap, type, "request", std::move(payload));
        const std::size_t mark = inbox_mark();
        if (!send_envelope("fjarr:control", e)) return std::nullopt;
        return wait_envelope(mark, cap, type, "result", timeout_ms, [id = e.event_id](const Envelope& r) { return r.event_id == id; });
    }

    std::optional<Envelope> wait_envelope(std::size_t mark, const std::string& cap, const std::string& type, const std::string& kind,
                                          int timeout_ms, std::function<bool(const Envelope&)> pred = nullptr, std::int64_t* seen_us = nullptr) {
        std::optional<Envelope> found;
        wait_for(
            [&] {
                for (std::size_t i = mark; i < sh_.inbox.size(); i++) {
                    const auto& r = sh_.inbox[i];
                    if (r.cap == cap && r.type == type && r.kind == kind && (!pred || pred(r))) {
                        found = r;
                        if (seen_us) *seen_us = g_get_monotonic_time();
                        return true;
                    }
                }
                return false;
            },
            timeout_ms);
        return found;
    }

    /// select-tracks for one track; true when the agent answered ok.
    bool select(const std::string& track_id, bool enabled, const std::string& tier = "active") {
        auto r = request("fjarr.test", "select-tracks", json{{"tracks", json::array({{{"track_id", track_id}, {"enabled", enabled}, {"tier", tier}}})}});
        if (!r) {
            logf("select-tracks %s enabled=%d: no result", track_id.c_str(), enabled);
            return false;
        }
        if (!r->payload.value("ok", false)) logf("select-tracks %s enabled=%d: %s", track_id.c_str(), enabled, r->payload.dump().c_str());
        return r->payload.value("ok", false);
    }

    // -------------------------------------------------------------- frames

    std::uint64_t frames(const std::string& track_id) {
        return locked<std::uint64_t>([&] {
            auto* t = sh_.track(track_id);
            return t ? t->frames : 0;
        });
    }
    std::uint64_t keyframes(const std::string& track_id) {
        return locked<std::uint64_t>([&] {
            auto* t = sh_.track(track_id);
            return t ? t->keyframes : 0;
        });
    }
    std::optional<TrackRx> track_snapshot(const std::string& track_id) {
        return locked<std::optional<TrackRx>>([&]() -> std::optional<TrackRx> {
            auto* t = sh_.track(track_id);
            if (!t) return std::nullopt;
            return *t;
        });
    }
    /// Wait until at least `n` more decoded frames than `since` arrived on the track.
    bool wait_frames(const std::string& track_id, std::uint64_t since, std::uint64_t n, int timeout_ms) {
        return wait_for(
            [&] {
                auto* t = sh_.track(track_id);
                return t && t->frames >= since + n;
            },
            timeout_ms);
    }
    /// Reset the stamp-continuity watch on a track (max_delta measured from here on).
    void watch_reset(const std::string& track_id) {
        std::lock_guard<std::mutex> lk(sh_.mu);
        if (auto* t = sh_.track(track_id)) {
            t->max_delta = 0;
            t->watch_first = t->have_counter ? t->last_counter : 0;
            t->watch_last = t->watch_first;
        }
    }

    // ------------------------------------------------------- introspection

    /// GET <introspect><path>; nullopt without --introspect or on failure.
    std::optional<json> http_get(const std::string& path, std::string* err = nullptr) {
        if (opts_.introspect.empty()) {
            if (err) *err = "--introspect not given";
            return std::nullopt;
        }
        if (!http_) http_.reset(soup_session_new());
        const std::string url = opts_.introspect + path;
        glib::GObjectPtr<SoupMessage> msg(soup_message_new(SOUP_METHOD_GET, url.c_str()));
        if (!msg) {
            if (err) *err = "bad url " + url;
            return std::nullopt;
        }
        GError* e = nullptr;
        glib::GBytesPtr body(soup_session_send_and_read(http_.get(), msg.get(), nullptr, &e));
        if (!body) {
            glib::GErrorPtr g(e);
            if (err) *err = std::string("GET ") + url + ": " + (e ? e->message : "failed");
            return std::nullopt;
        }
        const unsigned status = soup_message_get_status(msg.get());
        gsize len = 0;
        const char* data = static_cast<const char*>(g_bytes_get_data(body.get(), &len));
        if (status != 200) {
            if (err) *err = "GET " + url + ": HTTP " + std::to_string(status);
            return std::nullopt;
        }
        json j = json::parse(std::string(data, len), nullptr, false);
        if (j.is_discarded()) {
            if (err) *err = "GET " + url + ": not JSON";
            return std::nullopt;
        }
        return j;
    }

    /// POST <introspect><path> (no body); true on HTTP 200.
    bool http_post(const std::string& path, std::string* err = nullptr) {
        if (opts_.introspect.empty()) {
            if (err) *err = "--introspect not given";
            return false;
        }
        if (!http_) http_.reset(soup_session_new());
        const std::string url = opts_.introspect + path;
        glib::GObjectPtr<SoupMessage> msg(soup_message_new(SOUP_METHOD_POST, url.c_str()));
        if (!msg) {
            if (err) *err = "bad url " + url;
            return false;
        }
        GError* e = nullptr;
        glib::GBytesPtr body(soup_session_send_and_read(http_.get(), msg.get(), nullptr, &e));
        glib::GErrorPtr g(e);
        const unsigned status = body ? soup_message_get_status(msg.get()) : 0;
        if (status != 200 && err) *err = "POST " + url + ": " + (e ? e->message : "HTTP " + std::to_string(status));
        return status == 200;
    }

    static const json* find_element(const json& elements, const std::string& name) {
        if (!elements.is_array()) return nullptr;
        for (const auto& e : elements) {
            if (e.value("name", "") == name) return &e;
            if (e.contains("children"))
                if (const json* c = find_element(e["children"], name)) return c;
        }
        return nullptr;
    }

    /// The `drop` property of session:<sid8>/<track>/valve, from a snapshot forced now
    /// (the store debounces triggered snapshots by 250 ms; docs/24 POST /snapshot).
    std::optional<bool> valve_drop(const std::string& track_id, std::string* err = nullptr) {
        if (!http_post("/snapshot?pipeline=session:" + session_id(), err)) return std::nullopt;
        auto snap = http_get("/pipelines/session:" + session_id() + ".json", err);
        if (!snap) return std::nullopt;
        const std::string name = "session:" + sid8() + "/" + track_id + "/valve";
        static const json no_elements = json::array();
        const json& elements = snap->contains("elements") ? (*snap)["elements"] : no_elements;
        const json* el = find_element(elements, name);
        if (!el || !el->contains("properties") || !(*el)["properties"].contains("drop")) {
            if (err) *err = "element " + name + " not in the snapshot";
            return std::nullopt;
        }
        return (*el)["properties"]["drop"].get<bool>();
    }

    /// `last_trigger` of the session pipeline in GET /pipelines.
    std::optional<std::string> session_pipeline_trigger(std::string* err = nullptr) {
        auto list = http_get("/pipelines", err);
        if (!list) return std::nullopt;
        for (const auto& p : list->value("pipelines", json::array()))
            if (p.value("id", "") == "session:" + session_id()) return p.value("last_trigger", "");
        if (err) *err = "session:" + sid8() + " not listed";
        return std::nullopt;
    }

    // ------------------------------------------------------------ teardown

    void close_session(const std::string& reason) {
        json m = protocol::signaling_base("session-close");
        m["reason"] = reason;
        send_signal(std::move(m));
    }

    void teardown() {
        ping_timer_.cancel();
        drive_timer_.cancel();
        if (peer_) peer_->stop();
        peer_.reset();
        if (signaling_) signaling_->close(true);
        signaling_.reset();
    }
    void drop_socket() { signaling_->close(false); }
    void reset_for_new_session() {
        teardown();
        std::lock_guard<std::mutex> lk(sh_.mu);
        sh_.ws_open = sh_.ws_closed = false;
        sh_.ws_close_code = 0;
        sh_.ws_error.clear();
        sh_.session_id.clear();
        sh_.turn.reset();
        sh_.conn_state = sh_.ice_state = sh_.gathering_state = "new";
        sh_.channels.clear();
        sh_.tracks.clear();
        sh_.mid_by_track.clear();
        sh_.manifest_version = 0;
        sh_.sig_in.clear();
        sh_.inbox.clear();
        std::lock_guard<std::mutex> lk2(early_mu_);
        early_candidates_.clear();
        peer_ready_ = false;
    }

    /// Lock-free readers for wait_for predicates (the lock is already held there).
    Shared& sh() { return sh_; }
    std::uint64_t frames_unlocked(const std::string& id) {
        auto* t = sh_.track(id);
        return t ? t->frames : 0;
    }
    std::uint64_t keyframes_unlocked(const std::string& id) {
        auto* t = sh_.track(id);
        return t ? t->keyframes : 0;
    }
    /// Frames on the pad that carried `track_id` per an earlier offer's manifest (pt) — survives the track leaving the manifest.
    std::uint64_t frames_by_offer(const json& offer, const std::string& track_id) {
        int pt = -1;
        for (const auto& t : offer.value("tracks", json::array()))
            if (t.value("track_id", "") == track_id) pt = t.value("pt", -1);
        return locked<std::uint64_t>([&] {
            std::uint64_t n = 0;
            for (const auto& [_, t] : sh_.tracks)
                if (t.pt == pt) n += t.frames;
            return n;
        });
    }
    std::vector<std::string> mid_mismatches() { return locked<std::vector<std::string>>([this] { return sh_.mid_mismatches; }); }
    bool has_turn() { return locked<bool>([this] { return sh_.turn.has_value(); }); }
    std::vector<std::string> manifest_tracks() {
        return locked<std::vector<std::string>>([this] {
            std::vector<std::string> v;
            for (const auto& [id, _] : sh_.mid_by_track) v.push_back(id);
            return v;
        });
    }
    bool ws_closed() { return locked<bool>([this] { return sh_.ws_closed; }); }
    int ws_close_code() { return locked<int>([this] { return sh_.ws_close_code; }); }
    double best_rtt() { return locked<double>([this] { return sh_.best_rtt_ms; }); }
    int pongs() { return locked<int>([this] { return sh_.pongs; }); }
    const std::string& introspect() const { return opts_.introspect; }
    std::string conn_state() { return locked<std::string>([this] { return sh_.conn_state; }); }

    Peer* peer() { return peer_.get(); }
    Report& report() { return report_; }

  private:
    // Loop thread: everything besides ICE is consumed by the scenario thread from sig_in.
    void on_signaling(const json& m) {
        const std::string type = m.value("type", "");
        if (type == "ice") {
            const std::string cand = m.value("candidate", "");
            const unsigned mline = m.value("sdp_mline_index", 0u);
            std::lock_guard<std::mutex> lk(early_mu_);
            if (peer_ready_ && peer_) peer_->add_remote_candidate(mline, cand);
            else early_candidates_.emplace_back(mline, cand);
            return;
        }
        if (type == "offer") logf("signaling: offer manifest_version=%u tracks=%zu", m.value("manifest_version", 0u), m.value("tracks", json::array()).size());
        else if (type == "session-close") logf("signaling: session-close reason=%s retry=%s", m.value("reason", "?").c_str(), m.value("retry", false) ? "true" : "false");
        else if (type == "error") logf("signaling: error %s: %s", m.value("code", "?").c_str(), m.value("message", "").c_str());
        else logf("signaling: %s", type.c_str());
    }

    const Options& opts_;
    fjarr::CoreLoop& loop_;
    Shared& sh_;
    Report& report_;
    Deadline deadline_;
    std::unique_ptr<Signaling> signaling_;
    std::unique_ptr<Peer> peer_;
    glib::GObjectPtr<SoupSession> http_;
    glib::SourceGuard ping_timer_, drive_timer_;
    std::atomic<bool> pings_enabled_{false};
    unsigned drive_seq_ = 0;
    std::mutex early_mu_;
    bool peer_ready_ = false;
    std::vector<std::pair<unsigned, std::string>> early_candidates_;
    std::int64_t connect_started_us_ = 0;
    int connected_ms_ = 0;
};


// --------------------------------------------------------------- scenarios

std::string ms_str(std::int64_t us) { return std::to_string(us / 1000) + " ms"; }

/// connect + one assertion line.
void connect_and_report(Operator& op) {
    op.connect();
    op.report().check("connect", true,
                      "session " + op.sid8() + " connected in " + std::to_string(op.connected_ms()) + " ms, manifest v" + std::to_string(op.manifest_version()));
}

/// Enable a track, wait for decoded frames, assert the stamp counter advances.
void stream_and_assert(Operator& op, const std::string& track, const std::string& tier = "active") {
    Report& r = op.report();
    const std::uint64_t before = op.frames(track);
    const std::int64_t t0 = g_get_monotonic_time();
    const bool ok = op.select(track, true, tier);
    const bool first = ok && op.wait_frames(track, before, 1, 8000);
    r.check("first-frame " + track, first,
            first ? "decoded " + ms_str(g_get_monotonic_time() - t0) + " after enable (" + tier + ")" : (ok ? "no decoded frame within 8 s" : "select-tracks failed"));
    if (!first) return;
    auto a = op.track_snapshot(track);
    op.sleep_ms(1000);
    auto b = op.track_snapshot(track);
    const bool advances = a && b && b->have_counter && a->have_counter && b->last_counter > a->last_counter && b->frames - a->frames >= 10;
    r.check("stamp-advances " + track, advances,
            a && b ? "counter " + std::to_string(a->last_counter) + " -> " + std::to_string(b->last_counter) + ", " + std::to_string(b->frames - a->frames) +
                         " frames in 1 s, " + std::to_string(b->bad_stamps) + " undecodable stamps, keyframes " + std::to_string(b->keyframes)
                   : "track not received");
}

/// Orderly close and the two observations that prove it reached the server (and the agent).
void close_and_assert(Operator& op) {
    Report& r = op.report();
    op.close_session("operator-closed");
    const std::int64_t t0 = g_get_monotonic_time();
    const bool closed = op.wait_for([&] { return op.sh().ws_closed; }, 5000);
    r.check("session-close", closed,
            closed ? "server ended the socket " + ms_str(g_get_monotonic_time() - t0) + " after session-close (close code " + std::to_string(op.ws_close_code()) + ")"
                   : "socket still open 5 s after session-close");
    if (!op.introspect().empty()) {
        std::string err;
        std::optional<std::string> trig;
        for (int i = 0; i < 20; i++) {
            trig = op.session_pipeline_trigger(&err);
            if (trig && *trig == "closing") break;
            op.sleep_ms(250);
        }
        r.check("agent-closed", trig && *trig == "closing", trig ? "session pipeline last_trigger=" + *trig : err);
    }
}

void scenario_smoke(Operator& op) {
    Report& r = op.report();
    connect_and_report(op);
    // Every manifest track streams (docs/06: test-pattern always; test-second only when plugged).
    for (const std::string& t : op.manifest_tracks()) stream_and_assert(op, t);
    const json payload{{"n", 42}, {"s", "hi"}, {"nested", {{"k", true}}}};
    auto echo = op.request("fjarr.test", "echo", payload);
    const bool echo_ok = echo && echo->payload.value("ok", false) && echo->payload.value("echo", json()) == payload && echo->payload.contains("t_agent");
    r.check("echo", echo_ok, echo ? "result " + echo->payload.dump() : "no result within 5 s");
    // Heartbeat: at least one pong, echoing t0 exactly (docs/08#fjarr-core).
    op.wait_for([&] { return op.sh().pongs > 0; }, 6000);
    const std::string defect = op.pong_defect();
    r.check("heartbeat", op.pongs() > 0 && defect.empty(),
            op.pongs() > 0 ? (defect.empty() ? "pong echoes t0; best rtt " + std::to_string(static_cast<int>(op.best_rtt())) + " ms over " + std::to_string(op.pongs()) + " pong(s)" : defect)
                           : "no pong within 6 s");
    close_and_assert(op);
}

void scenario_relay_only(Operator& op) {
    Report& r = op.report();
    op.open_signaling();
    r.check("turn-credentials", op.has_turn(), op.has_turn() ? "hello-ack carried TURN credentials" : "hello-ack carried no TURN credentials (fjarr-server FJARR_TURN_URLS unset / coturn profile down); relay-only cannot connect");
    if (!op.has_turn()) return;
    op.finish_connect();
    r.check("connect", true, "session " + op.sid8() + " connected in " + std::to_string(op.connected_ms()) + " ms with ice-transport-policy=relay");
    auto pairs = op.peer()->selected_candidate_types();
    std::string detail;
    bool all_relay = !pairs.empty();
    for (auto& [l, rm] : pairs) {
        detail += "local=" + l + " remote=" + rm + "; ";
        all_relay = all_relay && l == "relay" && rm == "relay";
    }
    if (pairs.empty()) detail = "no candidate-pair in get-stats ";
    r.check("relay-candidates", all_relay, detail + "(ice-transport-policy=relay on this side; the agent side needs FJARR_ICE_POLICY=relay)");
    for (const std::string& t : op.manifest_tracks()) stream_and_assert(op, t);
    close_and_assert(op);
}

void scenario_toggle(Operator& op) {
    Report& r = op.report();
    connect_and_report(op);
    const std::string track = "test-pattern";
    const int N = 20;
    int keyframe_ok = 0, valve_ok = 0, silent_ok = 0, results_ok = 0;
    std::int64_t worst_kf_us = 0;
    std::string valve_err, silent_detail;
    for (int i = 0; i < N; i++) {
        const std::uint64_t kf_before = op.keyframes(track), f_before = op.frames(track);
        const std::int64_t t0 = g_get_monotonic_time();
        const bool on = op.select(track, true);
        if (!on) continue;
        results_ok++;
        const bool kf = op.wait_for([&] { return op.keyframes_unlocked(track) > kf_before && op.frames_unlocked(track) > f_before; }, 4000);
        const std::int64_t took = g_get_monotonic_time() - t0;
        if (kf) {
            keyframe_ok++;
            worst_kf_us = std::max(worst_kf_us, took);
        }
        std::string err;
        auto drop = op.valve_drop(track, &err);
        const bool open = drop && !*drop;
        if (!open && valve_err.empty()) valve_err = "enable #" + std::to_string(i + 1) + ": " + (drop ? "drop=true" : err);
        const bool off = op.select(track, false);
        if (!off) continue;
        results_ok++;
        auto drop2 = op.valve_drop(track, &err);
        const bool closed = drop2 && *drop2;
        if (!closed && valve_err.empty()) valve_err = "disable #" + std::to_string(i + 1) + ": " + (drop2 ? "drop=false" : err);
        if (open && closed) valve_ok++;
        op.sleep_ms(300); // in-flight packets and the jitter buffer drain
        const std::uint64_t f0 = op.frames(track);
        op.sleep_ms(500);
        const std::uint64_t f1 = op.frames(track);
        if (f1 == f0) silent_ok++;
        else if (silent_detail.empty()) silent_detail = "disable #" + std::to_string(i + 1) + ": " + std::to_string(f1 - f0) + " frames decoded 300-800 ms after disable";
    }
    r.check("toggle-results", results_ok == 2 * N, std::to_string(results_ok) + "/" + std::to_string(2 * N) + " select-tracks results ok");
    r.check("toggle-keyframe", keyframe_ok == N,
            std::to_string(keyframe_ok) + "/" + std::to_string(N) + " enables produced a keyframe + decoded frame, worst " + ms_str(worst_kf_us));
    if (op.introspect().empty()) r.check("toggle-valve", false, "--introspect not given: cannot read session:<sid8>/test-pattern/valve");
    else r.check("toggle-valve", valve_ok == N, std::to_string(valve_ok) + "/" + std::to_string(N) + " cycles matched valve drop state via /pipelines" + (valve_err.empty() ? "" : "; first mismatch: " + valve_err));
    r.check("toggle-silent", silent_ok == N, std::to_string(silent_ok) + "/" + std::to_string(N) + " disables produced no frames" + (silent_detail.empty() ? "" : "; " + silent_detail));
    close_and_assert(op);
}

void scenario_hotplug(Operator& op) {
    Report& r = op.report();
    connect_and_report(op);
    stream_and_assert(op, "test-pattern");
    op.watch_reset("test-pattern");
    const std::uint64_t pattern_frames_start = op.frames("test-pattern");

    // plug
    std::size_t mark = op.sig_mark();
    auto plug = op.request("fjarr.test", "hotplug", json{{"plugged", true}});
    r.check("hotplug-plug-result", plug && plug->payload.value("ok", false) && plug->payload.value("manifest_version", 0u) == 2,
            plug ? plug->payload.dump() : "no result within 5 s");
    auto offer2 = op.wait_signal(mark, "offer", 8000);
    bool has_second = false;
    if (offer2)
        for (const auto& t : offer2->value("tracks", json::array())) has_second = has_second || t.value("track_id", "") == "test-second";
    r.check("hotplug-reoffer-v2", offer2 && offer2->value("manifest_version", 0u) == 2 && has_second,
            offer2 ? "offer manifest_version=" + std::to_string(offer2->value("manifest_version", 0u)) + " tracks=" + std::to_string(offer2->value("tracks", json::array()).size()) +
                         (has_second ? " incl. test-second" : " without test-second")
                   : "no re-offer within 8 s");
    if (!offer2) return;
    op.answer_offer(*offer2);
    {
        const std::uint64_t before = op.frames("test-second");
        const std::int64_t t0 = g_get_monotonic_time();
        const bool ok = op.select("test-second", true);
        const bool flows = ok && op.wait_frames("test-second", before, 10, 8000);
        r.check("test-second-flows", flows, flows ? "10 decoded frames within " + ms_str(g_get_monotonic_time() - t0) + " of enable" : (ok ? "no frames within 8 s" : "select-tracks failed"));
        auto mm = op.mid_mismatches();
        std::string detail;
        for (const auto& m : mm) detail += m + "; ";
        r.check("track-mapping", mm.empty(),
                mm.empty() ? "every received pad's transceiver mid matches the manifest"
                           : detail + "(the offer carries no a=ssrc for the new m-section, so a bundled webrtcbin answerer routes the unsignaled ssrc to m-line 0; opsim fell back to pt)");
    }

    // unplug
    mark = op.sig_mark();
    auto unplug = op.request("fjarr.test", "hotplug", json{{"plugged", false}});
    r.check("hotplug-unplug-result", unplug && unplug->payload.value("ok", false) && unplug->payload.value("manifest_version", 0u) == 3,
            unplug ? unplug->payload.dump() : "no result within 5 s");
    auto offer3 = op.wait_signal(mark, "offer", 8000);
    has_second = false;
    if (offer3)
        for (const auto& t : offer3->value("tracks", json::array())) has_second = has_second || t.value("track_id", "") == "test-second";
    r.check("hotplug-reoffer-v3", offer3 && offer3->value("manifest_version", 0u) == 3 && !has_second,
            offer3 ? "offer manifest_version=" + std::to_string(offer3->value("manifest_version", 0u)) + " tracks=" + std::to_string(offer3->value("tracks", json::array()).size()) +
                         (has_second ? " still lists test-second" : " without test-second")
                   : "no re-offer within 8 s");
    if (!offer3) return;
    // The manifest no longer names test-second; keep watching its mid through the pad we already have.
    const std::uint64_t second_frames_at_unplug = op.frames_by_offer(offer2, "test-second");
    op.answer_offer(*offer3);
    op.sleep_ms(800);
    const std::uint64_t f0 = op.frames_by_offer(offer2, "test-second");
    op.sleep_ms(1000);
    const std::uint64_t f1 = op.frames_by_offer(offer2, "test-second");
    r.check("test-second-disappears", f1 == f0,
            std::to_string(f1 - f0) + " frames decoded in the 1 s window starting 800 ms after the v3 answer (" + std::to_string(f0 - second_frames_at_unplug) + " arrived in between)");

    auto t = op.track_snapshot("test-pattern");
    const bool cont = t && t->max_delta <= 2 && t->frames > pattern_frames_start + 30;
    r.check("test-pattern-continuity", cont,
            t ? "max stamp gap " + std::to_string(t->max_delta ? t->max_delta - 1 : 0) + " frame(s) across both renegotiations (" + std::to_string(t->frames - pattern_frames_start) +
                    " frames decoded, " + std::to_string(t->bad_stamps) + " undecodable stamps)"
              : "test-pattern not received");
    close_and_assert(op);
}

void scenario_silent_operator(Operator& op) {
    Report& r = op.report();
    connect_and_report(op);
    stream_and_assert(op, "test-pattern");
    std::size_t imark = op.inbox_mark();
    op.start_drive();
    auto armed = op.wait_envelope(imark, "fjarr.test", "deadman", "event", 3000, [](const Envelope& e) { return e.payload.value("state", "") == "armed"; });
    r.check("deadman-armed", armed.has_value(), armed ? "deadman armed by drive" : "no deadman{armed} within 3 s of driving");
    imark = op.inbox_mark();
    const std::size_t smark = op.sig_mark();
    op.stop_pings();
    const std::int64_t last_ping = op.last_ping_us();
    logf("pings stopped (last ping %s ago); waiting for the agent's liveness budget", ms_str(g_get_monotonic_time() - last_ping).c_str());
    auto close = op.wait_signal(smark, "session-close", 25000, "peer-gone");
    const std::int64_t t_close = g_get_monotonic_time();
    const std::int64_t since_ping_ms = (t_close - last_ping) / 1000;
    const bool ok = close && close->value("type", "") == "session-close" && close->value("reason", "") == "heartbeat" && since_ping_ms >= 15000 && since_ping_ms <= 20000;
    r.check("heartbeat-close", ok,
            close ? close->value("type", "?") + " reason=" + close->value("reason", "?") + " " + std::to_string(since_ping_ms) + " ms after the last ping (budget 15-20 s)"
                  : "no session-close within 25 s of the last ping");
    std::int64_t t_exp = 0;
    auto expired = op.wait_envelope(imark, "fjarr.test", "deadman", "event", 2000, [](const Envelope& e) { return e.payload.value("state", "") == "expired"; }, &t_exp);
    op.stop_drive();
    r.check("release-all-input", expired.has_value(),
            expired ? "deadman{expired} received (ms_since_feed=" + std::to_string(expired->payload.value("ms_since_feed", -1)) + ") while drive kept feeding at 20 Hz"
                    : "no deadman{expired} event seen around the heartbeat close (release_all_input not observed)");
}

void scenario_no_answer(Operator& op) {
    Report& r = op.report();
    op.open_signaling();
    op.make_peer();
    auto offer = op.wait_signal(0, "offer", 15000);
    r.check("offer-received", offer.has_value(), offer ? "offer manifest_version=" + std::to_string(offer->value("manifest_version", 0u)) + "; never answering" : "no offer within 15 s");
    if (!offer) return;
    const std::int64_t t_offer = g_get_monotonic_time();
    const std::size_t smark = op.sig_mark();
    auto close = op.wait_signal(smark, "session-close", 25000, "peer-gone");
    const std::int64_t since_ms = (g_get_monotonic_time() - t_offer) / 1000;
    const std::string reason = close ? close->value("reason", "?") : "";
    const bool ok = close && close->value("type", "") == "session-close" && reason == "negotiation-timeout:offer-created" && since_ms >= 14000 && since_ms <= 17000;
    r.check("watchdog-close", ok,
            close ? close->value("type", "?") + " reason=" + reason + " " + std::to_string(since_ms) + " ms after the offer (expected negotiation-timeout:offer-created at 15 s)"
                  : "no session-close within 25 s of the unanswered offer");
}

void scenario_socket_drop(Operator& op) {
    Report& r = op.report();
    connect_and_report(op);
    stream_and_assert(op, "test-pattern");
    const std::int64_t t0 = g_get_monotonic_time();
    op.drop_socket();
    logf("operator socket dropped (TCP close underneath the WebSocket)");
    if (op.introspect().empty()) r.check("agent-closed", false, "--introspect not given: cannot observe the agent's session teardown");
    else {
        std::string err;
        std::optional<std::string> trig;
        for (int i = 0; i < 40; i++) {
            trig = op.session_pipeline_trigger(&err);
            if (trig && *trig == "closing") break;
            op.sleep_ms(250);
        }
        r.check("agent-closed", trig && *trig == "closing",
                trig && *trig == "closing" ? "session pipeline reached last_trigger=closing " + ms_str(g_get_monotonic_time() - t0) + " after the drop (peer-gone)"
                                           : (trig ? "session pipeline last_trigger=" + *trig + " 10 s after the drop" : err));
    }
    op.sleep_ms(1000);
    const std::uint64_t f0 = op.frames("test-pattern");
    op.sleep_ms(1000);
    const std::uint64_t f1 = op.frames("test-pattern");
    r.check("media-stopped", f1 == f0, std::to_string(f1 - f0) + " frames decoded in the 1 s window starting 1 s after the drop (connection-state=" + op.conn_state() + ")");
    if (!op.introspect().empty()) {
        // Census baseline (/memory) is slice 3c; the session must at least be gone from the live list of pipelines
        // or retired (last_trigger=closing). Producers stay: they are shared and idle.
        auto list = op.http_get("/pipelines");
        int live_sessions = 0;
        if (list)
            for (const auto& p : list->value("pipelines", json::array()))
                if (p.value("kind", "") == "session" && p.value("last_trigger", "") != "closing") live_sessions++;
        r.check("census-baseline", list && live_sessions == 0, list ? std::to_string(live_sessions) + " session pipeline(s) not closing (/memory census is slice 3c)" : "GET /pipelines failed");
    }
}

void scenario_ice_restart(Operator& op) {
    Report& r = op.report();
    connect_and_report(op);
    stream_and_assert(op, "test-pattern");
    const std::string first = op.session_id();
    const std::size_t smark = op.sig_mark();
    const std::int64_t t0 = g_get_monotonic_time();
    op.send_signal(protocol::signaling_base("ice-restart"));
    auto close = op.wait_signal(smark, "session-close", 5000);
    const std::int64_t took_us = g_get_monotonic_time() - t0;
    const bool ok = close && close->value("reason", "") == "ice-restart" && close->value("retry", false) && took_us <= 100000;
    r.check("ice-restart-close", ok,
            close ? "session-close reason=" + close->value("reason", "?") + " retry=" + (close->value("retry", false) ? "true" : "false") + " after " + std::to_string(took_us / 1000) +
                        " ms (budget 100 ms)"
                  : "no session-close within 5 s of ice-restart");
    // Rung 3: a new signaling round with the same grant, immediately.
    op.reset_for_new_session();
    op.connect();
    r.check("new-session", op.session_id() != first, "session " + op.sid8() + " brokered after " + first.substr(0, 8) + ", connected in " + std::to_string(op.connected_ms()) + " ms");
    stream_and_assert(op, "test-pattern");
    close_and_assert(op);
}

void scenario_deadman(Operator& op) {
    Report& r = op.report();
    connect_and_report(op);
    std::size_t imark = op.inbox_mark();
    op.start_drive();
    auto armed = op.wait_envelope(imark, "fjarr.test", "deadman", "event", 3000, [](const Envelope& e) { return e.payload.value("state", "") == "armed"; });
    r.check("deadman-armed", armed.has_value(), armed ? "armed on the first drive" : "no deadman{armed} within 3 s of driving");
    op.sleep_ms(1000);
    imark = op.inbox_mark();
    op.stop_drive();
    op.sleep_ms(60); // let a dispatch in flight finish so last_drive_us is final
    const std::int64_t last_drive = op.last_drive_us();
    std::int64_t t_exp = 0;
    auto expired = op.wait_envelope(imark, "fjarr.test", "deadman", "event", 3000, [](const Envelope& e) { return e.payload.value("state", "") == "expired"; }, &t_exp);
    const std::int64_t since_ms = expired ? (t_exp - last_drive) / 1000 : -1;
    r.check("deadman-expired", expired && since_ms <= 600,
            expired ? "deadman{expired} " + std::to_string(since_ms) + " ms after the last drive (budget 600 ms; agent ms_since_feed=" + std::to_string(expired->payload.value("ms_since_feed", -1)) + ")"
                    : "no deadman{expired} within 3 s of the last drive");
    imark = op.inbox_mark();
    op.start_drive();
    auto fed = op.wait_envelope(imark, "fjarr.test", "deadman", "event", 3000, [](const Envelope& e) { return e.payload.value("state", "") == "fed"; });
    op.stop_drive();
    r.check("deadman-fed", fed.has_value(), fed ? "deadman{fed} on the next drive after expiry" : "no deadman{fed} within 3 s of resuming drive");
    close_and_assert(op);
}

// -------------------------------------------------------------------- main

using ScenarioFn = void (*)(Operator&);
const std::map<std::string, ScenarioFn> kScenarios = {
    {"smoke", scenario_smoke},       {"toggle", scenario_toggle},   {"hotplug", scenario_hotplug},     {"silent-operator", scenario_silent_operator},
    {"no-answer", scenario_no_answer}, {"socket-drop", scenario_socket_drop}, {"ice-restart", scenario_ice_restart}, {"deadman", scenario_deadman},
    {"relay-only", scenario_relay_only},
};

} // namespace

int main(int argc, char** argv) {
    Options opts;
    if (!parse_args(argc, argv, opts)) {
        usage();
        return 2;
    }
    g_verbose = opts.verbose;
    if (opts.scenario == "soak" || opts.scenario.rfind("netem-", 0) == 0) {
        std::printf("FAIL %s: not in this slice (3c)\n", opts.scenario.c_str());
        return 3;
    }
    auto it = kScenarios.find(opts.scenario);
    if (it == kScenarios.end()) {
        std::fprintf(stderr, "unknown scenario %s\n", opts.scenario.c_str());
        usage();
        return 2;
    }
    if (opts.scenario == "relay-only") opts.ice_policy = "relay";
    gst_init(&argc, &argv);

    Shared sh;
    Report report;
    fjarr::CoreLoop loop;
    loop.start();
    const Deadline deadline{std::chrono::steady_clock::now() + std::chrono::seconds(opts.timeout_s)};
    // Hard watchdog: a wedged teardown must still exit 3.
    std::atomic<bool> finished{false};
    std::thread([&finished, deadline] {
        while (!finished) {
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            if (std::chrono::steady_clock::now() > deadline.at + std::chrono::seconds(10)) {
                std::fprintf(stderr, "fjarr-opsim: hard timeout, exiting 3\n");
                std::_Exit(3);
            }
        }
    }).detach();

    int code = 0;
    logf("scenario %s: robot=%s server=%s ice-policy=%s timeout=%ds", opts.scenario.c_str(), opts.robot.c_str(), opts.server.c_str(), opts.ice_policy.c_str(), opts.timeout_s);
    {
        Operator op(opts, loop, sh, report, deadline);
        try {
            it->second(op);
        } catch (const Timeout&) {
            report.check(opts.scenario + "-timeout", false, "scenario exceeded --timeout " + std::to_string(opts.timeout_s) + " s");
            code = 3;
        } catch (const Abort& e) {
            report.check(opts.scenario + "-abort", false, e.what());
        } catch (const std::exception& e) {
            report.check(opts.scenario + "-exception", false, e.what());
        }
        op.teardown();
    }
    if (code == 0) code = report.failed() ? 1 : 0;
    const int passed = static_cast<int>(report.results.size()) - report.failed();
    std::printf("SUMMARY %s: %d passed, %d failed%s\n", opts.scenario.c_str(), passed, report.failed(), code == 3 ? ", timeout" : "");
    std::fflush(stdout);
    if (!opts.json_out.empty()) {
        json out{{"scenario", opts.scenario}, {"robot", opts.robot}, {"server", opts.server}, {"ice_policy", opts.ice_policy}, {"exit_code", code}};
        json results = json::array();
        for (const auto& r : report.results) results.push_back({{"name", r.name}, {"pass", r.pass}, {"detail", r.detail}});
        out["results"] = results;
        json cap = json::array();
        {
            std::lock_guard<std::mutex> lk(sh.mu);
            for (const auto& c : sh.captured) cap.push_back({{"t", c.t}, {"dir", c.dir}, {"channel", c.channel}, {"body", c.body}});
        }
        out["captured"] = cap;
        std::ofstream f(opts.json_out);
        f << out.dump(2) << "\n";
    }
    loop.stop();
    finished = true;
    return code;
}
