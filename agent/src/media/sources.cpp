#include "sources.hpp"

#include <sstream>

#include <fjarr/errors.hpp>

#include "core/glib/raii.hpp"
#include "core/log.hpp"

namespace fjarr::media {

// ------------------------------------------------------ description

GstDescriptionSource::GstDescriptionSource(std::string description, std::string identity)
    : description_(std::move(description)), identity_(identity.empty() ? description_ : std::move(identity)) {}

std::string GstDescriptionSource::first_factory(const std::string& description) {
    std::istringstream ss(description);
    std::string tok;
    ss >> tok;
    return tok;
}

SourceInfo GstDescriptionSource::describe() const {
    return SourceInfo{identity_, {SourceOutput{"src", TrackKind::Video, "video/x-raw"}}};
}

GstBin* GstDescriptionSource::create_bin() {
    GError* err = nullptr;
    GstElement* bin = gst_parse_bin_from_description(description_.c_str(), TRUE, &err);
    if (err) {
        glib::GErrorPtr guard(err);
        if (bin) [[maybe_unused]] auto released = glib::sink_element(bin); // partial bin from a recoverable error: released
        last_error_ = err->message;
        log::error("source", "description failed", {{"description", description_}, {"error", last_error_}});
        return nullptr;
    }
    return GST_BIN(bin);
}

bool GstDescriptionSource::available() const {
    const std::string factory = first_factory(description_);
    if (factory.empty()) return false;
    glib::GstObjectPtr<GstElementFactory> f(gst_element_factory_find(factory.c_str()));
    if (!f) {
        last_error_ = "element missing: " + factory;
        return false;
    }
    return true;
}

void GstDescriptionSource::on_availability_changed(std::function<void(bool)> cb) { cb_ = std::move(cb); }

// ------------------------------------------------------- test pattern

TestPatternSource::TestPatternSource(std::string pattern, int width, int height, int fps, std::string identity)
    : pattern_(std::move(pattern)), width_(width), height_(height), fps_(fps),
      identity_(identity.empty() ? "test:" + pattern_ : std::move(identity)) {}

SourceInfo TestPatternSource::describe() const {
    return SourceInfo{identity_, {SourceOutput{"src", TrackKind::Video, "video/x-raw,format=I420"}}};
}

GstBin* TestPatternSource::create_bin() {
    // An explicit capsfilter: a bare caps string at the end of a bin description does not parse.
    const std::string desc = "videotestsrc pattern=" + pattern_ + " is-live=true ! capsfilter caps=\"video/x-raw,format=I420,width=" +
                             std::to_string(width_) + ",height=" + std::to_string(height_) + ",framerate=" +
                             std::to_string(fps_) + "/1\"";
    GError* err = nullptr;
    GstElement* bin = gst_parse_bin_from_description(desc.c_str(), TRUE, &err);
    if (err) {
        glib::GErrorPtr guard(err);
        if (bin) [[maybe_unused]] auto released = glib::sink_element(bin);
        log::error("source", "test pattern failed", {{"error", err->message}});
        return nullptr;
    }
    return GST_BIN(bin);
}

// ----------------------------------------------------------- registry

SourceRegistry::SourceRegistry() {
    add(SourceType{"gst", nlohmann::json{{"type", "object"}, {"properties", {{"description", {{"type", "string"}}}}}, {"required", {"description"}}},
                   [](const nlohmann::json& p) -> std::unique_ptr<VideoSource> {
                       return std::make_unique<GstDescriptionSource>(p.at("description").get<std::string>());
                   }});
    add(SourceType{"test",
                   nlohmann::json{{"type", "object"},
                                  {"properties",
                                   {{"pattern", {{"type", "string"}}},
                                    {"width", {{"type", "integer"}}},
                                    {"height", {{"type", "integer"}}},
                                    {"fps", {{"type", "integer"}}}}}},
                   [](const nlohmann::json& p) -> std::unique_ptr<VideoSource> {
                       return std::make_unique<TestPatternSource>(p.value("pattern", "smpte"), p.value("width", 1280),
                                                                  p.value("height", 720), p.value("fps", 30));
                   }});
}

void SourceRegistry::add(SourceType type) { types_[type.name] = std::move(type); }
bool SourceRegistry::has(const std::string& type) const { return types_.count(type) > 0; }
std::vector<std::string> SourceRegistry::types() const {
    std::vector<std::string> out;
    for (const auto& [k, _] : types_) out.push_back(k);
    return out;
}

std::unique_ptr<VideoSource> SourceRegistry::create(const nlohmann::json& config) const {
    if (config.is_string()) return std::make_unique<GstDescriptionSource>(config.get<std::string>());
    if (!config.is_object() || !config.contains("type") || !config["type"].is_string())
        throw FjarrError("config", "source must be a description string or {type = …}");
    const std::string type = config["type"];
    auto it = types_.find(type);
    if (it == types_.end()) throw FjarrError("config", "unknown source type: " + type);
    nlohmann::json params = config;
    params.erase("type");
    return it->second.create(params);
}

} // namespace fjarr::media
