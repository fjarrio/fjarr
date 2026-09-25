#pragma once
// A SessionContext that records what a capability asks of it: tracks, envelopes
// and blobs — the unit-test stand-in for the core's real context (docs/15:
// capabilities are tested without a peer).
#include <chrono>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include <fjarr/session_context.hpp>

namespace fjarr::testing {

struct RecordingContext final : SessionContext {
    struct Sent {
        std::string kind, type;
        nlohmann::json payload;
    };
    struct Blob {
        blob::BlobRef ref;
        std::string bytes;
        std::function<void(bool)> done;
    };
    SessionId sid = "01a0-test-session";
    std::vector<std::vector<std::string>> updates; // each update_tracks call's track ids
    std::vector<std::string> added;
    std::vector<Sent> sent;  // results, events, fails in order
    std::vector<Blob> blobs; // send_blob calls in order; `done` is called by the
                             // test (deferred) or at once
    bool complete_blobs_at_once = true;
    std::vector<std::string> cancelled;
    int blob_seq = 0;

    const SessionId &id() const override { return sid; }
    const OperatorInfo &operator_info() const override {
        static OperatorInfo o;
        return o;
    }
    const nlohmann::json &granted_params(std::string_view) const override {
        static nlohmann::json j = nlohmann::json::object();
        return j;
    }
    void add_track(TrackSpec spec) override { added.push_back(spec.track_id); }
    void update_tracks(std::vector<TrackSpec> full_set) override {
        std::vector<std::string> ids;
        for (const auto &t : full_set) ids.push_back(t.track_id);
        updates.push_back(ids);
    }
    TrackState track_state(std::string_view) const override { return {}; }
    unsigned manifest_version() const override { return 1; }
    ChannelSender &control() override { throw std::runtime_error("no channel"); }
    ChannelSender &realtime() override { throw std::runtime_error("no channel"); }
    ChannelSender &bulk() override { throw std::runtime_error("no channel"); }
    /// A stream channel that records what was sent and can be told to refuse, the way a real one
    /// does above its watermark — which is how a lossy capability's drop path gets tested.
    struct RecordingSender final : ChannelSender {
        std::vector<std::string> frames;
        bool refuse = false;
        void send(const Envelope &) override { throw std::runtime_error("envelopes do not ride binary channels"); }
        bool send_binary(std::span<const std::byte> f) override {
            if (refuse) return false;
            frames.emplace_back(reinterpret_cast<const char *>(f.data()), f.size());
            return true;
        }
        std::size_t buffered_amount() const override { return 0; }
        void on_drain(std::function<void()>) override {}
    };
    RecordingSender stream_sender;
    ChannelSender &stream() override { return stream_sender; }
    void accept(const Envelope &r) override { sent.push_back({"accept", r.type, {}}); }
    void feedback(const Envelope &r, nlohmann::json p) override { sent.push_back({"feedback", r.type, std::move(p)}); }
    void result(const Envelope &r, nlohmann::json p) override { sent.push_back({"result", r.type, std::move(p)}); }
    void fail(const Envelope &r, std::string_view code, std::string_view message) override {
        sent.push_back(
            {"result", r.type, nlohmann::json{{"ok", false}, {"error", {{"code", std::string(code)}, {"message", std::string(message)}}}}});
    }
    void event(std::string_view type, nlohmann::json p) override { sent.push_back({"event", std::string(type), std::move(p)}); }
    blob::BlobRef send_blob(std::string bytes, std::string media_type, std::function<void(bool)> done) override {
        char id[64];
        std::snprintf(id, sizeof id, "01930000-0000-7000-8000-%012d", ++blob_seq);
        blob::BlobRef ref{id, bytes.size(), std::move(media_type)};
        blobs.push_back({ref, std::move(bytes), std::move(done)});
        if (complete_blobs_at_once && blobs.back().done) blobs.back().done(true);
        return ref;
    }
    void cancel_blob(std::string_view blob_id) override { cancelled.emplace_back(blob_id); }
    /// No loop here: the double records the request and never fires. A capability under test
    /// drives its own reads by calling the handler directly.
    std::unique_ptr<FdWatch> watch_readable(int fd, std::function<bool()> on_readable) override {
        watched.push_back(fd);
        on_readable_ = std::move(on_readable);
        struct NoopWatch final : FdWatch {};
        return std::make_unique<NoopWatch>();
    }
    /// Same as the fd watch: the double records the request and never fires on its own, so a
    /// test says when a second passed instead of waiting one.
    std::unique_ptr<Timer> every(std::chrono::milliseconds period, std::function<bool()> on_tick) override {
        timers.push_back(period);
        on_tick_ = std::move(on_tick);
        struct NoopTimer final : Timer {};
        return std::make_unique<NoopTimer>();
    }
    /// Fire the registered timer once; returns what the capability's handler said.
    bool fire_timer() { return on_tick_ ? on_tick_() : false; }
    std::vector<std::chrono::milliseconds> timers;
    std::function<bool()> on_tick_;

    /// Pretend the fd became readable; returns what the capability's handler said.
    bool fire_readable() { return on_readable_ ? on_readable_() : false; }
    std::vector<int> watched;

    void run_async(std::function<void()> job, std::function<void()> done) override {
        job();
        done();
    }
    std::unique_ptr<DeadmanHandle> arm_deadman(std::chrono::milliseconds, std::function<void()>) override { return nullptr; }
    void close(std::string_view) override {}

    /// Events of one type, in order.
    std::vector<nlohmann::json> events(std::string_view type) const {
        std::vector<nlohmann::json> out;
        for (const auto &s : sent)
            if (s.kind == "event" && s.type == type) out.push_back(s.payload);
        return out;
    }
    const Sent *last_result() const {
        for (auto it = sent.rbegin(); it != sent.rend(); ++it)
            if (it->kind == "result") return &*it;
        return nullptr;
    }
    std::function<bool()> on_readable_;

};

} // namespace fjarr::testing
