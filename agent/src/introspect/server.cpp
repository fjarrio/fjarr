#include "server.hpp"

#include <algorithm>

#include <libsoup/soup.h>

#include <fjarr/errors.hpp>
#include <fjarr/fjarr.hpp>

#include "bundle.hpp"
#include "core/glib/raii.hpp"
#include "core/log.hpp"

namespace fjarr::introspect {

namespace {
constexpr std::size_t SSE_MAX_PENDING = 1024 * 1024; // a reader that falls this far behind is dropped (docs/24)

std::string escape_newlines(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '\n') out += "\\n";
        else if (c == '\r') continue;
        else out += c;
    }
    return out;
}
} // namespace

struct Server::Impl {
    AgentConfig::IntrospectSection config;
    SnapshotStore& store;
    Providers providers;
    glib::GObjectPtr<SoupServer> server;

    // ----- /events clients: the message is kept alive by our ref until libsoup says it finished.
    struct SseClient {
        glib::GObjectPtr<SoupServerMessage> msg;
        std::string body; // "" | json | dot | txt
        std::size_t pending = 0; // appended but not yet written
        bool closing = false;    // stream ended by us: nothing more is appended
        glib::SignalConnection finished;
        glib::SignalConnection disconnected;
        glib::SignalConnection wrote;
    };
    std::vector<std::unique_ptr<SseClient>> clients;
    glib::SourceGuard keepalive;

    Impl(AgentConfig::IntrospectSection c, SnapshotStore& s, Providers p) : config(std::move(c)), store(s), providers(std::move(p)) {}

    static void respond(SoupServerMessage* msg, int status, const char* content_type, const std::string& body) {
        soup_server_message_set_status(msg, static_cast<guint>(status), nullptr);
        soup_server_message_set_response(msg, content_type, SOUP_MEMORY_COPY, body.data(), body.size());
    }

    static const char* q(GHashTable* query, const char* key) {
        return query ? static_cast<const char*>(g_hash_table_lookup(query, key)) : nullptr;
    }

    void send_sse(SseClient& c, const std::string& frame) {
        if (c.closing) return;
        if (c.pending + frame.size() > SSE_MAX_PENDING) {
            // Dropped, as docs/24 promises: a stalled peer would never consume a terminator, so the
            // socket is closed; libsoup then finishes the message (INTERRUPTED → `finished`).
            log::warn("introspect", "events client too slow: dropped", {{"pending", std::to_string(c.pending)}});
            c.closing = true;
            if (GSocket* sock = soup_server_message_get_socket(c.msg.get())) g_socket_close(sock, nullptr);
            return;
        }
        c.pending += frame.size();
        soup_message_body_append(soup_server_message_get_response_body(c.msg.get()), SOUP_MEMORY_COPY, frame.data(), frame.size());
        soup_server_message_unpause(c.msg.get()); // the documented kick after appending to a chunked response
    }

    void broadcast(const Snapshot& snap) {
        for (auto& c : clients) send_sse(*c, sse_frame(snap, c->body));
    }

    void drop_client(SoupServerMessage* msg) {
        clients.erase(std::remove_if(clients.begin(), clients.end(), [msg](const std::unique_ptr<SseClient>& c) { return c->msg.get() == msg; }),
                      clients.end());
        if (clients.empty()) keepalive.cancel();
    }

    void open_events(SoupServerMessage* msg, GHashTable* query) {
        auto client = std::make_unique<SseClient>();
        client->msg = glib::GObjectPtr<SoupServerMessage>(glib::ref_object(msg));
        const char* body = q(query, "body");
        client->body = body ? body : "";
        if (client->body != "json" && client->body != "dot" && client->body != "txt") client->body.clear();
        soup_server_message_set_status(msg, 200, nullptr);
        SoupMessageHeaders* h = soup_server_message_get_response_headers(msg);
        soup_message_headers_set_encoding(h, SOUP_ENCODING_CHUNKED);
        // Without this libsoup keeps every written chunk for the life of the response (accumulate
        // defaults to TRUE): a day-long /events client would retain every frame ever sent.
        soup_message_body_set_accumulate(soup_server_message_get_response_body(msg), FALSE);
        soup_message_headers_set_content_type(h, "text/event-stream", nullptr);
        soup_message_headers_append(h, "Cache-Control", "no-cache");
        soup_message_headers_append(h, "X-Accel-Buffering", "no");
        std::string first = "retry: 1000\n\n";
        // Last-Event-ID: <pipeline>@<seq> — replay everything the rings still hold from after that
        // snapshot, across every pipeline (a reconnecting client missed all of them, not one ring's).
        if (const char* last = soup_message_headers_get_one(soup_server_message_get_request_headers(msg), "Last-Event-ID")) {
            const std::string id = last;
            const auto at = id.rfind('@');
            if (at != std::string::npos) {
                const std::string pipeline = id.substr(0, at);
                const unsigned seq = static_cast<unsigned>(std::atoi(id.c_str() + at + 1));
                std::int64_t since_ts = -1;
                if (auto ref = store.at(pipeline, seq)) since_ts = ref->meta.ts;
                std::vector<std::shared_ptr<const Snapshot>> replay;
                for (const auto& m : store.list())
                    for (const auto& h : store.history(m.pipeline_id))
                        if ((m.pipeline_id == pipeline && h.seq > seq) || (m.pipeline_id != pipeline && since_ts >= 0 && h.ts > since_ts))
                            if (auto snap = store.at(m.pipeline_id, h.seq)) replay.push_back(snap);
                std::sort(replay.begin(), replay.end(), [](const auto& a, const auto& b) { return a->meta.ts < b->meta.ts; });
                for (const auto& snap : replay) first += sse_frame(*snap, client->body);
            }
        }
        SseClient* raw = client.get();
        client->finished = glib::SignalConnection(msg, "finished", G_CALLBACK((+[](SoupServerMessage* m, gpointer d) {
                                                      static_cast<Impl*>(d)->drop_client(m);
                                                  })),
                                                  this);
        // libsoup tears a connection down without `finished` on its own disconnect paths.
        client->disconnected = glib::SignalConnection(msg, "disconnected", G_CALLBACK((+[](SoupServerMessage* m, gpointer d) {
                                                          static_cast<Impl*>(d)->drop_client(m);
                                                      })),
                                                      this);
        client->wrote = glib::SignalConnection(msg, "wrote-body-data", G_CALLBACK((+[](SoupServerMessage*, guint chunk, gpointer d) {
                                                   auto* c = static_cast<SseClient*>(d);
                                                   c->pending = chunk >= c->pending ? 0 : c->pending - chunk;
                                               })),
                                               raw);
        clients.push_back(std::move(client));
        log::info("introspect", "events client opened", {{"body", raw->body.empty() ? "none" : raw->body}, {"clients", std::to_string(clients.size())}});
        send_sse(*raw, first);
        if (!keepalive.active()) {
            keepalive = glib::SourceGuard(g_timeout_source_new_seconds(15), g_main_context_get_thread_default(), [this] {
                for (auto& c : clients) send_sse(*c, ": keep-alive\n\n");
                return true;
            });
        }
    }

    static void handler(SoupServer* server, SoupServerMessage* msg, const char* path, GHashTable* query, gpointer user) {
        // A libsoup handler is a C callback on the core loop: nothing may unwind through it (docs/23).
        try {
            handle(server, msg, path, query, user);
        } catch (const std::exception& e) {
            log::error("introspect", "request failed", {{"path", path ? path : "/"}, {"error", e.what()}});
            respond(msg, 500, "application/json", nlohmann::json{{"error", "internal"}, {"message", std::string(e.what()).substr(0, 256)}}.dump());
        }
    }

    static void handle(SoupServer*, SoupServerMessage* msg, const char* path, GHashTable* query, gpointer user) {
        auto* self = static_cast<Impl*>(user);
        const std::string p = path ? path : "/";
        const std::string method = soup_server_message_get_method(msg);
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
            respond(msg, 200, "application/json", (self->providers.sources ? self->providers.sources() : nlohmann::json::object()).dump(2));
            return;
        }
        if (p == "/stats") {
            nlohmann::json j = self->providers.stats ? self->providers.stats() : nlohmann::json::object();
            j["events_clients"] = self->clients.size();
            respond(msg, 200, "application/json", j.dump(2));
            return;
        }
        if (p == "/memory" || p == "/memory/") {
            if (!self->providers.memory) {
                respond(msg, 404, "application/json", R"({"error":"no memory census"})");
                return;
            }
            const char* since = q(query, "since");
            respond(msg, 200, "application/json", (since ? self->providers.memory->since(since) : self->providers.memory->now()).dump(2));
            return;
        }
        if (p == "/memory/checkpoint" && method == "POST") {
            if (!self->providers.memory) {
                respond(msg, 404, "application/json", R"({"error":"no memory census"})");
                return;
            }
            respond(msg, 200, "application/json", self->providers.memory->checkpoint().dump(2));
            return;
        }
        if (p == "/log") {
            const char* minutes = q(query, "minutes");
            const int m = minutes ? std::max(1, std::atoi(minutes)) : 10;
            std::string out;
            for (const auto& line : log::recent(std::chrono::minutes(m))) {
                out += line;
                out += '\n';
            }
            respond(msg, 200, "text/plain; charset=utf-8", out);
            return;
        }
        if (p == "/events") {
            self->open_events(msg, query);
            return;
        }
        if (p == "/diagnostics.tar.gz") {
            const std::string bytes = self->bundle();
            soup_server_message_set_status(msg, 200, nullptr);
            soup_message_headers_append(soup_server_message_get_response_headers(msg), "Content-Disposition",
                                        "attachment; filename=\"fjarr-diagnostics.tar.gz\"");
            soup_server_message_set_response(msg, "application/gzip", SOUP_MEMORY_COPY, bytes.data(), bytes.size());
            return;
        }
        if (p == "/snapshot" && method == "POST") {
            const char* pid = q(query, "pipeline");
            if (self->providers.snapshot_now) self->providers.snapshot_now(pid ? pid : "");
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
            const char* seq = q(query, "seq");
            if (seq) snap = self->store.at(rest, static_cast<unsigned>(std::atoi(seq)));
            else snap = self->store.latest(rest);
            if (!snap) {
                respond(msg, 404, "application/json", R"({"error":"no such pipeline or seq"})");
                return;
            }
            if (fmt == "dot") {
                if (snap->dot.empty()) respond(msg, 404, "application/json", R"({"error":"DOT is kept for a ring's most recent snapshots only (docs/24); the JSON and summary are"})");
                else respond(msg, 200, "text/vnd.graphviz", snap->dot);
            }
            else if (fmt == "txt") respond(msg, 200, "text/plain; charset=utf-8", snap->txt + "\n");
            else respond(msg, 200, "application/json", snap->json);
            return;
        }
        if (p == "/") {
            respond(msg, 200, "text/plain; charset=utf-8",
                    "fjarr introspection endpoint (docs/24)\n"
                    "/pipelines  /pipelines/<id>.{json,txt,dot}[?seq=n]  /pipelines/<id>/history  /sources  /stats\n"
                    "/memory[?since=<checkpoint>]  POST /memory/checkpoint  /log[?minutes=n]  /events[?body=json|dot|txt]\n"
                    "/diagnostics.tar.gz  POST /snapshot?pipeline=<id>\n(the viewer arrives in slice 5)\n");
            return;
        }
        respond(msg, 404, "application/json", R"({"error":"not found"})");
    }

    std::string bundle() const {
        BundleFiles files;
        nlohmann::json extra = providers.bundle_extra ? providers.bundle_extra() : nlohmann::json::object();
        files.emplace_back("fjarr-diagnostics/README.txt",
                           "fjarr diagnostics bundle (docs/24)\nconfig.json: config with secrets redacted · check.txt: encoder/versions · "
                           "pipelines/: every snapshot the ring holds (json/txt/dot) · stats.json · sources.json · memory.json · log.txt: the last 10 minutes\n");
        files.emplace_back("fjarr-diagnostics/config.json", extra.value("config", nlohmann::json::object()).dump(2) + "\n");
        files.emplace_back("fjarr-diagnostics/check.txt", extra.value("check", std::string()));
        files.emplace_back("fjarr-diagnostics/versions.json", extra.value("versions", nlohmann::json::object()).dump(2) + "\n");
        for (const auto& m : store.list()) {
            for (const auto& h : store.history(m.pipeline_id)) {
                auto snap = store.at(m.pipeline_id, h.seq);
                if (!snap) continue;
                const std::string base = "fjarr-diagnostics/pipelines/" + m.pipeline_id + "-" + std::to_string(h.seq) + "-" + h.trigger;
                files.emplace_back(base + ".json", snap->json + "\n");
                files.emplace_back(base + ".txt", snap->txt + "\n");
                if (!snap->dot.empty()) files.emplace_back(base + ".dot", snap->dot); // retired rings keep DOT for the last snapshot only
            }
        }
        files.emplace_back("fjarr-diagnostics/stats.json", (providers.stats ? providers.stats() : nlohmann::json::object()).dump(2) + "\n");
        files.emplace_back("fjarr-diagnostics/sources.json", (providers.sources ? providers.sources() : nlohmann::json::object()).dump(2) + "\n");
        files.emplace_back("fjarr-diagnostics/memory.json", (providers.memory ? providers.memory->now() : nlohmann::json::object()).dump(2) + "\n");
        std::string logtxt;
        for (const auto& line : log::recent()) {
            logtxt += line;
            logtxt += '\n';
        }
        files.emplace_back("fjarr-diagnostics/log.txt", logtxt);
        return gzip(tar(files));
    }
};

std::string Server::sse_frame(const Snapshot& snap, const std::string& body) {
    const auto& m = snap.meta;
    std::string frame = "id: " + m.pipeline_id + "@" + std::to_string(m.seq) + "\nevent: snapshot\ndata: " + meta_json(m).dump();
    if (body == "json") frame += "\ndata: " + snap.json;
    else if (body == "dot") frame += "\ndata: " + escape_newlines(snap.dot);
    else if (body == "txt") frame += "\ndata: " + escape_newlines(snap.txt);
    frame += "\n\n";
    return frame;
}

Server::Server(const AgentConfig::IntrospectSection& config, SnapshotStore& store, Providers providers)
    : impl_(std::make_unique<Impl>(config, store, std::move(providers))) {
    listener_ = impl_->store.add_listener([this](const Snapshot& s) { impl_->broadcast(s); });
}

Server::~Server() {
    impl_->store.remove_listener(listener_);
    impl_->keepalive.cancel();
    for (auto& c : impl_->clients) {
        c->finished = glib::SignalConnection();
        c->disconnected = glib::SignalConnection();
        c->wrote = glib::SignalConnection();
        soup_message_body_complete(soup_server_message_get_response_body(c->msg.get()));
        soup_server_message_unpause(c->msg.get());
    }
    impl_->clients.clear();
    if (impl_->server) soup_server_disconnect(impl_->server.get());
}

std::size_t Server::event_clients() const { return impl_->clients.size(); }
std::string Server::diagnostics_bundle() const { return impl_->bundle(); }

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
    if (port_ == 0) { // ephemeral (tests): ask the server which one it took
        GSList* uris = soup_server_get_uris(impl_->server.get());
        for (GSList* l = uris; l; l = l->next) port_ = static_cast<int>(g_uri_get_port(static_cast<GUri*>(l->data)));
        g_slist_free_full(uris, reinterpret_cast<GDestroyNotify>(g_uri_unref));
    }
    log::info("introspect", "endpoint listening", {{"bind", impl_->config.bind}, {"port", std::to_string(port_)}});
}

} // namespace fjarr::introspect
