#pragma once
// Built-in VideoSource implementations and the type registry.
// spec: docs/23-agent-core-architecture.md#video-sources-one-contract-three-ways-to-provide-one
#include <functional>
#include <map>
#include <memory>
#include <string>

#include <fjarr/video_source.hpp>

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
class SourceRegistry {
  public:
    SourceRegistry();
    void add(SourceType type);
    bool has(const std::string& type) const;
    /// Build a source from a config value: a string (tier 1) or {type, …params}.
    std::unique_ptr<VideoSource> create(const nlohmann::json& config) const;
    std::vector<std::string> types() const;

  private:
    std::map<std::string, SourceType> types_;
};

} // namespace fjarr::media
