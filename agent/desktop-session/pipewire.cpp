// Narrowing as the portal's OpenPipeWireRemote does, proven for an ordinary session process in
// spikes/pipewire-narrowing (2026-10-01).
// spec: docs/23-agent-core-architecture.md#desktop-descriptor-handover
#include "pipewire.hpp"

#include <cstring>

#include <pipewire/pipewire.h>

namespace fjarr::desktop::helper {
namespace {

/// One short-lived connection, driven synchronously on its own loop with a deadline per round trip.
struct Connection {
    pw_main_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    spa_hook core_listener{};
    spa_source* timer = nullptr;
    int pending = -1;
    bool done = false, timed_out = false;
    std::string error;

    ~Connection() {
        if (core) {
            spa_hook_remove(&core_listener);
            pw_core_disconnect(core);
        }
        if (context) pw_context_destroy(context);
        if (loop) pw_main_loop_destroy(loop);
    }

    /// Everything sent so far has been processed by the server, or false (with `error`).
    bool roundtrip(int timeout_ms = 2000) {
        done = timed_out = false;
        pending = pw_core_sync(core, PW_ID_CORE, 0);
        timespec ts{timeout_ms / 1000, (timeout_ms % 1000) * 1000000L};
        pw_loop_update_timer(pw_main_loop_get_loop(loop), timer, &ts, nullptr, false);
        pw_main_loop_run(loop);
        if (timed_out && error.empty()) error = "PipeWire did not answer within " + std::to_string(timeout_ms) + " ms";
        return done && error.empty();
    }
};

const pw_core_events CORE_EVENTS = [] {
    pw_core_events e{};
    e.version = PW_VERSION_CORE_EVENTS;
    e.done = [](void* d, uint32_t id, int seq) {
        auto* c = static_cast<Connection*>(d);
        if (id == PW_ID_CORE && seq == c->pending) {
            c->done = true;
            pw_main_loop_quit(c->loop);
        }
    };
    e.error = [](void* d, uint32_t id, int, int res, const char* message) {
        auto* c = static_cast<Connection*>(d);
        if (id != PW_ID_CORE) return; // an object's own error is not the connection's
        c->error = std::string("PipeWire: ") + (message ? message : std::strerror(-res));
        pw_main_loop_quit(c->loop);
    };
    return e;
}();

struct FactoryLookup {
    std::uint32_t id = SPA_ID_INVALID;
};

const pw_registry_events REGISTRY_EVENTS = [] {
    pw_registry_events e{};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global = [](void* d, uint32_t id, uint32_t, const char* type, uint32_t, const spa_dict* props) {
        if (std::strcmp(type, PW_TYPE_INTERFACE_Factory) != 0 || !props) return;
        const char* name = spa_dict_lookup(props, PW_KEY_FACTORY_NAME);
        if (name && std::strcmp(name, "client-node") == 0) static_cast<FactoryLookup*>(d)->id = id;
    };
    return e;
}();

} // namespace

int open_narrowed_pipewire(std::uint32_t node, std::string* error) {
    static const bool initialized = [] {
        pw_init(nullptr, nullptr);
        return true;
    }();
    (void)initialized;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return -1;
    };
    Connection c;
    c.loop = pw_main_loop_new(nullptr);
    c.context = c.loop ? pw_context_new(pw_main_loop_get_loop(c.loop), nullptr, 0) : nullptr;
    if (!c.context) return fail("cannot create a PipeWire context");
    c.timer = pw_loop_add_timer(pw_main_loop_get_loop(c.loop), [](void* d, uint64_t) {
        auto* conn = static_cast<Connection*>(d);
        conn->timed_out = true;
        pw_main_loop_quit(conn->loop);
    }, &c);
    c.core = pw_context_connect(c.context, nullptr, 0); // $PIPEWIRE_REMOTE or pipewire-0 in $XDG_RUNTIME_DIR, as this user
    if (!c.core) return fail(std::string("cannot connect to PipeWire: ") + std::strerror(errno));
    pw_core_add_listener(c.core, &c.core_listener, &CORE_EVENTS, &c);

    // The client-node factory's id: a capture stream cannot be made without it.
    FactoryLookup factory;
    auto* registry = pw_core_get_registry(c.core, PW_VERSION_REGISTRY, 0);
    spa_hook registry_listener{};
    pw_registry_add_listener(registry, &registry_listener, &REGISTRY_EVENTS, &factory);
    const bool listed = c.roundtrip();
    spa_hook_remove(&registry_listener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
    if (!listed) return fail(c.error);
    if (factory.id == SPA_ID_INVALID) return fail("PipeWire has no client-node factory");

    const pw_permission permissions[] = {
        PW_PERMISSION_INIT(PW_ID_CORE, PW_PERM_R | PW_PERM_X),
        PW_PERMISSION_INIT(node, PW_PERM_R | PW_PERM_X),
        PW_PERMISSION_INIT(factory.id, PW_PERM_R | PW_PERM_X),
        PW_PERMISSION_INIT(PW_ID_ANY, 0), // everything else, and everything created later
    };
    pw_client_update_permissions(pw_core_get_client(c.core), SPA_N_ELEMENTS(permissions), permissions);
    if (!c.roundtrip()) return fail("narrowing the PipeWire connection failed: " + c.error); // never hand over an unnarrowed one
    const int fd = pw_core_steal_fd(c.core);
    if (fd < 0) return fail("cannot take the PipeWire connection's descriptor");
    return fd;
}

} // namespace fjarr::desktop::helper
