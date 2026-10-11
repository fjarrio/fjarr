// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol (Sound) · #desktop-audio
#include "sound.hpp"

#include <cerrno>
#include <cstring>
#include <map>

#include <glib-unix.h>
#include <glib.h>
#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>

#include "desktop/default_sink.hpp"

namespace fjarr::desktop::helper {

struct SoundWatch::Impl {
    std::function<void()> changed;
    pw_loop* loop = nullptr;
    pw_context* context = nullptr;
    pw_core* core = nullptr;
    pw_registry* registry = nullptr;
    pw_metadata* metadata = nullptr;
    std::uint32_t metadata_id = SPA_ID_INVALID;
    spa_hook core_listener{}, registry_listener{}, metadata_listener{};
    guint fd_watch = 0, retry = 0, reconnect = 0;
    std::map<std::uint32_t, Sink> sinks;
    std::string default_name;

    ~Impl() {
        if (retry) g_source_remove(retry);
        if (reconnect) g_source_remove(reconnect);
        disconnect();
        if (fd_watch) g_source_remove(fd_watch);
        if (context) pw_context_destroy(context);
        if (loop) pw_loop_destroy(loop);
    }

    void connect() {
        core = pw_context_connect(context, nullptr, 0); // $XDG_RUNTIME_DIR/pipewire-0, as this user
        if (!core) {
            retry_later();
            return;
        }
        static const pw_core_events core_events = [] {
            pw_core_events e{};
            e.version = PW_VERSION_CORE_EVENTS;
            e.error = [](void* d, uint32_t id, int, int res, const char*) {
                auto* self = static_cast<Impl*>(d);
                // The connection itself broke (PipeWire restarted): start over, outside this callback.
                if (id != PW_ID_CORE || res != -EPIPE || self->reconnect) return;
                self->reconnect = g_idle_add([](gpointer p) {
                    auto* s = static_cast<Impl*>(p);
                    s->reconnect = 0;
                    s->disconnect();
                    s->changed();
                    s->retry_later();
                    return G_SOURCE_REMOVE;
                }, self);
            };
            return e;
        }();
        pw_core_add_listener(core, &core_listener, &core_events, this);
        static const pw_registry_events registry_events = [] {
            pw_registry_events e{};
            e.version = PW_VERSION_REGISTRY_EVENTS;
            e.global = [](void* d, uint32_t id, uint32_t, const char* type, uint32_t, const spa_dict* props) { static_cast<Impl*>(d)->on_global(id, type, props); };
            e.global_remove = [](void* d, uint32_t id) { static_cast<Impl*>(d)->on_global_remove(id); };
            return e;
        }();
        registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
        pw_registry_add_listener(registry, &registry_listener, &registry_events, this);
    }

    void retry_later() {
        if (retry) return;
        retry = g_timeout_add_seconds(2, [](gpointer d) {
            auto* self = static_cast<Impl*>(d);
            self->retry = 0;
            self->connect();
            return G_SOURCE_REMOVE;
        }, this);
    }

    void disconnect() {
        if (metadata) {
            spa_hook_remove(&metadata_listener);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(metadata));
            metadata = nullptr;
        }
        metadata_id = SPA_ID_INVALID;
        if (registry) {
            spa_hook_remove(&registry_listener);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry));
            registry = nullptr;
        }
        if (core) {
            spa_hook_remove(&core_listener);
            pw_core_disconnect(core);
            core = nullptr;
        }
        sinks.clear();
        default_name.clear();
    }

    void on_global(std::uint32_t id, const char* type, const spa_dict* props) {
        if (!props) return;
        if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
            const char* cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
            if (!cls || std::strcmp(cls, "Audio/Sink") != 0) return;
            const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
            const char* desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
            sinks[id] = Sink{id, name ? name : "", desc ? desc : (name ? name : "")};
            changed();
        } else if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 && !metadata) {
            const char* name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
            if (!name || std::strcmp(name, "default") != 0) return;
            metadata = static_cast<pw_metadata*>(pw_registry_bind(registry, id, type, PW_VERSION_METADATA, 0));
            if (!metadata) return;
            metadata_id = id;
            static const pw_metadata_events metadata_events = [] {
                pw_metadata_events e{};
                e.version = PW_VERSION_METADATA_EVENTS;
                e.property = [](void* d, uint32_t subject, const char* key, const char*, const char* value) -> int {
                    static_cast<Impl*>(d)->on_property(subject, key, value);
                    return 0;
                };
                return e;
            }();
            pw_metadata_add_listener(metadata, &metadata_listener, &metadata_events, this);
        }
    }

    void on_property(std::uint32_t subject, const char* key, const char* value) {
        if (subject != PW_ID_CORE) return;
        if (!key) default_name.clear(); // every key was cleared
        else if (std::strcmp(key, "default.audio.sink") == 0) default_name = default_sink_name(value);
        else return;
        changed();
    }

    void on_global_remove(std::uint32_t id) {
        if (id == metadata_id) {
            spa_hook_remove(&metadata_listener);
            pw_proxy_destroy(reinterpret_cast<pw_proxy*>(metadata));
            metadata = nullptr;
            metadata_id = SPA_ID_INVALID;
            default_name.clear();
            changed();
        } else if (sinks.erase(id)) {
            changed();
        }
    }
};

SoundWatch::SoundWatch(std::function<void()> changed) : impl_(std::make_unique<Impl>()) {
    static const bool initialized = [] {
        pw_init(nullptr, nullptr);
        return true;
    }();
    (void)initialized;
    impl_->changed = std::move(changed);
    impl_->loop = pw_loop_new(nullptr);
    impl_->context = impl_->loop ? pw_context_new(impl_->loop, nullptr, 0) : nullptr;
    if (!impl_->context) return; // no speakers to report, ever; the helper still serves the screen
    // PipeWire's loop runs inside GLib's: its descriptor is readable whenever one of its sources is.
    impl_->fd_watch = g_unix_fd_add(pw_loop_get_fd(impl_->loop), G_IO_IN, [](gint, GIOCondition, gpointer d) -> gboolean {
        auto* loop = static_cast<pw_loop*>(d);
        pw_loop_enter(loop);
        pw_loop_iterate(loop, 0);
        pw_loop_leave(loop);
        return G_SOURCE_CONTINUE;
    }, impl_->loop);
    impl_->connect();
}

SoundWatch::~SoundWatch() = default;

std::optional<Sink> SoundWatch::default_sink() const {
    if (impl_->default_name.empty()) return std::nullopt;
    for (const auto& [_, s] : impl_->sinks)
        if (s.name == impl_->default_name) return s;
    return std::nullopt;
}

} // namespace fjarr::desktop::helper
