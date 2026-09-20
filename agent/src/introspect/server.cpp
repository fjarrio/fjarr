#include "server.hpp"

#include <libsoup/soup.h>

#include <fjarr/errors.hpp>

#include "core/glib/raii.hpp"
#include "core/log.hpp"

namespace fjarr::introspect {

struct Server::Impl {
    AgentConfig::IntrospectSection config;
    SnapshotStore& store;
    std::function<nlohmann::json()> sources;
    std::function<void(const std::string&)> snapshot_now;
    glib::GObjectPtr<SoupServer> server;

    Impl(AgentConfig::IntrospectSection c, SnapshotStore& s, std::function<nlohmann::json()> src, std::function<void(const std::string&)> snap)
        : config(std::move(c)), store(s), sources(std::move(src)), snapshot_now(std::move(snap)) {}

    static void respond(SoupServerMessage* msg, int status, const char* content_type, const std::string& body) {
        soup_server_message_set_status(msg, static_cast<guint>(status), nullptr);
        soup_server_message_set_response(msg, content_type, SOUP_MEMORY_COPY, body.data(), body.size());
    }

    static void handler(SoupServer*, SoupServerMessage* msg, const char* path, GHashTable* query, gpointer user) {
        auto* self = static_cast<Impl*>(user);
        const std::string p = path ? path : "/";
        const char* method = soup_server_message_get_method(msg);
        if (!self->config.token.empty()) {
            const char* auth = soup_message_headers_get_one(soup_server_message_get_request_headers(msg), "Authorization");
            if (!auth || std::string(auth) != "Bearer " + self->config.token) {
                respond(msg, 401, "application/json", R"({"error":"token"})");
                return;
            }
        }
        if (p == "/pipelines" || p == "/pipelines/") {
            nlohmann::json list = nlohmann::json::array();
            for (const auto& m : self->store.list()) {
                list.push_back({{"id", m.pipeline_id}, {"kind", m.kind}, {"state", m.state}, {"session_id", m.session_id},
                                {"seq", m.seq}, {"last_trigger", m.trigger}, {"ts", m.ts}});
            }
            respond(msg, 200, "application/json", nlohmann::json{{"pipelines", list}}.dump(2));
            return;
        }
        if (p == "/sources") {
            respond(msg, 200, "application/json", (self->sources ? self->sources() : nlohmann::json::object()).dump(2));
            return;
        }
        if (p == "/snapshot" && std::string(method) == "POST") {
            const char* pid = query ? static_cast<const char*>(g_hash_table_lookup(query, "pipeline")) : nullptr;
            if (self->snapshot_now) self->snapshot_now(pid ? pid : "");
            respond(msg, 200, "application/json", R"({"ok":true})");
            return;
        }
        if (p.rfind("/pipelines/", 0) == 0) {
            std::string rest = p.substr(11);
            std::string fmt;
            if (rest.size() > 8 && rest.compare(rest.size() - 8, 8, "/history") == 0) {
                const std::string id = rest.substr(0, rest.size() - 8);
                nlohmann::json h = nlohmann::json::array();
                for (const auto& m : self->store.history(id)) h.push_back({{"seq", m.seq}, {"trigger", m.trigger}, {"ts", m.ts}, {"state", m.state}});
                respond(msg, 200, "application/json", nlohmann::json{{"pipeline_id", id}, {"history", h}}.dump(2));
                return;
            }
            // Only a known format suffix is a suffix: a dotted track id stays part of the pipeline id.
            for (const char* f : {"json", "txt", "dot"}) {
                const std::string suffix = std::string(".") + f;
                if (rest.size() > suffix.size() && rest.compare(rest.size() - suffix.size(), suffix.size(), suffix) == 0) {
                    fmt = f;
                    rest = rest.substr(0, rest.size() - suffix.size());
                    break;
                }
            }
            std::shared_ptr<const Snapshot> snap;
            const char* seq = query ? static_cast<const char*>(g_hash_table_lookup(query, "seq")) : nullptr;
            if (seq) snap = self->store.at(rest, static_cast<unsigned>(std::atoi(seq)));
            else snap = self->store.latest(rest);
            if (!snap) {
                respond(msg, 404, "application/json", R"({"error":"no such pipeline or seq"})");
                return;
            }
            if (fmt == "dot") respond(msg, 200, "text/vnd.graphviz", snap->dot);
            else if (fmt == "txt") respond(msg, 200, "text/plain; charset=utf-8", snap->txt + "\n");
            else respond(msg, 200, "application/json", snap->json.dump(2));
            return;
        }
        if (p == "/") {
            respond(msg, 200, "text/plain; charset=utf-8",
                    "fjarr introspection endpoint (docs/24)\n/pipelines  /pipelines/<id>.{json,txt,dot}[?seq=n]  /pipelines/<id>/history  /sources  POST /snapshot?pipeline=<id>\n(the viewer arrives in slice 5)\n");
            return;
        }
        respond(msg, 404, "application/json", R"({"error":"not found"})");
    }
};

Server::Server(const AgentConfig::IntrospectSection& config, SnapshotStore& store, std::function<nlohmann::json()> sources,
               std::function<void(const std::string&)> snapshot_now)
    : impl_(std::make_unique<Impl>(config, store, std::move(sources), std::move(snapshot_now))) {}

Server::~Server() {
    if (impl_->server) soup_server_disconnect(impl_->server.get());
}

void Server::start() {
    impl_->server.reset(soup_server_new("server-header", "fjarr-agent", nullptr));
    soup_server_add_handler(impl_->server.get(), "/", &Impl::handler, impl_.get(), nullptr);
    GError* err = nullptr;
    gboolean ok = FALSE;
    if (!impl_->config.socket.empty()) {
        glib::GObjectPtr<GSocketAddress> addr(g_unix_socket_address_new(impl_->config.socket.c_str()));
        ok = soup_server_listen(impl_->server.get(), addr.get(), static_cast<SoupServerListenOptions>(0), &err);
    } else if (impl_->config.bind == "127.0.0.1" || impl_->config.bind == "localhost") {
        ok = soup_server_listen_local(impl_->server.get(), static_cast<guint>(impl_->config.port), SOUP_SERVER_LISTEN_IPV4_ONLY, &err);
    } else {
        ok = soup_server_listen_all(impl_->server.get(), static_cast<guint>(impl_->config.port), SOUP_SERVER_LISTEN_IPV4_ONLY, &err);
    }
    if (!ok) {
        glib::GErrorPtr e(err);
        // A port already in use is a startup error (docs/24): tests rely on the endpoint.
        throw FjarrError("config", "introspection endpoint cannot listen on " + impl_->config.bind + ":" + std::to_string(impl_->config.port) +
                                       ": " + (err ? err->message : "unknown"));
    }
    port_ = impl_->config.port;
    log::info("introspect", "endpoint listening", {{"bind", impl_->config.bind}, {"port", std::to_string(port_)}});
}

} // namespace fjarr::introspect
