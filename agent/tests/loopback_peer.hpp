// A browser's side of a session, reduced to what the agent tests need: a webrtcbin that answers
// every offer (accepting new m-lines with recvonly transceivers, or — without BUNDLE — rejecting the
// bundle-only ones), trickles its candidates through `on_candidate`, and hands the agent's data
// channels to `on_channel` as they are announced.
#pragma once

#include <functional>
#include <string>

#include <gst/gst.h>
#include <gst/sdp/sdp.h>
#include <gst/webrtc/webrtc.h>

namespace fjarr::testing {

/// The browser's side, reduced to what negotiation needs: a webrtcbin that answers every offer,
/// accepting each new m-line with a recvonly transceiver (or, with `bundle=false`, rejecting the
/// bundle-only ones as a peer without BUNDLE does), and trickles its candidates back.
struct LoopbackPeer {
    std::function<void(GstWebRTCDataChannel*)> on_channel;
    GstElement* pipe = gst_pipeline_new("loopback-peer");
    GstElement* wb = gst_element_factory_make("webrtcbin", "loopback-peer-wb");
    std::function<void(unsigned, std::string)> on_candidate;

    explicit LoopbackPeer(bool bundle) {
        if (bundle) g_object_set(wb, "bundle-policy", GST_WEBRTC_BUNDLE_POLICY_MAX_BUNDLE, nullptr);
        g_object_set(wb, "reuse-source-pads", TRUE, nullptr); // docs/23: every webrtcbin answerer sets it
        gst_bin_add(GST_BIN(pipe), wb);
        g_signal_connect(wb, "on-ice-candidate", G_CALLBACK(+[](GstElement*, guint mline, gchar* cand, gpointer self) {
                             auto* me = static_cast<LoopbackPeer*>(self);
                             if (me->on_candidate) me->on_candidate(mline, cand);
                         }),
                         this);
        g_signal_connect(wb, "on-data-channel", G_CALLBACK(+[](GstElement*, GstWebRTCDataChannel* dc, gpointer self) {
                             auto* me = static_cast<LoopbackPeer*>(self);
                             if (me->on_channel) me->on_channel(dc);
                         }),
                         this);
        g_signal_connect(wb, "pad-added", G_CALLBACK(+[](GstElement*, GstPad* pad, gpointer bin) {
                             if (GST_PAD_DIRECTION(pad) != GST_PAD_SRC) return;
                             GstElement* sink = gst_element_factory_make("fakesink", nullptr);
                             g_object_set(sink, "async", FALSE, "sync", FALSE, nullptr);
                             gst_bin_add(GST_BIN(bin), sink);
                             gst_element_sync_state_with_parent(sink);
                             GstPad* sp = gst_element_get_static_pad(sink, "sink");
                             gst_pad_link(pad, sp);
                             gst_object_unref(sp);
                         }),
                         pipe);
        gst_element_set_state(pipe, GST_STATE_PLAYING);
    }
    ~LoopbackPeer() {
        gst_element_set_state(pipe, GST_STATE_NULL);
        gst_object_unref(pipe);
    }
    LoopbackPeer(const LoopbackPeer&) = delete;
    LoopbackPeer& operator=(const LoopbackPeer&) = delete;

    std::string answer(const std::string& offer_text) {
        GstSDPMessage* sdp = nullptr;
        if (gst_sdp_message_new_from_text(offer_text.c_str(), &sdp) != GST_SDP_OK) return "";
        GArray* have = nullptr;
        g_signal_emit_by_name(wb, "get-transceivers", &have);
        const guint n_have = have ? have->len : 0;
        if (have) g_array_unref(have);
        for (guint i = n_have; i < gst_sdp_message_medias_len(sdp); i++) {
            GstCaps* caps = gst_caps_from_string("application/x-rtp,media=video,encoding-name=H264,clock-rate=90000");
            GstWebRTCRTPTransceiver* tr = nullptr;
            g_signal_emit_by_name(wb, "add-transceiver", GST_WEBRTC_RTP_TRANSCEIVER_DIRECTION_RECVONLY, caps, &tr);
            gst_caps_unref(caps);
            if (tr) gst_object_unref(tr);
        }
        GstWebRTCSessionDescription* offer = gst_webrtc_session_description_new(GST_WEBRTC_SDP_TYPE_OFFER, sdp);
        GstPromise* p = gst_promise_new();
        g_signal_emit_by_name(wb, "set-remote-description", offer, p);
        gst_promise_wait(p);
        gst_promise_unref(p);
        gst_webrtc_session_description_free(offer);
        p = gst_promise_new();
        g_signal_emit_by_name(wb, "create-answer", nullptr, p);
        gst_promise_wait(p);
        GstWebRTCSessionDescription* ans = nullptr;
        if (const GstStructure* reply = gst_promise_get_reply(p)) gst_structure_get(reply, "answer", GST_TYPE_WEBRTC_SESSION_DESCRIPTION, &ans, nullptr);
        gst_promise_unref(p);
        if (!ans) return "";
        p = gst_promise_new();
        g_signal_emit_by_name(wb, "set-local-description", ans, p);
        gst_promise_wait(p);
        gst_promise_unref(p);
        gchar* text = gst_sdp_message_as_text(ans->sdp);
        std::string out = text;
        g_free(text);
        gst_webrtc_session_description_free(ans);
        return out;
    }
};

} // namespace fjarr::testing
