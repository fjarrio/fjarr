// spec:
// docs/24-pipeline-introspection.md#from-the-dashboard-the-fjarrintrospect-capability
#include "introspect_capability.hpp"

#include <fjarr/errors.hpp>

#include "core/log.hpp"

namespace fjarr::capabilities {

namespace {
const char *media_type(const std::string &form) { return form == "dot" ? "text/vnd.graphviz" : "application/json"; }
} // namespace

IntrospectCapability::IntrospectCapability(std::function<introspect::SnapshotStore *()> store, std::function<nlohmann::json()> stats)
    : store_(std::move(store)),
      stats_(std::move(stats)) {}

IntrospectCapability::~IntrospectCapability() {
    if (listener_ && store_ && store_()) store_()->remove_listener(listener_);
}

CapabilityManifest IntrospectCapability::manifest() const {
    CapabilityManifest m;
    m.name = "fjarr.introspect";
    m.version = {0, 1, 0};
    m.channels = {{ChannelClass::Control}, {ChannelClass::Bulk, BulkFraming::Blob}};
    m.config_schema =
        nlohmann::json{{"type", "object"}, {"properties", {{"enabled", {{"type", "boolean"}}}}}, {"additionalProperties", false}};
    m.consumers.peer = true;
    return m;
}

void IntrospectCapability::ensure_listening() {
    if (listener_ || !store_ || !store_()) return;
    listener_ = store_()->add_listener([this](const introspect::Snapshot &s) { on_snapshot(s); });
}

void IntrospectCapability::session_attached(SessionContext &ctx, const nlohmann::json &) {
    ensure_listening();
    sessions_[ctx.id()] = Attached{&ctx};
}

void IntrospectCapability::session_detached(const SessionId &id, DetachReason, std::string_view) { sessions_.erase(id); }

void IntrospectCapability::shutdown() {
    if (listener_ && store_ && store_()) store_()->remove_listener(listener_);
    listener_ = 0;
    sessions_.clear();
}

std::set<std::string> IntrospectCapability::parse_forms(const nlohmann::json &payload) {
    std::set<std::string> forms{"txt", "json", "dot"};
    if (!payload.contains("forms")) return forms;
    if (!payload["forms"].is_array()) throw FjarrError(std::string(error_codes::payload_invalid), "forms must be an array of txt|json|dot");
    forms.clear();
    for (const auto &f : payload["forms"]) {
        const std::string s = f.is_string() ? f.get<std::string>() : "";
        if (s != "txt" && s != "json" && s != "dot") throw FjarrError(std::string(error_codes::payload_invalid), "unknown form: " + s);
        forms.insert(s);
    }
    return forms;
}

nlohmann::json IntrospectCapability::snapshot_payload(Attached &a, const introspect::Snapshot &snap, const std::set<std::string> &forms,
                                                      const std::string &pipeline_id) {
    nlohmann::json payload = introspect::meta_json(snap.meta);
    if (forms.count("txt")) payload["txt"] = snap.txt;
    for (const char *form : {"json", "dot"}) {
        if (!forms.count(form)) continue;
        const std::string &body = form == std::string("json") ? snap.json : snap.dot;
        if (body.empty())
            continue; // DOT is kept for the last few snapshots only (docs/24
                      // retention): omitted, not empty
        const SessionId sid = a.ctx->id();
        a.in_flight[pipeline_id]++;
        const auto ref = a.ctx->send_blob(body, media_type(form), [this, sid, pipeline_id](bool ok) {
            auto it = sessions_.find(sid);
            if (it == sessions_.end()) return; // the session ended: the pump reported false after detach
            auto &s = it->second;
            if (--s.in_flight[pipeline_id] > 0) return;
            s.in_flight.erase(pipeline_id);
            auto held = s.held.find(pipeline_id);
            if (held == s.held.end()) return;
            auto next = std::move(held->second);
            s.held.erase(held);
            if (ok)
                deliver(sid, std::move(next)); // newest-wins: the snapshot that
                                               // waited is the current one
        });
        payload[form] = ref.to_json();
    }
    return payload;
}

void IntrospectCapability::deliver(const SessionId &id, std::shared_ptr<const introspect::Snapshot> snap) {
    auto it = sessions_.find(id);
    if (it == sessions_.end() || !it->second.sub) return;
    Attached &a = it->second;
    const std::string pipeline_id = snap->meta.pipeline_id;
    if (a.in_flight.count(pipeline_id)) { // blobs of this pipeline still pumping:
                                          // hold the newest only
        if (a.held.count(pipeline_id)) coalesced_++;
        a.held[pipeline_id] = std::move(snap);
        return;
    }
    try {
        const auto payload = snapshot_payload(a, *snap, a.sub->forms, pipeline_id);
        a.ctx->event("snapshot",
                     payload); // the envelope leaves before the first chunk (docs/08)
    } catch (const std::exception &e) {
        log::warn("introspect", "snapshot not delivered", {{"session", id}, {"pipeline", pipeline_id}, {"error", e.what()}});
    }
}

void IntrospectCapability::on_snapshot(const introspect::Snapshot &snap) {
    auto *store = store_();
    if (!store) return;
    auto latest = store->at(snap.meta.pipeline_id, snap.meta.seq);
    if (!latest) return;
    std::vector<SessionId> ids;
    for (const auto &[id, a] : sessions_)
        if (a.sub && a.sub->matches(snap.meta.pipeline_id)) ids.push_back(id);
    for (const auto &id : ids) deliver(id, latest);
}

void IntrospectCapability::on_message(SessionContext &ctx, const Envelope &msg) {
    if (msg.kind != "request") return;
    auto *store = store_();
    if (!store) throw FjarrError(std::string(error_codes::internal), "introspection is disabled on this agent");
    auto it = sessions_.find(ctx.id());
    if (it == sessions_.end()) throw FjarrError(std::string(error_codes::internal), "session not attached");
    Attached &a = it->second;
    const auto &p = msg.payload;

    if (msg.type == "pipelines/list") {
        nlohmann::json list = nlohmann::json::array();
        for (const auto &m : store->list()) list.push_back(introspect::meta_json(m));
        ctx.result(msg, nlohmann::json{{"ok", true}, {"pipelines", list}});
        return;
    }
    if (msg.type == "pipelines/subscribe") {
        Subscription sub;
        if (p.contains("pipeline_id") && p["pipeline_id"].is_string() && !p["pipeline_id"].get<std::string>().empty())
            sub.filter = p["pipeline_id"].get<std::string>();
        sub.forms = parse_forms(p);
        a.sub = sub;
        a.held.clear();
        nlohmann::json list = nlohmann::json::array();
        for (const auto &m : store->list()) list.push_back(introspect::meta_json(m));
        ctx.result(msg, nlohmann::json{{"ok", true}, {"pipelines", list}});
        // Replay the current state: one snapshot event per matching pipeline (its
        // latest), so a subscriber never has to ask twice for "what does it look
        // like now".
        if (!p.value("replay", true)) return;
        for (const auto &m : store->list())
            if (sub.matches(m.pipeline_id))
                if (auto latest = store->latest(m.pipeline_id)) deliver(ctx.id(), latest);
        return;
    }
    if (msg.type == "pipelines/unsubscribe") {
        a.sub.reset();
        a.held.clear();
        ctx.result(msg, nlohmann::json{{"ok", true}});
        return;
    }
    if (msg.type == "pipelines/history") {
        const std::string id = p.value("pipeline_id", "");
        const unsigned from = p.value("seq_from", 0u);
        nlohmann::json list = nlohmann::json::array();
        for (const auto &m : store->history(id))
            if (m.seq >= from) list.push_back(introspect::meta_json(m));
        if (list.empty() && !store->latest(id)) throw FjarrError(std::string(error_codes::payload_invalid), "unknown pipeline: " + id);
        ctx.result(msg, nlohmann::json{{"ok", true}, {"snapshots", list}});
        return;
    }
    if (msg.type == "pipelines/snapshot") {
        const std::string id = p.value("pipeline_id", "");
        std::shared_ptr<const introspect::Snapshot> snap = p.contains("seq") ? store->at(id, p.value("seq", 0u)) : store->latest(id);
        if (!snap)
            throw FjarrError(std::string(error_codes::payload_invalid),
                             "no such snapshot: " + id + "@" + std::to_string(p.value("seq", 0u)));
        const auto forms = parse_forms(p);
        const auto payload = snapshot_payload(a, *snap, forms, id);
        ctx.result(msg, nlohmann::json{{"ok", true}, {"snapshot", payload}}); // the result leaves before the first chunk
        return;
    }
    if (msg.type == "stats") {
        ctx.result(msg, nlohmann::json{{"ok", true}, {"stats", stats_ ? stats_() : nlohmann::json::object()}});
        return;
    }
    throw FjarrError(std::string(error_codes::payload_invalid), "unknown fjarr.introspect request: " + msg.type);
}

} // namespace fjarr::capabilities
