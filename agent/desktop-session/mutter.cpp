#include "mutter.hpp"

#include <gio/gunixfdlist.h>

namespace fjarr::desktop::helper {

namespace {
constexpr const char* RD = "org.gnome.Mutter.RemoteDesktop";
constexpr const char* SC = "org.gnome.Mutter.ScreenCast";
constexpr const char* DC = "org.gnome.Mutter.DisplayConfig";

std::string take_error(GError* e) {
    std::string s = e ? e->message : "unknown error";
    if (e) g_error_free(e);
    return s;
}

/// A synchronous method call; null with `error` set on failure. The caller unrefs the result.
GVariant* call(GDBusConnection* bus, const char* dest, const std::string& path, const std::string& iface, const char* method,
               GVariant* params, const char* reply_type, std::string* error) {
    GError* e = nullptr;
    GVariant* r = g_dbus_connection_call_sync(bus, dest, path.c_str(), iface.c_str(), method, params,
                                              reply_type ? G_VARIANT_TYPE(reply_type) : nullptr, G_DBUS_CALL_FLAGS_NONE, 5000, nullptr, &e);
    if (!r && error) *error = std::string(method) + ": " + take_error(e);
    else if (!r) take_error(e);
    return r;
}

std::uint32_t cursor_mode(const std::string& cursor) {
    if (cursor == "hidden") return 0;
    if (cursor == "metadata") return 2;
    return 1; // embedded
}
} // namespace

std::vector<MonitorInfo> current_monitors(GDBusConnection* bus, std::string* error) {
    // (serial, monitors a((ssss) a(siiddada{sv}) a{sv}), logical a(iiduba(ssss)a{sv}), props a{sv})
    std::vector<MonitorInfo> out;
    GVariant* r = call(bus, DC, "/org/gnome/Mutter/DisplayConfig", DC, "GetCurrentState", nullptr,
                       "(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})", error);
    if (!r) return out;

    struct Physical {
        std::string vendor, product, serial, name;
        int w = 0, h = 0;
    };
    std::map<std::string, Physical> physical; // by connector
    GVariant* monitors = g_variant_get_child_value(r, 1);
    for (gsize i = 0; i < g_variant_n_children(monitors); i++) {
        GVariant* mon = g_variant_get_child_value(monitors, i);
        const gchar *connector, *vendor, *product, *serial;
        g_variant_get_child(mon, 0, "(&s&s&s&s)", &connector, &vendor, &product, &serial);
        Physical p{vendor, product, serial, "", 0, 0};
        GVariant* modes = g_variant_get_child_value(mon, 1);
        for (gsize k = 0; k < g_variant_n_children(modes); k++) {
            GVariant* mode = g_variant_get_child_value(modes, k);
            GVariant* mprops = g_variant_get_child_value(mode, 6);
            gboolean current = FALSE;
            g_variant_lookup(mprops, "is-current", "b", &current);
            if (current) {
                g_variant_get_child(mode, 1, "i", &p.w);
                g_variant_get_child(mode, 2, "i", &p.h);
            }
            g_variant_unref(mprops);
            g_variant_unref(mode);
        }
        g_variant_unref(modes);
        GVariant* props = g_variant_get_child_value(mon, 2);
        const gchar* display_name = nullptr;
        if (g_variant_lookup(props, "display-name", "&s", &display_name)) p.name = display_name;
        g_variant_unref(props);
        physical[connector] = p;
        g_variant_unref(mon);
    }
    g_variant_unref(monitors);

    GVariant* logical = g_variant_get_child_value(r, 2);
    for (gsize i = 0; i < g_variant_n_children(logical); i++) {
        GVariant* lm = g_variant_get_child_value(logical, i);
        gint x = 0, y = 0;
        gdouble scale = 1.0;
        gboolean primary = FALSE;
        g_variant_get_child(lm, 0, "i", &x);
        g_variant_get_child(lm, 1, "i", &y);
        g_variant_get_child(lm, 2, "d", &scale);
        g_variant_get_child(lm, 4, "b", &primary);
        GVariant* specs = g_variant_get_child_value(lm, 5);
        for (gsize k = 0; k < g_variant_n_children(specs); k++) {
            const gchar *connector, *vendor, *product, *serial;
            g_variant_get_child(specs, k, "(&s&s&s&s)", &connector, &vendor, &product, &serial);
            auto p = physical.find(connector);
            if (p == physical.end()) continue;
            MonitorInfo m;
            m.connector = connector;
            m.vendor = p->second.vendor;
            m.product = p->second.product;
            m.serial = p->second.serial;
            m.name = p->second.name;
            m.x = x;
            m.y = y;
            m.width = p->second.w;
            m.height = p->second.h;
            m.scale = scale;
            m.primary = primary;
            m.is_virtual = std::string(connector).rfind("Meta-", 0) == 0; // mutter's own virtual monitors
            out.push_back(m);
        }
        g_variant_unref(specs);
        g_variant_unref(lm);
    }
    g_variant_unref(logical);
    g_variant_unref(r);
    return out;
}

guint watch_monitors(GDBusConnection* bus, std::function<void()> changed) {
    auto* cb = new std::function<void()>(std::move(changed));
    return g_dbus_connection_signal_subscribe(
        bus, DC, DC, "MonitorsChanged", "/org/gnome/Mutter/DisplayConfig", nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer d) {
            (*static_cast<std::function<void()>*>(d))();
        },
        cb, [](gpointer d) { delete static_cast<std::function<void()>*>(d); });
}

RemoteDesktop::RemoteDesktop(GDBusConnection* bus) : bus_(bus) { g_object_ref(bus_); }

RemoteDesktop::~RemoteDesktop() {
    stop();
    g_object_unref(bus_);
}

bool RemoteDesktop::ensure_sessions(std::string* error) {
    if (!rd_path_.empty()) return true;
    GVariant* r = call(bus_, RD, "/org/gnome/Mutter/RemoteDesktop", RD, "CreateSession", nullptr, "(o)", error);
    if (!r) return false;
    const gchar* path;
    g_variant_get(r, "(&o)", &path);
    rd_path_ = path;
    g_variant_unref(r);

    // The SessionId property links the ScreenCast session to this one.
    GVariant* id = call(bus_, RD, rd_path_, "org.freedesktop.DBus.Properties", "Get",
                        g_variant_new("(ss)", "org.gnome.Mutter.RemoteDesktop.Session", "SessionId"), "(v)", error);
    if (!id) return false;
    GVariant* v;
    g_variant_get(id, "(v)", &v);
    const std::string session_id = g_variant_get_string(v, nullptr);
    g_variant_unref(v);
    g_variant_unref(id);

    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&opts, "{sv}", "remote-desktop-session-id", g_variant_new_string(session_id.c_str()));
    r = call(bus_, SC, "/org/gnome/Mutter/ScreenCast", SC, "CreateSession", g_variant_new("(a{sv})", &opts), "(o)", error);
    if (!r) return false;
    g_variant_get(r, "(&o)", &path);
    sc_path_ = path;
    g_variant_unref(r);

    subscriptions_.push_back(g_dbus_connection_signal_subscribe(
        bus_, RD, "org.gnome.Mutter.RemoteDesktop.Session", "Closed", rd_path_.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer d) {
            auto* self = static_cast<RemoteDesktop*>(d);
            self->rd_path_.clear();
            self->sc_path_.clear();
            self->started_ = false;
            if (self->closed_) self->closed_();
        },
        this, nullptr));
    return true;
}

void RemoteDesktop::record(const std::string& connector, const std::string& cursor, Recorded done) {
    std::string error;
    if (!ensure_sessions(&error)) {
        done(false, {}, error);
        return;
    }
    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&props, "{sv}", "cursor-mode", g_variant_new_uint32(cursor_mode(cursor)));
    GVariant* r = call(bus_, SC, sc_path_, "org.gnome.Mutter.ScreenCast.Session", "RecordMonitor",
                       g_variant_new("(sa{sv})", connector.c_str(), &props), "(o)", &error);
    if (!r) {
        done(false, {}, error);
        return;
    }
    const gchar* stream;
    g_variant_get(r, "(&o)", &stream);
    const std::string stream_path = stream;
    g_variant_unref(r);
    pending_[stream_path] = Pending{stream_path, std::move(done)};

    struct Ctx {
        RemoteDesktop* self;
        std::string stream_path;
    };
    subscriptions_.push_back(g_dbus_connection_signal_subscribe(
        bus_, SC, "org.gnome.Mutter.ScreenCast.Stream", "PipeWireStreamAdded", stream_path.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* params, gpointer d) {
            auto* ctx = static_cast<Ctx*>(d);
            auto it = ctx->self->pending_.find(ctx->stream_path);
            if (it == ctx->self->pending_.end()) return;
            StreamInfo info;
            g_variant_get(params, "(u)", &info.node);
            std::string err;
            GVariant* p = call(ctx->self->bus_, SC, ctx->stream_path, "org.freedesktop.DBus.Properties", "Get",
                               g_variant_new("(ss)", "org.gnome.Mutter.ScreenCast.Stream", "Parameters"), "(v)", &err);
            if (p) {
                GVariant* dict;
                g_variant_get(p, "(v)", &dict);
                g_variant_lookup(dict, "position", "(ii)", &info.x, &info.y);
                g_variant_lookup(dict, "size", "(ii)", &info.width, &info.height);
                g_variant_unref(dict);
                g_variant_unref(p);
            }
            auto done = std::move(it->second.done);
            ctx->self->pending_.erase(it);
            done(true, info, "");
        },
        new Ctx{this, stream_path}, [](gpointer d) { delete static_cast<Ctx*>(d); }));

    if (!started_) {
        if (GVariant* s = call(bus_, RD, rd_path_, "org.gnome.Mutter.RemoteDesktop.Session", "Start", nullptr, nullptr, &error)) {
            g_variant_unref(s);
            started_ = true;
        } else {
            auto it = pending_.find(stream_path);
            if (it != pending_.end()) {
                auto d = std::move(it->second.done);
                pending_.erase(it);
                d(false, {}, error);
            }
        }
    }
}

int RemoteDesktop::connect_eis(std::string* error) {
    if (!started_) {
        if (error) *error = "the remote-desktop session has not started";
        return -1;
    }
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    GError* e = nullptr;
    GUnixFDList* out_fds = nullptr;
    GVariant* r = g_dbus_connection_call_with_unix_fd_list_sync(bus_, RD, rd_path_.c_str(), "org.gnome.Mutter.RemoteDesktop.Session", "ConnectToEIS",
                                                               g_variant_new("(a{sv})", &opts), G_VARIANT_TYPE("(h)"), G_DBUS_CALL_FLAGS_NONE, 5000,
                                                               nullptr, &out_fds, nullptr, &e);
    if (!r) {
        if (error) *error = "ConnectToEIS: " + take_error(e);
        else take_error(e);
        return -1;
    }
    gint32 handle = -1;
    g_variant_get(r, "(h)", &handle);
    g_variant_unref(r);
    const int fd = g_unix_fd_list_get(out_fds, handle, &e);
    g_object_unref(out_fds);
    if (fd < 0 && error) *error = "ConnectToEIS: " + take_error(e);
    return fd;
}

void RemoteDesktop::stop() {
    for (guint id : subscriptions_) g_dbus_connection_signal_unsubscribe(bus_, id);
    subscriptions_.clear();
    pending_.clear();
    if (!rd_path_.empty()) {
        if (GVariant* r = call(bus_, RD, rd_path_, "org.gnome.Mutter.RemoteDesktop.Session", "Stop", nullptr, nullptr, nullptr)) g_variant_unref(r);
    }
    rd_path_.clear();
    sc_path_.clear();
    started_ = false;
}

} // namespace fjarr::desktop::helper
