#include <fjarr/diagnostics.hpp>

#include <cstdio>

#include <libsoup/soup.h>

#include <fjarr/version.hpp>

#include "bundle.hpp"
#include "core/glib/raii.hpp"

namespace fjarr {

DiagnosticsResult write_diagnostics(const AgentConfig& config, const std::string& out) {
    // docs/24: the bundle a support engineer asks for first. From the running agent's endpoint
    // when it answers (it has the rings and the log ring); otherwise an offline bundle of what
    // this process can know: config (redacted), versions, the encoder check.
    DiagnosticsResult result;
    std::string bytes, note;
    const std::string url = "http://" + (config.introspect.bind == "0.0.0.0" ? std::string("127.0.0.1") : config.introspect.bind) + ":" +
                            std::to_string(config.introspect.port) + "/diagnostics.tar.gz";
    if (config.introspect.enabled && config.introspect.socket.empty()) {
        glib::GObjectPtr<SoupSession> session(soup_session_new_with_options("timeout", 5u, nullptr));
        glib::GObjectPtr<SoupMessage> msg(soup_message_new(SOUP_METHOD_GET, url.c_str()));
        if (!config.introspect.token.empty())
            soup_message_headers_append(soup_message_get_request_headers(msg.get()), "Authorization", ("Bearer " + config.introspect.token).c_str());
        GError* err = nullptr;
        glib::GBytesPtr body(soup_session_send_and_read(session.get(), msg.get(), nullptr, &err));
        if (err) {
            glib::GErrorPtr e(err);
            note = "no running agent at " + url + " (" + err->message + "): offline bundle";
        } else if (soup_message_get_status(msg.get()) != SOUP_STATUS_OK) {
            note = url + " answered " + std::to_string(soup_message_get_status(msg.get())) + ": offline bundle";
        } else {
            gsize n = 0;
            const auto* data = static_cast<const char*>(g_bytes_get_data(body.get(), &n));
            bytes.assign(data, n);
            result.source = "endpoint";
            note = "from the running agent at " + url;
        }
    } else note = "introspection endpoint disabled or on a socket: offline bundle";
    if (bytes.empty()) {
        result.source = "offline";
        introspect::BundleFiles files;
        nlohmann::json cfg{{"agent", {{"robot_id", config.agent.robot_id}, {"server_url", config.agent.server_url}, {"ice_policy", config.agent.ice_policy},
                                      {"dev_token", config.agent.dev_token.empty() ? "" : "<redacted>"}}},
                           {"media", {{"encoder", config.media.encoder}}},
                           {"introspect", {{"enabled", config.introspect.enabled}, {"bind", config.introspect.bind}, {"port", config.introspect.port}}},
                           {"capabilities", config.capabilities}};
        files.emplace_back("fjarr-diagnostics/README.txt", "fjarr diagnostics bundle (offline: no running agent answered — docs/24)\n");
        files.emplace_back("fjarr-diagnostics/config.json", cfg.dump(2) + "\n");
        files.emplace_back("fjarr-diagnostics/versions.json", nlohmann::json{{"fjarr", version()}, {"gstreamer", gstreamer_version()}}.dump(2) + "\n");
        files.emplace_back("fjarr-diagnostics/check.txt", std::string("hardware H.264 encode available: ") + (hardware_encode_available() ? "yes" : "no") +
                                                               "\nencoder configured: " + config.media.encoder + "\n");
        bytes = introspect::gzip(introspect::tar(files));
    }
    FILE* f = std::fopen(out.c_str(), "wb");
    if (!f) {
        result.message = "cannot write " + out;
        return result;
    }
    std::fwrite(bytes.data(), 1, bytes.size(), f);
    std::fclose(f);
    result.ok = true;
    result.message = note + "; wrote " + out + " (" + std::to_string(bytes.size()) + " bytes)";
    return result;
}

} // namespace fjarr
