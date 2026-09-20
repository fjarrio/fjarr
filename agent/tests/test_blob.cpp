// Blob frames (docs/08#blob-frames): the chunk codec, the bounded assembler and
// the outbound pump — the failure modes first: bad headers, gaps, oversize,
// eviction, expiry, a stalled channel, cancel and a session ending mid-blob.
#include <gtest/gtest.h>

#include <fjarr/blob.hpp>
#include <fjarr/errors.hpp>

#include "core/blob_pump.hpp"

using namespace fjarr;
using namespace std::chrono;

namespace {
const std::string ID = "01936b1e-2c1a-7c3e-9a8b-0f1e2d3c4b5c";
std::span<const std::byte> bytes_of(const std::string &s) { return {reinterpret_cast<const std::byte *>(s.data()), s.size()}; }
std::string payload_of(const BlobChunk &c) { return std::string(reinterpret_cast<const char *>(c.payload.data()), c.payload.size()); }

/// A ChannelSender that accepts frames until `capacity` bytes are buffered,
/// then refuses (HIGH_WATER).
struct FakeBulk final : ChannelSender {
    std::size_t capacity;
    std::size_t buffered = 0;
    std::vector<std::vector<std::byte>> frames;
    explicit FakeBulk(std::size_t cap) : capacity(cap) {}
    void send(const Envelope &) override { throw FjarrError("payload-invalid", "bulk"); }
    bool send_binary(std::span<const std::byte> frame) override {
        if (buffered >= capacity) return false;
        frames.emplace_back(frame.begin(), frame.end());
        buffered += frame.size();
        return true;
    }
    std::size_t buffered_amount() const override { return buffered; }
    void on_drain(std::function<void()>) override {}
    void drain() { buffered = 0; }
    std::string reassembled() const {
        std::string out;
        for (const auto &f : frames) out += payload_of(*blob::parse_chunk(f));
        return out;
    }
};
} // namespace

TEST(BlobCodec, roundTripsTheHeaderAndRefusesBadFrames) {
    const std::string body = "hello blob";
    const auto frame = blob::encode_chunk(ID, 3, 20, bytes_of(body));
    ASSERT_EQ(frame.size(), blob::HEADER_BYTES + body.size());
    const auto c = blob::parse_chunk(frame);
    ASSERT_TRUE(c);
    EXPECT_EQ(c->blob_id, ID);
    EXPECT_EQ(c->offset, 3u);
    EXPECT_EQ(c->blob_len, 20u);
    EXPECT_EQ(payload_of(*c), body);
    // wrong version
    auto bad = frame;
    bad[0] = std::byte{2};
    EXPECT_FALSE(blob::parse_chunk(bad));
    // payload_len disagrees with the message length
    bad = frame;
    bad.push_back(std::byte{0});
    EXPECT_FALSE(blob::parse_chunk(bad));
    // truncated header
    EXPECT_FALSE(blob::parse_chunk(std::span<const std::byte>(frame.data(), 10)));
    // a chunk that overruns blob_len is refused at both ends
    EXPECT_THROW(blob::encode_chunk(ID, 15, 20, bytes_of(body)), FjarrError);
    bad = blob::encode_chunk(ID, 0, 20, bytes_of(body));
    bad[32] = std::byte{5}; // blob_len := 5 < offset+payload
    EXPECT_FALSE(blob::parse_chunk(bad));
    // ids: text ↔ bytes
    EXPECT_FALSE(blob::uuid_bytes("not-a-uuid"));
    EXPECT_THROW(blob::encode_chunk("nope", 0, 1, bytes_of("x")), FjarrError);
    EXPECT_EQ(blob::uuid_text(*blob::uuid_bytes(ID)), ID);
    // the reference object (the blob-ref schema fragment)
    blob::BlobRef ref{ID, 20, "text/vnd.graphviz"};
    EXPECT_EQ(ref.to_json(), (nlohmann::json{{"blob", ID}, {"len", 20}, {"type", "text/vnd.graphviz"}}));
    EXPECT_EQ(blob::BlobRef::from_json(ref.to_json())->id, ID);
    EXPECT_FALSE(blob::BlobRef::from_json(nlohmann::json{{"blob", "x"}, {"len", 1}}));
    EXPECT_FALSE(blob::BlobRef::from_json(nlohmann::json{{"len", 1}}));
}

TEST(BlobAssembler, completesInterleavedBlobsAndDiscardsOnAGap) {
    blob::BlobAssembler a;
    const std::string other = "01936b1e-2c1a-7c3e-9a8b-0f1e2d3c4b5d";
    auto chunk = [](const std::string &id, std::uint64_t off, std::uint64_t len, const std::string &p) {
        return blob::parse_chunk(blob::encode_chunk(id, off, len, bytes_of(p)));
    };
    auto keep1 = blob::encode_chunk(ID, 0, 6, bytes_of("abc"));
    EXPECT_FALSE(a.on_chunk(*blob::parse_chunk(keep1)));
    auto keep2 = blob::encode_chunk(other, 0, 2, bytes_of("xy"));
    EXPECT_EQ(a.on_chunk(*blob::parse_chunk(keep2)),
              other); // interleaved, completed first
    auto keep3 = blob::encode_chunk(ID, 3, 6, bytes_of("def"));
    EXPECT_EQ(a.on_chunk(*blob::parse_chunk(keep3)), ID);
    EXPECT_EQ(a.take(ID), "abcdef");
    EXPECT_EQ(a.take(other), "xy");
    EXPECT_FALSE(a.take(ID)); // taken once
    EXPECT_EQ(a.pending_bytes(), 0u);
    // a gap (offset 3 before offset 0..3 arrived) discards the blob
    auto gap = blob::encode_chunk(ID, 3, 6, bytes_of("def"));
    EXPECT_FALSE(a.on_chunk(*blob::parse_chunk(gap)));
    EXPECT_EQ(a.dropped(), 1u);
    auto first = blob::encode_chunk(ID, 0, 6, bytes_of("abc"));
    EXPECT_FALSE(a.on_chunk(*blob::parse_chunk(first)));
    auto wrong = blob::encode_chunk(ID, 4, 6, bytes_of("ef")); // skipped a byte
    EXPECT_FALSE(a.on_chunk(*blob::parse_chunk(wrong)));
    EXPECT_EQ(a.size(), 0u);
    EXPECT_EQ(a.dropped(), 2u);
    (void)chunk;
}

TEST(BlobAssembler, isBoundedInBytesAndInTime) {
    blob::BlobAssembler a(/*max_blob=*/64, /*pending_bytes=*/10, milliseconds(1000));
    const auto t0 = blob::BlobAssembler::clock::now();
    // above max_blob: refused outright
    auto big = blob::encode_chunk(ID, 0, 65, bytes_of("x"));
    EXPECT_FALSE(a.on_chunk(*blob::parse_chunk(big), t0));
    EXPECT_EQ(a.dropped(), 1u);
    // three incomplete 4-byte blobs into a 10-byte store: the oldest is evicted
    const std::string ids[] = {"01936b1e-2c1a-7c3e-9a8b-000000000001", "01936b1e-2c1a-7c3e-9a8b-000000000002",
                               "01936b1e-2c1a-7c3e-9a8b-000000000003"};
    for (int i = 0; i < 3; i++) {
        auto f = blob::encode_chunk(ids[i], 0, 8, bytes_of("abcd"));
        EXPECT_FALSE(a.on_chunk(*blob::parse_chunk(f), t0 + milliseconds(i)));
    }
    EXPECT_EQ(a.size(), 2u);
    EXPECT_LE(a.pending_bytes(), 10u);
    auto tail = blob::encode_chunk(ids[0], 4, 8, bytes_of("efgh")); // the evicted one cannot resume
    EXPECT_FALSE(a.on_chunk(*blob::parse_chunk(tail), t0 + milliseconds(3)));
    // idle blobs expire after the ttl
    a.expire(t0 + milliseconds(1500));
    EXPECT_EQ(a.size(), 0u);
}

TEST(BlobPump, chunksUnderTheWatermarkResumesOnDrainAndReportsCompletion) {
    FakeBulk sender(/*capacity=*/200);
    core::BlobPump pump(/*chunk_payload=*/64);
    std::string body(300, 'z');
    for (std::size_t i = 0; i < body.size(); i++) body[i] = static_cast<char>('a' + i % 26);
    std::vector<bool> done;
    pump.enqueue(core::BlobTransfer{ID, body, 0, [&](bool ok) { done.push_back(ok); }});
    pump.pump(nullptr); // channel not open yet: nothing sent, nothing lost
    EXPECT_TRUE(sender.frames.empty());
    EXPECT_EQ(pump.queued_bytes(), 300u);
    pump.pump(&sender);
    EXPECT_EQ(sender.frames.size(),
              2u); // 2 × (37 + 64) ≥ 200: stalled at HIGH_WATER
    EXPECT_TRUE(done.empty());
    EXPECT_EQ(pump.queued_bytes(), 300u - 2 * 64);
    int drains = 0;
    while (done.empty() && drains < 10) { // the channel's drain signal resumes the pump
        sender.drain();
        pump.pump(&sender);
        drains++;
    }
    EXPECT_EQ(drains, 2); // 2 + 2 + 1 frames
    EXPECT_EQ(sender.frames.size(), 5u);
    EXPECT_EQ(done, std::vector<bool>{true});
    EXPECT_EQ(sender.reassembled(), body);
    EXPECT_EQ(pump.size(), 0u);
    // every frame carries the same id and the whole length; offsets are
    // contiguous
    std::uint64_t expect_off = 0;
    for (const auto &f : sender.frames) {
        const auto c = *blob::parse_chunk(f);
        EXPECT_EQ(c.blob_id, ID);
        EXPECT_EQ(c.blob_len, 300u);
        EXPECT_EQ(c.offset, expect_off);
        expect_off += c.payload.size();
    }
}

TEST(BlobPump, cancelAndFailAllReportFalseAndAnEnqueueFromDoneIsServed) {
    FakeBulk sender(1 << 20);
    core::BlobPump pump(64);
    std::vector<std::string> outcomes;
    pump.enqueue(core::BlobTransfer{ID, "first", 0, [&](bool ok) {
                                        outcomes.push_back(ok ? "first:ok" : "first:cancelled");
                                        pump.enqueue(
                                            core::BlobTransfer{"01936b1e-2c1a-7c3e-9a8b-000000000009", "chained", 0, [&](bool ok2) {
                                                                   outcomes.push_back(ok2 ? "chained:ok" : "chained:cancelled");
                                                               }});
                                    }});
    pump.enqueue(core::BlobTransfer{"01936b1e-2c1a-7c3e-9a8b-000000000002", "second", 0,
                                    [&](bool ok) { outcomes.push_back(ok ? "second:ok" : "second:cancelled"); }});
    EXPECT_TRUE(pump.cancel("01936b1e-2c1a-7c3e-9a8b-000000000002"));
    EXPECT_FALSE(pump.cancel("01936b1e-2c1a-7c3e-9a8b-000000000002"));
    pump.pump(&sender);
    EXPECT_EQ(outcomes, (std::vector<std::string>{"second:cancelled", "first:ok", "chained:ok"}));
    pump.enqueue(core::BlobTransfer{"01936b1e-2c1a-7c3e-9a8b-000000000003", "third", 0,
                                    [&](bool ok) { outcomes.push_back(ok ? "third:ok" : "third:cancelled"); }});
    pump.fail_all(); // the session ended
    EXPECT_EQ(outcomes.back(), "third:cancelled");
    EXPECT_EQ(pump.size(), 0u);
}
