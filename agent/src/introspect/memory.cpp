#include "memory.hpp"

#include <algorithm>
#include <chrono>
#include <deque>
#include <map>
#include <set>
#include <vector>
#include <cstdio>
#include <unistd.h>

#include <gst/gst.h>
#include <gst/gsttracer.h>

#include "core/glib/raii.hpp"
#include "core/log.hpp"

namespace fjarr::introspect {

namespace {

GstTracer* leaks_tracer() {
    // Tracers are never unloaded, so the borrowed pointer stays valid for the process lifetime.
    GList* tracers = gst_tracing_get_active_tracers(); // transfer full
    GstTracer* found = nullptr;
    for (GList* l = tracers; l; l = l->next) {
        auto* t = static_cast<GstTracer*>(l->data);
        if (std::string(G_OBJECT_TYPE_NAME(t)) == "GstLeaksTracer") found = t;
    }
    g_list_free_full(tracers, gst_object_unref); // NOLINT: the list's own refs, not a wrapper's
    return found;
}

std::int64_t now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}

nlohmann::json diff_numbers(const nlohmann::json& then, const nlohmann::json& now) {
    if (now.is_object() && then.is_object()) {
        nlohmann::json out = nlohmann::json::object();
        for (auto it = now.begin(); it != now.end(); ++it) {
            if (!then.contains(it.key())) continue;
            nlohmann::json d = diff_numbers(then[it.key()], it.value());
            if (!d.is_null()) out[it.key()] = d;
        }
        return out;
    }
    if (now.is_number_integer() && then.is_number_integer()) return now.get<std::int64_t>() - then.get<std::int64_t>();
    return nullptr;
}

/// The tracer's checkpoint lists: `objects-created-list` / `objects-removed-list`, each a
/// GstValueList of structures `objects-created, type-name=(string)GstPad, address=(string)0x…`,
/// rendered as "GstPad@0x…".
std::vector<std::string> objects_of(const GstStructure* s, const char* field) {
    std::vector<std::string> out;
    const GValue* v = s ? gst_structure_get_value(s, field) : nullptr;
    if (!v || !GST_VALUE_HOLDS_LIST(v)) return out;
    const guint n = gst_value_list_get_size(v);
    for (guint i = 0; i < n; i++) {
        const GValue* item = gst_value_list_get_value(v, i);
        if (!GST_VALUE_HOLDS_STRUCTURE(item)) continue;
        const GstStructure* o = gst_value_get_structure(item);
        const char* type = gst_structure_get_string(o, "type-name");
        const char* addr = gst_structure_get_string(o, "address");
        out.push_back(std::string(type ? type : "?") + "@" + (addr ? addr : "?"));
    }
    return out;
}

// Process-global tracer bookkeeping (the tracer's window is global: reading it resets it, and
// `get-live-objects` must never be used in a live process — it takes the objects' references).
// "created and still alive since X" = the multiset of created − removed, accumulated across
// every read, minus the multiset that was alive at checkpoint X: idempotent, instance-independent.
struct TracerLedger {
    bool started = false;
    std::multiset<std::string> alive;                                  // created − removed so far
    std::map<std::string, std::multiset<std::string>> at_checkpoint;   // token → alive at that point
    std::deque<std::string> order;                                     // bounded like the census checkpoints
};
TracerLedger& ledger() {
    static TracerLedger l;
    return l;
}
/// Merge the tracer's current window into the ledger (and reset the window).
void ledger_sync(GstTracer* t) {
    TracerLedger& l = ledger();
    if (!l.started) {
        g_signal_emit_by_name(t, "activity-start-tracking");
        l.started = true;
    }
    GstStructure* raw = nullptr;
    g_signal_emit_by_name(t, "activity-get-checkpoint", &raw);
    glib::GstStructurePtr s(raw);
    for (auto& c : objects_of(s.get(), "objects-created-list")) l.alive.insert(std::move(c));
    for (const auto& r : objects_of(s.get(), "objects-removed-list")) {
        auto it = l.alive.find(r);
        if (it != l.alive.end()) l.alive.erase(it); // removed since a checkpoint saw it created
    }
}

} // namespace

MemoryCensus::MemoryCensus(Extra extra) : extra_(std::move(extra)) {}

std::uint64_t MemoryCensus::rss_bytes() {
    FILE* f = std::fopen("/proc/self/statm", "r");
    if (!f) return 0;
    unsigned long size = 0, resident = 0;
    const int n = std::fscanf(f, "%lu %lu", &size, &resident);
    std::fclose(f);
    if (n != 2) return 0;
    return static_cast<std::uint64_t>(resident) * static_cast<std::uint64_t>(sysconf(_SC_PAGESIZE));
}

bool MemoryCensus::leaks_tracer_active() { return leaks_tracer() != nullptr; }

nlohmann::json MemoryCensus::now() const {
    const auto& c = glib::ObjectCensus::instance();
    nlohmann::json j{
        {"ts", now_ms()},
        {"rss_bytes", rss_bytes()},
        {"census",
         {{"elements", c.elements.load()},
          {"pads", c.pads.load()},
          {"samples", c.samples.load()},
          {"buffers", c.buffers.load()},
          {"promises", c.promises.load()},
          {"sources", c.sources.load()},
          {"signals", c.signals.load()},
          {"probes", c.probes.load()},
          {"sessions", c.sessions.load()},
          {"pipelines", c.pipelines.load()}}},
        {"leaks_tracer", leaks_tracer_active()},
    };
    if (extra_) {
        nlohmann::json e = extra_();
        for (auto it = e.begin(); it != e.end(); ++it) j[it.key()] = it.value();
    }
    return j;
}

nlohmann::json MemoryCensus::checkpoint() {
    nlohmann::json report = now();
    const std::string token = "cp-" + std::to_string(next_token_++);
    report["checkpoint"] = token;
    if (GstTracer* t = leaks_tracer()) {
        ledger_sync(t);
        TracerLedger& l = ledger();
        l.at_checkpoint[token] = l.alive;
        l.order.push_back(token);
        while (l.order.size() > 16) {
            l.at_checkpoint.erase(l.order.front());
            l.order.pop_front();
        }
    }
    checkpoints_.emplace_back(token, report);
    while (checkpoints_.size() > 16) checkpoints_.pop_front();
    log::info("introspect", "memory checkpoint", {{"token", token}, {"rss_bytes", std::to_string(report["rss_bytes"].get<std::uint64_t>())}});
    return report;
}

nlohmann::json MemoryCensus::since(const std::string& token) const {
    const nlohmann::json* then = nullptr;
    for (const auto& [t, r] : checkpoints_)
        if (t == token) then = &r;
    if (!then) return nlohmann::json{{"error", "unknown checkpoint"}, {"since", token}};
    nlohmann::json current = now();
    nlohmann::json out{{"since", token}, {"then", *then}, {"now", current}, {"diff", diff_numbers(*then, current)}};
    if (GstTracer* t = leaks_tracer()) {
        // Objects created since the checkpoint and still alive: alive now − alive then (multisets, so
        // an address reused by a new object counts once). Idempotent: reading merges, never resets.
        ledger_sync(t);
        TracerLedger& l = ledger();
        auto at = l.at_checkpoint.find(token);
        nlohmann::json created = nlohmann::json::array();
        if (at != l.at_checkpoint.end()) {
            std::multiset<std::string> then = at->second;
            for (const auto& o : l.alive) {
                auto it = then.find(o);
                if (it != then.end()) then.erase(it);
                else created.push_back(o);
            }
        }
        out["leaks"] = {{"created", created}, {"alive", l.alive.size()}, {"at_checkpoint", at != l.at_checkpoint.end() ? at->second.size() : 0}};
    }
    return out;
}

} // namespace fjarr::introspect
