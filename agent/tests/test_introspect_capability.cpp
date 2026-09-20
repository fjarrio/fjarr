// fjarr.introspect over a recording context (docs/24 message table, docs/23
// slice 5a gate 3): subscribe replays the latest with txt inline and json/dot
// as blob references; newest-wins per pipeline while blobs are in flight;
// history and one snapshot by seq; a session ending mid-blob.
#include <gtest/gtest.h>

#include <gst/gst.h>

#include <fjarr/blob.hpp>
#include <fjarr/errors.hpp>

#include "capabilities/introspect_capability.hpp"
#include "core/loop.hpp"
#include "recording_context.hpp"

using namespace fjarr;
using fjarr::testing::RecordingContext;

namespace {
struct Fixture {
    CoreLoop loop;
    introspect::SnapshotStore store{8};
    capabilities::IntrospectCapability cap{[this] { return &store; }, [] { return nlohmann::json{{"encoder", "software"}}; }};
    glib::GstElementPtr pipeline;
    Fixture() {
        if (!gst_is_initialized()) gst_init(nullptr, nullptr);
        pipeline = glib::GstElementPtr(gst_parse_launch("fakesrc name=src ! fakesink name=sink", nullptr));
        gst_element_set_name(pipeline.get(), "producer:pat:active");
        loop.start();
    }
    ~Fixture() { loop.stop(); }
    void take(const std::string &trigger = "state-changed") {
        loop.call_sync([&] {
            introspect::SnapshotMeta meta;
            meta.pipeline_id = "producer:pat:active";
            meta.kind = "producer";
            meta.trigger = trigger;
            store.take(GST_BIN(pipeline.get()), meta, true);
        });
    }
    Envelope request(const std::string &type, nlohmann::json payload = nlohmann::json::object()) {
        Envelope e;
        e.cap = "fjarr.introspect";
        e.type = type;
        e.kind = "request";
        e.event_id = "evt-" + type;
        e.payload = std::move(payload);
        return e;
    }
};
} // namespace

TEST(IntrospectCapability, subscribeRepliesWithTheListAndReplaysTheLatestAsTxtInlinePlusBlobReferences) {
    Fixture f;
    RecordingContext ctx;
    f.take();
    f.take("renegotiation");
    f.loop.call_sync([&] {
        f.cap.session_attached(ctx, nlohmann::json::object());
        f.cap.on_message(ctx, f.request("pipelines/list"));
    });
    ASSERT_TRUE(ctx.last_result());
    EXPECT_EQ(ctx.last_result()->payload["pipelines"].size(), 1u);
    EXPECT_EQ(ctx.last_result()->payload["pipelines"][0]["pipeline_id"], "producer:pat:active");
    EXPECT_EQ(ctx.last_result()->payload["pipelines"][0]["seq"], 2);

    f.loop.call_sync([&] {
        f.cap.on_message(ctx, f.request("pipelines/subscribe", nlohmann::json{{"pipeline_id", "*"}, {"forms", {"txt", "json", "dot"}}}));
    });
    const auto events = ctx.events("snapshot");
    ASSERT_EQ(events.size(), 1u); // the latest of the one pipeline, replayed
    const auto &ev = events[0];
    EXPECT_EQ(ev["pipeline_id"], "producer:pat:active");
    EXPECT_EQ(ev["seq"], 2);
    EXPECT_EQ(ev["trigger"], "renegotiation");
    EXPECT_TRUE(ev["txt"].is_string()); // inline
    ASSERT_EQ(ctx.blobs.size(), 2u);    // json and dot ride the bulk channel
    const auto json_ref = blob::BlobRef::from_json(ev["json"]);
    const auto dot_ref = blob::BlobRef::from_json(ev["dot"]);
    ASSERT_TRUE(json_ref && dot_ref);
    EXPECT_EQ(json_ref->type, "application/json");
    EXPECT_EQ(dot_ref->type, "text/vnd.graphviz");
    const auto latest = f.store.latest("producer:pat:active");
    EXPECT_EQ(ctx.blobs[0].bytes, latest->json);
    EXPECT_EQ(ctx.blobs[1].bytes, latest->dot);
    EXPECT_EQ(json_ref->len, latest->json.size());
    EXPECT_EQ(dot_ref->id, ctx.blobs[1].ref.id);
    EXPECT_NE(ctx.blobs[1].bytes.find("digraph"), std::string::npos);

    // a new snapshot reaches the subscriber as one more event
    f.take("select-tracks");
    EXPECT_EQ(ctx.events("snapshot").size(), 2u);
    EXPECT_EQ(ctx.events("snapshot")[1]["seq"], 3);

    // history and one snapshot by seq
    f.loop.call_sync([&] {
        f.cap.on_message(ctx, f.request("pipelines/history", nlohmann::json{{"pipeline_id", "producer:pat:active"}, {"seq_from", 2}}));
    });
    EXPECT_EQ(ctx.last_result()->payload["snapshots"].size(), 2u);
    f.loop.call_sync([&] {
        f.cap.on_message(
            ctx, f.request("pipelines/snapshot", nlohmann::json{{"pipeline_id", "producer:pat:active"}, {"seq", 1}, {"forms", {"txt"}}}));
    });
    EXPECT_EQ(ctx.last_result()->payload["snapshot"]["seq"], 1);
    EXPECT_TRUE(ctx.last_result()->payload["snapshot"]["txt"].is_string());
    EXPECT_FALSE(ctx.last_result()->payload["snapshot"].contains("dot")); // not requested
    f.loop.call_sync([&] { f.cap.on_message(ctx, f.request("stats")); });
    EXPECT_EQ(ctx.last_result()->payload["stats"]["encoder"], "software");

    // failure modes: unknown pipeline, bad forms, unknown request
    f.loop.call_sync([&] {
        EXPECT_THROW(f.cap.on_message(ctx, f.request("pipelines/snapshot", nlohmann::json{{"pipeline_id", "nope"}})), FjarrError);
        EXPECT_THROW(f.cap.on_message(ctx, f.request("pipelines/subscribe", nlohmann::json{{"forms", {"pdf"}}})), FjarrError);
        EXPECT_THROW(f.cap.on_message(ctx, f.request("pipelines/history", nlohmann::json{{"pipeline_id", "nope"}})), FjarrError);
        EXPECT_THROW(f.cap.on_message(ctx, f.request("what")), FjarrError);
        f.cap.on_message(ctx, f.request("pipelines/unsubscribe"));
    });
    f.take();
    EXPECT_EQ(ctx.events("snapshot").size(), 2u); // unsubscribed: nothing more
    f.loop.call_sync([&] { f.cap.session_detached(ctx.sid, DetachReason::Closed, ""); });
}

TEST(IntrospectCapability, newestWinsWhileBlobsAreInFlightAndASessionEndingMidBlobDeliversNothing) {
    Fixture f;
    RecordingContext ctx;
    ctx.complete_blobs_at_once = false; // the test plays the pump
    f.take();
    f.loop.call_sync([&] {
        f.cap.session_attached(ctx, nlohmann::json::object());
        f.cap.on_message(ctx, f.request("pipelines/subscribe", nlohmann::json{{"forms", {"dot"}}}));
    });
    ASSERT_EQ(ctx.events("snapshot").size(),
              1u); // seq 1 replayed, its dot in flight
    ASSERT_EQ(ctx.blobs.size(), 1u);
    f.take();
    f.take();
    f.take();
    EXPECT_EQ(ctx.events("snapshot").size(),
              1u); // seq 2..4 held back: only the newest waits
    EXPECT_EQ(ctx.blobs.size(), 1u);
    EXPECT_EQ(f.cap.coalesced(), 2u);
    f.loop.call_sync([&] { ctx.blobs[0].done(true); }); // the channel drained
    ASSERT_EQ(ctx.events("snapshot").size(), 2u);
    EXPECT_EQ(ctx.events("snapshot")[1]["seq"], 4); // seq 2 and 3 were never sent
    ASSERT_EQ(ctx.blobs.size(), 2u);
    EXPECT_EQ(ctx.blobs[1].bytes, f.store.latest("producer:pat:active")->dot);
    // the session ends while seq 4's blob is in flight: the core reports false
    // after detach
    f.take();
    EXPECT_EQ(ctx.events("snapshot").size(), 2u);
    f.loop.call_sync([&] {
        f.cap.session_detached(ctx.sid, DetachReason::PeerGone, "peer-gone");
        ctx.blobs[1].done(false);
    });
    EXPECT_EQ(ctx.events("snapshot").size(), 2u);
    EXPECT_EQ(ctx.blobs.size(), 2u);
    f.take(); // nobody attached: nothing delivered, nothing crashes
    f.loop.call_sync([&] { f.cap.shutdown(); });
}
