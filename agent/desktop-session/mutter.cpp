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

bool make_primary(GDBusConnection* bus, const std::string& target, std::string* error) {
    GVariant* r = call(bus, DC, "/org/gnome/Mutter/DisplayConfig", DC, "GetCurrentState", nullptr,
                       "(ua((ssss)a(siiddada{sv})a{sv})a(iiduba(ssss)a{sv})a{sv})", error);
    if (!r) return false;
    guint32 serial = 0;
    g_variant_get_child(r, 0, "u", &serial);
    // The current mode of every connector: ApplyMonitorsConfig names modes by id.
    std::map<std::string, std::string> mode_of;
    GVariant* monitors = g_variant_get_child_value(r, 1);
    for (gsize i = 0; i < g_variant_n_children(monitors); i++) {
        GVariant* mon = g_variant_get_child_value(monitors, i);
        const gchar *connector, *vendor, *product, *mserial;
        g_variant_get_child(mon, 0, "(&s&s&s&s)", &connector, &vendor, &product, &mserial);
        GVariant* modes = g_variant_get_child_value(mon, 1);
        for (gsize k = 0; k < g_variant_n_children(modes); k++) {
            GVariant* mode = g_variant_get_child_value(modes, k);
            GVariant* mprops = g_variant_get_child_value(mode, 6);
            gboolean current = FALSE;
            g_variant_lookup(mprops, "is-current", "b", &current);
            if (current) {
                const gchar* id = nullptr;
                g_variant_get_child(mode, 0, "&s", &id);
                mode_of[connector] = id;
            }
            g_variant_unref(mprops);
            g_variant_unref(mode);
        }
        g_variant_unref(modes);
        g_variant_unref(mon);
    }
    g_variant_unref(monitors);
    // The same logical monitors, the primary flag moved to the one holding `target`.
    GVariantBuilder lms;
    g_variant_builder_init(&lms, G_VARIANT_TYPE("a(iiduba(ssa{sv}))"));
    bool found = false;
    GVariant* logical = g_variant_get_child_value(r, 2);
    for (gsize i = 0; i < g_variant_n_children(logical); i++) {
        GVariant* lm = g_variant_get_child_value(logical, i);
        gint x = 0, y = 0;
        gdouble scale = 1.0;
        guint32 transform = 0;
        g_variant_get_child(lm, 0, "i", &x);
        g_variant_get_child(lm, 1, "i", &y);
        g_variant_get_child(lm, 2, "d", &scale);
        g_variant_get_child(lm, 3, "u", &transform);
        GVariant* specs = g_variant_get_child_value(lm, 5);
        bool holds = false;
        GVariantBuilder mons;
        g_variant_builder_init(&mons, G_VARIANT_TYPE("a(ssa{sv})"));
        for (gsize k = 0; k < g_variant_n_children(specs); k++) {
            const gchar *connector, *vendor, *product, *mserial;
            g_variant_get_child(specs, k, "(&s&s&s&s)", &connector, &vendor, &product, &mserial);
            holds |= target == connector;
            g_variant_builder_add(&mons, "(ss@a{sv})", connector, mode_of[connector].c_str(), g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0));
        }
        found |= holds;
        g_variant_builder_add(&lms, "(iiduba(ssa{sv}))", x, y, scale, transform, holds ? TRUE : FALSE, &mons);
        g_variant_unref(specs);
        g_variant_unref(lm);
    }
    g_variant_unref(logical);
    g_variant_unref(r);
    if (!found) {
        g_variant_builder_clear(&lms);
        if (error) *error = "no monitor " + target + " in the current layout";
        return false;
    }
    constexpr guint32 PERSISTENT = 2;
    GVariant* res = call(bus, DC, "/org/gnome/Mutter/DisplayConfig", DC, "ApplyMonitorsConfig",
                         g_variant_new("(uua(iiduba(ssa{sv}))@a{sv})", serial, PERSISTENT, &lms, g_variant_new_array(G_VARIANT_TYPE("{sv}"), nullptr, 0)),
                         nullptr, error);
    if (!res) return false;
    g_variant_unref(res);
    return true;
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

// --- input: one RemoteDesktop session, no linked ScreenCast -------------------------------------
// Unlinked on purpose: its EIS absolute pointer then covers every monitor in the layout, and is
// replaced by one covering the new layout on hot-plug (docs/23#desktop-helper-protocol, measured).

InputSession::InputSession(GDBusConnection* bus) : bus_(bus) { g_object_ref(bus_); }

InputSession::~InputSession() {
    stop();
    g_object_unref(bus_);
}

bool InputSession::ensure_started(std::string* error) {
    if (!path_.empty()) return true;
    GVariant* r = call(bus_, RD, "/org/gnome/Mutter/RemoteDesktop", RD, "CreateSession", nullptr, "(o)", error);
    if (!r) return false;
    const gchar* path;
    g_variant_get(r, "(&o)", &path);
    path_ = path;
    g_variant_unref(r);
    closed_sub_ = g_dbus_connection_signal_subscribe(
        bus_, RD, "org.gnome.Mutter.RemoteDesktop.Session", "Closed", path_.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer d) {
            auto* self = static_cast<InputSession*>(d);
            self->path_.clear();
            if (self->closed_) self->closed_();
        },
        this, nullptr);
    if (GVariant* st = call(bus_, RD, path_, "org.gnome.Mutter.RemoteDesktop.Session", "Start", nullptr, nullptr, error)) {
        g_variant_unref(st);
        return true;
    }
    stop();
    return false;
}

int InputSession::connect_eis(std::string* error) {
    if (!ensure_started(error)) return -1;
    GVariantBuilder opts;
    g_variant_builder_init(&opts, G_VARIANT_TYPE("a{sv}"));
    GError* e = nullptr;
    GUnixFDList* out_fds = nullptr;
    GVariant* r = g_dbus_connection_call_with_unix_fd_list_sync(bus_, RD, path_.c_str(), "org.gnome.Mutter.RemoteDesktop.Session", "ConnectToEIS",
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

void InputSession::stop() {
    if (closed_sub_) g_dbus_connection_signal_unsubscribe(bus_, closed_sub_);
    closed_sub_ = 0;
    if (!path_.empty()) {
        if (GVariant* r = call(bus_, RD, path_, "org.gnome.Mutter.RemoteDesktop.Session", "Stop", nullptr, nullptr, nullptr)) g_variant_unref(r);
    }
    path_.clear();
}

// --- capture: one ScreenCast session per monitor ---------------------------------------------------
// A started RemoteDesktop session takes no new streams (measured), so each capture is its own
// session, started and stopped without touching any other (docs/23#desktop-helper-protocol).

Capture::Capture(GDBusConnection* bus) : bus_(bus) { g_object_ref(bus_); }

Capture::~Capture() {
    for (guint id : subscriptions_) g_dbus_connection_signal_unsubscribe(bus_, id);
    if (!path_.empty()) {
        if (GVariant* r = call(bus_, SC, path_, "org.gnome.Mutter.ScreenCast.Session", "Stop", nullptr, nullptr, nullptr)) g_variant_unref(r);
    }
    g_object_unref(bus_);
}

std::unique_ptr<Capture> Capture::start(GDBusConnection* bus, const std::string& connector, const std::string& cursor, Recorded done) {
    std::unique_ptr<Capture> c(new Capture(bus));
    std::string error;
    GVariantBuilder none;
    g_variant_builder_init(&none, G_VARIANT_TYPE("a{sv}"));
    GVariant* r = call(bus, SC, "/org/gnome/Mutter/ScreenCast", SC, "CreateSession", g_variant_new("(a{sv})", &none), "(o)", &error);
    if (!r) {
        done(false, {}, error);
        return nullptr;
    }
    const gchar* path;
    g_variant_get(r, "(&o)", &path);
    c->path_ = path;
    g_variant_unref(r);

    GVariantBuilder props;
    g_variant_builder_init(&props, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&props, "{sv}", "cursor-mode", g_variant_new_uint32(cursor_mode(cursor)));
    r = call(bus, SC, c->path_, "org.gnome.Mutter.ScreenCast.Session", "RecordMonitor", g_variant_new("(sa{sv})", connector.c_str(), &props), "(o)", &error);
    if (!r) {
        done(false, {}, error);
        return nullptr;
    }
    const gchar* stream;
    g_variant_get(r, "(&o)", &stream);
    c->stream_ = stream;
    g_variant_unref(r);
    c->done_ = std::move(done);

    Capture* self = c.get();
    c->subscriptions_.push_back(g_dbus_connection_signal_subscribe(
        bus, SC, "org.gnome.Mutter.ScreenCast.Stream", "PipeWireStreamAdded", c->stream_.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant* params, gpointer d) {
            auto* cap = static_cast<Capture*>(d);
            if (!cap->done_) return;
            StreamInfo info;
            g_variant_get(params, "(u)", &info.node);
            std::string err;
            GVariant* p = call(cap->bus_, SC, cap->stream_, "org.freedesktop.DBus.Properties", "Get",
                               g_variant_new("(ss)", "org.gnome.Mutter.ScreenCast.Stream", "Parameters"), "(v)", &err);
            if (p) {
                GVariant* dict;
                g_variant_get(p, "(v)", &dict);
                g_variant_lookup(dict, "position", "(ii)", &info.x, &info.y);
                g_variant_lookup(dict, "size", "(ii)", &info.width, &info.height);
                g_variant_unref(dict);
                g_variant_unref(p);
            }
            auto done = std::move(cap->done_);
            cap->done_ = nullptr;
            done(true, info, "");
        },
        self, nullptr));
    c->subscriptions_.push_back(g_dbus_connection_signal_subscribe(
        bus, SC, "org.gnome.Mutter.ScreenCast.Session", "Closed", c->path_.c_str(), nullptr, G_DBUS_SIGNAL_FLAGS_NONE,
        [](GDBusConnection*, const gchar*, const gchar*, const gchar*, const gchar*, GVariant*, gpointer d) {
            auto* cap = static_cast<Capture*>(d);
            cap->path_.clear();
            if (cap->closed_) cap->closed_();
        },
        self, nullptr));
    if (GVariant* st = call(bus, SC, c->path_, "org.gnome.Mutter.ScreenCast.Session", "Start", nullptr, nullptr, &error)) {
        g_variant_unref(st);
    } else {
        auto d = std::move(c->done_);
        c->done_ = nullptr;
        d(false, {}, error);
        return nullptr;
    }
    return c;
}

} // namespace fjarr::desktop::helper
