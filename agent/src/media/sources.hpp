#pragma once
// Built-in VideoSource implementations and the type registry.
// spec: docs/23-agent-core-architecture.md#video-sources-one-contract-three-ways-to-provide-one
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <gio/gio.h>

#include <fjarr/video_source.hpp>

#include "core/glib/raii.hpp"

namespace fjarr::media {

/// Tier 1: a GStreamer description string wrapped as a bin with a ghost `src` pad.
class GstDescriptionSource final : public VideoSource {
  public:
    explicit GstDescriptionSource(std::string description, std::string identity = "");
    SourceInfo describe() const override;
    GstBin* create_bin() override;
    bool available() const override;
    void on_availability_changed(std::function<void(bool)> cb) override;
    /// The element the description needs (first factory name), for availability checks.
    static std::string first_factory(const std::string& description);
    const std::string& last_error() const { return last_error_; }
    std::string unavailable_reason() const override { return last_error_; }

  private:
    std::string description_;
    std::string identity_;
    std::function<void(bool)> cb_;
    mutable std::string last_error_;
};

/// Built-in `test`: videotestsrc with pattern/size/fps params (docs/06 fjarr.test).
class TestPatternSource final : public VideoSource {
  public:
    explicit TestPatternSource(std::string pattern = "smpte", int width = 1280, int height = 720, int fps = 30,
                               std::string identity = "");
    SourceInfo describe() const override;
    GstBin* create_bin() override;
    bool available() const override { return true; }
    void on_availability_changed(std::function<void(bool)>) override {}

  private:
    std::string pattern_;
    int width_, height_, fps_;
    std::string identity_;
};

/// type name → factory (built-ins + customer-registered).
/// `v4l2` (slice 4): a convenience over tier 1 — `v4l2src device=… ! <format caps> ! [jpegdec]` — with
/// availability from the device node and hot-plug from a GIO monitor on the kernel's
/// /dev/v4l/by-id tree (udev populates it; no libudev in the core). spec: docs/23#video-sources
class V4l2Source final : public VideoSource {
  public:
    struct Params {
        std::string device = "/dev/video0"; // a path, or a name under /dev/v4l/by-id
        std::string format = "auto";        // auto | mjpeg | yuyv
        int width = 0, height = 0, fps = 0; // 0 = whatever the device prefers
    };
    /// `ctx`: the core context the availability callback must run on (nullptr = thread-default).
    explicit V4l2Source(Params p, GMainContext* ctx = nullptr);
    ~V4l2Source() override;
    SourceInfo describe() const override;
    GstBin* create_bin() override;
    bool available() const override;
    void on_availability_changed(std::function<void(bool)> cb) override;
    /// The resolved device path (by-id names are resolved under `by_id_dir`).
    std::string device_path() const;
    /// The GStreamer description the bin is built from (tests, --probe-source).
    std::string description() const;
    const std::string& last_error() const { return last_error_; }
    std::string unavailable_reason() const override { return last_error_; }
    /// Tests: watch another directory than /dev/v4l/by-id.
    void set_by_id_dir(std::string dir) { by_id_dir_ = std::move(dir); }

  private:
    void watch();
    Params p_;
    GMainContext* ctx_;

    std::string by_id_dir_ = "/dev/v4l/by-id";
    std::function<void(bool)> cb_;
    bool last_available_ = false;
    mutable std::string last_error_;
    glib::GObjectPtr<GFileMonitor> monitor_by_id_, monitor_dev_;
    glib::SignalConnection changed_by_id_, changed_dev_;
};

/// `rtsp` (slice 4): `rtspsrc location=… latency=… protocols=… ! decodebin` — decoded to raw until
/// slice 6 decides the tier model for undecoded elementary streams. spec: docs/23#video-sources
class RtspSource final : public VideoSource {
  public:
    struct Params {
        std::string url;
        int latency_ms = 200;
        std::string protocols = "auto"; // auto | tcp | udp
    };
    explicit RtspSource(Params p);
    SourceInfo describe() const override;
    GstBin* create_bin() override;
    bool available() const override { return true; } // a network source is tried; failures are bus errors
    void on_availability_changed(std::function<void(bool)>) override {}
    std::string description() const;

  private:
    Params p_;
    // Per create_bin(): the plane serializes producers per track (the old one is reset before the
    // next is built), so one connection per source suffices; a second live bin would steal it.
    glib::SignalConnection rtsp_pad_added_;
    glib::SignalConnection rtsp_select_stream_;
};

/// A bin whose last element is a decodebin: parsed without ghosting (the parser would ghost
/// decodebin's *internal* typefind pad, and for rtspsrc its delayed link), then given a targetless
/// ghost `src` that decodebin's first video pad targets when it appears — and re-targets after a
/// NULL→PLAYING cycle, when decodebin rebuilds its pads (found in slice 4: a stale target starved
/// the second start of an rtsp producer). `decodebin_name` must be the decodebin's name in `desc`.
GstBin* make_late_ghost_bin(const std::string& desc, const std::string& decodebin_name, glib::SignalConnection& pad_added, std::string* error);

class SourceRegistry final : public SourceFactory {
  public:
    /// `ctx`: the core context hot-plug callbacks run on (nullptr = thread-default at watch time).
    explicit SourceRegistry(GMainContext* ctx = nullptr);
    void add(SourceType type);
    bool has(const std::string& type) const;
    /// Build a source from a config value: a string (tier 1) or {type, …params}; params are
    /// validated against the type's schema.
    std::unique_ptr<VideoSource> create(const nlohmann::json& config) const override;
    std::vector<std::string> types() const override;

  private:
    GMainContext* ctx_;
    std::map<std::string, SourceType> types_;
};

} // namespace fjarr::media
