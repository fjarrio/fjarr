// Backend A: an X11 kiosk's desktop from the agent's own process (ADR-0006, ADR-0028: no session
// helper; the kiosk session grants the agent's account with `xhost +si:localuser:fjarr`). Capture is
// `ximagesrc` cropped to each RandR monitor, input XTest, the cursor XFixes. Everything runs on the
// core loop: the X connection's descriptor is watched there and no Xlib call leaves it.
// spec: docs/23-agent-core-architecture.md#desktop-x11 · docs/adr/0006-desktop-backend-selection.md
#include <X11/XKBlib.h>
#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/extensions/XTest.h>
#include <X11/extensions/Xfixes.h>
#include <X11/extensions/Xrandr.h>
#include <glib-unix.h>
#include <gst/gst.h>
// X.h names a constant CursorShape (an XQueryBestSize class), which is also our type's name.
#undef CursorShape

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include <fjarr/desktop_backend.hpp>
#include <fjarr/video_source.hpp>

#include "desktop/cursor_image.hpp"
#include "desktop/edid.hpp"
#include "desktop/keymap.hpp"
#include "desktop/module.hpp"
#include "desktop/monitor_identity.hpp"

namespace fjarr {
namespace {

using json = nlohmann::json;

/// Xlib exits the process on a lost connection unless an exit handler returns (libX11 >= 1.7).
/// This one marks the display dead; the backend notices on the loop and reconnects.
std::set<Display*>& dead_displays() {
    static std::set<Display*> d;
    return d;
}
void on_io_exit(Display* dpy, void*) { dead_displays().insert(dpy); }
int on_error(Display*, XErrorEvent*) { return 0; } // a vanished output or window: answered by the next read

/// A rectangle of the X root, captured by `ximagesrc` (docs/23#desktop-x11).
class X11MonitorSource final : public VideoSource {
  public:
    X11MonitorSource(std::string display, std::string label) : display_(std::move(display)), label_(std::move(label)) {}
    void set_rect(int x, int y, int w, int h) { x_ = x, y_ = y, w_ = w, h_ = h; }
    SourceInfo describe() const override { return {"x11:" + label_, {{"src", TrackKind::Video, "video/x-raw"}}}; }
    GstBin* create_bin() override {
        // Inclusive end coordinates; no pointer in the picture (it travels beside it, docs/22#cursor-strategy).
        const std::string desc = "ximagesrc display-name=" + display_ + " startx=" + std::to_string(x_) + " starty=" + std::to_string(y_) +
                                 " endx=" + std::to_string(x_ + w_ - 1) + " endy=" + std::to_string(y_ + h_ - 1) +
                                 " show-pointer=false use-damage=false ! video/x-raw,framerate=30/1 ! videoconvert";
        GError* err = nullptr;
        GstElement* bin = gst_parse_bin_from_description(desc.c_str(), TRUE, &err);
        if (err) g_error_free(err);
        return bin ? GST_BIN(bin) : nullptr;
    }
    bool available() const override { return available_; }
    void on_availability_changed(std::function<void(bool)> cb) override { cb_ = std::move(cb); }
    std::string unavailable_reason() const override { return available_ ? "" : reason_; }
    void set_available(bool now, std::string reason = {}) {
        reason_ = std::move(reason);
        if (now == available_) return;
        available_ = now;
        if (cb_) cb_(now);
    }

  private:
    std::string display_, label_;
    int x_ = 0, y_ = 0, w_ = 0, h_ = 0;
    bool available_ = false;
    std::string reason_ = "not captured yet";
    std::function<void(bool)> cb_;
};

class X11Backend final : public DesktopBackend {
  public:
    X11Backend(const desktop::ModuleHost& host, const json& config)
        : host_(host), display_name_(config.value("display", std::string{})) {
        if (display_name_.empty()) display_name_ = std::getenv("DISPLAY") && *std::getenv("DISPLAY") ? std::getenv("DISPLAY") : ":0";
    }
    ~X11Backend() override {
        if (tick_) g_source_destroy(tick_), g_source_unref(tick_);
        if (pointer_tick_) g_source_destroy(pointer_tick_), g_source_unref(pointer_tick_);
        disconnect();
    }

    /// Open the display now, or keep trying every 2 s (docs/23: presence). The monitors are re-read
    /// every 500 ms besides: a RandR 1.5 monitor change may send no event (docs/07, Xvfb), and the
    /// re-read is what keeps such a change inside docs/06's 2 s hot-plug budget.
    void start() {
        connect();
        tick_ = g_timeout_source_new(500);
        g_source_set_callback(tick_, [](gpointer d) -> gboolean {
            auto* self = static_cast<X11Backend*>(d);
            if (self->dpy_) self->read_monitors();
            else if (++self->reconnect_ticks_ % 4 == 0) self->connect();
            return G_SOURCE_CONTINUE;
        }, this, nullptr);
        g_source_attach(tick_, host_.context);
    }

    // --- DesktopBackend ----------------------------------------------------------------------------
    Features features() override {
        Features f;
        f.local_cursor = true;
        return f; // no virtual monitors (mutter's RecordVirtual) and, until 3.7c, no clipboard
    }
    std::vector<Monitor> monitors() override { return monitors_; }
    void on_monitors_changed(std::function<void(std::vector<Monitor>)> cb) override {
        monitors_cb_ = std::move(cb);
        // The display opened while the backend was created, before anyone listened: say what it has,
        // on the next turn of the loop, once the capability has finished configuring.
        GSource* idle = g_idle_source_new();
        g_source_set_callback(idle, [](gpointer d) -> gboolean {
            auto* self = static_cast<X11Backend*>(d);
            if (self->monitors_cb_ && self->dpy_) self->monitors_cb_(self->monitors_);
            return G_SOURCE_REMOVE;
        }, this, nullptr);
        g_source_attach(idle, host_.context);
        g_source_unref(idle);
    }
    void on_capture_lost(std::function<void(MonitorId, CaptureLost)> cb) override { lost_cb_ = std::move(cb); }
    bool session_running() override { return dpy_ != nullptr; }
    void on_session_changed(std::function<void(bool)> cb) override { session_cb_ = std::move(cb); }

    std::shared_ptr<VideoSource> start_capture(MonitorId monitor, CaptureOptions) override {
        const Monitor* m = find(monitor);
        if (!m) return nullptr;
        auto& src = captures_[monitor];
        if (!src) src = std::make_shared<X11MonitorSource>(display_name_, m->wire_id);
        src->set_rect(m->x, m->y, m->width, m->height);
        src->set_available(dpy_ != nullptr, "the X display is not open");
        say(1, "capture of " + m->connector + " ready: " + std::to_string(m->width) + "x" + std::to_string(m->height) + "+" +
                   std::to_string(m->x) + "+" + std::to_string(m->y));
        return src;
    }
    void stop_capture(MonitorId monitor) override {
        auto it = captures_.find(monitor);
        if (it == captures_.end()) return;
        it->second->set_available(false, "capture stopped");
        captures_.erase(it);
    }
    MonitorId create_virtual_monitor(int, int) override { return INVALID_MONITOR; }
    void destroy_virtual_monitor(MonitorId) override {}
    std::shared_ptr<VideoSource> start_audio_capture() override { return nullptr; }
    void stop_audio_capture() override {}

    void on_cursor_shape(std::function<void(const CursorShape&)> cb) override {
        shape_cb_ = std::move(cb);
        if (have_shape_ && shape_cb_) shape_cb_(shape_);
    }
    void on_cursor_position(std::function<void(MonitorId, double, double)> cb) override {
        position_cb_ = std::move(cb);
        if (!pointer_tick_) {
            // XQueryPointer at 30 Hz, reported only when it moved (docs/23#desktop-x11).
            pointer_tick_ = g_timeout_source_new(33);
            g_source_set_callback(pointer_tick_, [](gpointer d) -> gboolean { static_cast<X11Backend*>(d)->poll_pointer(); return G_SOURCE_CONTINUE; }, this, nullptr);
            g_source_attach(pointer_tick_, host_.context);
        }
    }

    void pointer_motion(MonitorId monitor, double nx, double ny) override {
        const Monitor* m = find(monitor);
        if (!usable() || !m) return;
        const int x = m->x + static_cast<int>(std::lround(nx * (m->width - 1)));
        const int y = m->y + static_cast<int>(std::lround(ny * (m->height - 1)));
        XTestFakeMotionEvent(dpy_, -1, x, y, CurrentTime);
        XFlush(dpy_);
    }
    void pointer_button(MouseButton button, bool down) override {
        static constexpr unsigned buttons[] = {1, 2, 3, 8, 9}; // left middle right back forward
        const unsigned b = buttons[static_cast<int>(button)];
        if (!usable() || down == static_cast<bool>(held_buttons_.count(b))) return;
        XTestFakeButtonEvent(dpy_, b, down, CurrentTime);
        XFlush(dpy_);
        if (down) held_buttons_.insert(b);
        else held_buttons_.erase(b);
    }
    void pointer_wheel(double dx, double dy) override {
        // Buttons 4-7, one click per 48 px: three lines of the client's 16 px (docs/22 wheel).
        if (!usable()) return;
        wheel_x_ += dx;
        wheel_y_ += dy;
        auto clicks = [&](double& acc, unsigned neg, unsigned pos) {
            while (std::abs(acc) >= WHEEL_STEP) {
                const unsigned b = acc < 0 ? neg : pos;
                XTestFakeButtonEvent(dpy_, b, True, CurrentTime);
                XTestFakeButtonEvent(dpy_, b, False, CurrentTime);
                acc -= acc < 0 ? -WHEEL_STEP : WHEEL_STEP;
            }
        };
        clicks(wheel_y_, 4, 5);
        clicks(wheel_x_, 6, 7);
        XFlush(dpy_);
    }
    void key(LinuxKeycode code, bool down) override {
        // Xorg's evdev rule: X keycode = evdev code + 8.
        if (!usable() || down == static_cast<bool>(held_keys_.count(code))) return;
        XTestFakeKeyEvent(dpy_, code + 8, down, CurrentTime);
        XFlush(dpy_);
        if (down) held_keys_.insert(code);
        else held_keys_.erase(code);
    }
    std::vector<std::string> type_text(const std::string& utf8, std::string& error) override {
        if (!usable()) {
            error = "the X display is not open";
            return {};
        }
        if (!keymap_) load_keymap();
        if (!keymap_) {
            error = "the X server's keyboard layout could not be read";
            return {};
        }
        // A text is typed whole or not at all (docs/08 `text`).
        std::vector<desktop::KeyStroke> strokes;
        std::vector<std::string> untypable;
        for (const char32_t cp : desktop::decode_utf8(utf8)) {
            if (auto s = keymap_->lookup(cp)) strokes.push_back(std::move(*s));
            else if (std::find(untypable.begin(), untypable.end(), desktop::encode_utf8(cp)) == untypable.end()) untypable.push_back(desktop::encode_utf8(cp));
        }
        if (!untypable.empty()) return untypable;
        release_all_input(); // keys the session holds would change the level of what is typed
        for (const auto& s : strokes) {
            for (const auto m : s.modifiers) key(m, true);
            key(s.key, true);
            key(s.key, false);
            for (auto it = s.modifiers.rbegin(); it != s.modifiers.rend(); ++it) key(*it, false);
        }
        return {};
    }
    void release_all_input() override {
        // spec: docs/15-testing-strategy.md#safety-behaviors — nothing XTest pressed stays down.
        if (!usable()) {
            held_keys_.clear();
            held_buttons_.clear();
            return;
        }
        for (const auto k : std::vector<LinuxKeycode>(held_keys_.begin(), held_keys_.end())) XTestFakeKeyEvent(dpy_, k + 8, False, CurrentTime);
        for (const auto b : std::vector<unsigned>(held_buttons_.begin(), held_buttons_.end())) XTestFakeButtonEvent(dpy_, b, False, CurrentTime);
        held_keys_.clear();
        held_buttons_.clear();
        XFlush(dpy_);
    }
    ClipboardHandle* clipboard() override { return nullptr; } // slice 3.7c

  private:
    static constexpr double WHEEL_STEP = 48.0;

    void say(int level, const std::string& msg) {
        if (host_.log) host_.log(level, "desktop", msg.c_str());
    }
    bool usable() {
        if (dpy_ && dead_displays().count(dpy_)) lost("the X server went away");
        return dpy_ != nullptr;
    }
    const Monitor* find(MonitorId id) const {
        for (const auto& m : monitors_)
            if (m.id == id) return &m;
        return nullptr;
    }

    void connect() {
        Display* d = XOpenDisplay(display_name_.c_str());
        if (!d) {
            if (!warned_) say(2, "cannot open X display " + display_name_ + " (the kiosk session grants the agent's account: xhost +si:localuser:<agent user>); retrying every 2 s");
            warned_ = true;
            return;
        }
        XSetIOErrorExitHandler(d, &on_io_exit, nullptr);
        XSetErrorHandler(&on_error);
        int ev, err, maj, min;
        if (!XTestQueryExtension(d, &ev, &err, &maj, &min) || !XRRQueryExtension(d, &rr_event_, &err) || !XFixesQueryExtension(d, &fixes_event_, &err)) {
            say(3, "X display " + display_name_ + " lacks XTest, RandR or XFixes: no desktop");
            XCloseDisplay(d);
            return;
        }
        int rmaj = 0, rmin = 0, fmaj = 6, fmin = 0;
        XRRQueryVersion(d, &rmaj, &rmin);
        XFixesQueryVersion(d, &fmaj, &fmin); // XFixes refuses requests until the version is negotiated
        if (rmaj < 1 || (rmaj == 1 && rmin < 5)) {
            say(3, "X display " + display_name_ + " has RandR " + std::to_string(rmaj) + "." + std::to_string(rmin) + ", 1.5 is needed: no desktop");
            XCloseDisplay(d);
            return;
        }
        dpy_ = d;
        warned_ = false;
        root_ = DefaultRootWindow(dpy_);
        XRRSelectInput(dpy_, root_, RRScreenChangeNotifyMask | RROutputChangeNotifyMask | RRCrtcChangeNotifyMask);
        XFixesSelectCursorInput(dpy_, root_, XFixesDisplayCursorNotifyMask);
        XFlush(dpy_);
        watch_ = g_unix_fd_source_new(ConnectionNumber(dpy_), static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR));
        g_source_set_callback(watch_, G_SOURCE_FUNC(+[](gint, GIOCondition cond, gpointer d) -> gboolean {
            auto* self = static_cast<X11Backend*>(d);
            if (cond & (G_IO_HUP | G_IO_ERR)) {
                self->lost("the X server closed the connection");
                return G_SOURCE_REMOVE;
            }
            self->drain();
            return self->dpy_ ? G_SOURCE_CONTINUE : G_SOURCE_REMOVE;
        }), this, nullptr);
        g_source_attach(watch_, host_.context);
        say(1, "the X display " + display_name_ + " is open (RandR " + std::to_string(rmaj) + "." + std::to_string(rmin) + ")");
        keymap_.reset();
        read_monitors(true);
        read_cursor();
        for (auto& [_, src] : captures_) src->set_available(true);
        if (session_cb_) session_cb_(true);
    }

    void drain() {
        bool layout = false;
        while (dpy_ && XPending(dpy_)) {
            XEvent e;
            XNextEvent(dpy_, &e);
            if (e.type == rr_event_ + RRScreenChangeNotify || e.type == rr_event_ + RRNotify) {
                XRRUpdateConfiguration(&e);
                layout = true;
            } else if (e.type == fixes_event_ + XFixesCursorNotify) {
                read_cursor();
            } else if (e.type == MappingNotify) {
                keymap_.reset();
            }
        }
        if (dpy_ && dead_displays().count(dpy_)) return lost("the X server went away");
        if (layout) read_monitors();
    }

    /// RRGetMonitors covers real outputs and RandR 1.5 monitors (Xvfb's --setmonitor) alike.
    void read_monitors(bool force = false) {
        if (!usable()) return;
        int n = 0;
        XRRMonitorInfo* info = XRRGetMonitors(dpy_, root_, True, &n);
        std::vector<desktop::MonitorKey> keys;
        struct Geo { int x, y, w, h; bool primary; std::string label; };
        std::vector<Geo> geos;
        const Atom edid_atom = XInternAtom(dpy_, "EDID", True);
        for (int i = 0; i < n; i++) {
            char* name = XGetAtomName(dpy_, info[i].name);
            std::string label = name ? name : "";
            if (name) XFree(name);
            desktop::MonitorKey k;
            k.connector = label;
            if (info[i].noutput > 0) {
                if (XRROutputInfo* oi = XRRGetOutputInfo(dpy_, XRRGetScreenResourcesCurrent(dpy_, root_), info[i].outputs[0])) {
                    k.connector = oi->name;
                    XRRFreeOutputInfo(oi);
                }
                if (edid_atom != None) {
                    Atom type;
                    int format;
                    unsigned long items = 0, after = 0;
                    unsigned char* data = nullptr;
                    if (XRRGetOutputProperty(dpy_, info[i].outputs[0], edid_atom, 0, 128, False, False, AnyPropertyType, &type, &format, &items, &after, &data) == Success && data) {
                        if (format == 8) k = desktop::edid_key(data, items, k.connector);
                        XFree(data);
                    }
                }
            }
            keys.push_back(k);
            geos.push_back({info[i].x, info[i].y, info[i].width, info[i].height, info[i].primary != 0, label});
        }
        if (info) XRRFreeMonitors(info);
        const auto ids = desktop::wire_ids(keys);
        std::vector<Monitor> next;
        for (std::size_t i = 0; i < ids.size(); i++) {
            Monitor m;
            m.wire_id = ids[i];
            auto known = ids_by_wire_.find(m.wire_id);
            m.id = known != ids_by_wire_.end() ? known->second : (ids_by_wire_[m.wire_id] = next_monitor_++);
            m.identity = {keys[i].vendor, keys[i].model, keys[i].serial};
            m.kind = MonitorKind::Physical;
            m.connector = keys[i].connector;
            m.label = keys[i].model.empty() ? geos[i].label : keys[i].model;
            m.primary = geos[i].primary;
            m.x = geos[i].x;
            m.y = geos[i].y;
            m.width = geos[i].w;
            m.height = geos[i].h;
            m.scale = 1.0; // X11 has no per-monitor scale (ADR-0006)
            next.push_back(m);
        }
        if (next.size() == 1) next[0].primary = true; // one monitor is the primary, whatever RandR says
        auto same = [](const std::vector<Monitor>& a, const std::vector<Monitor>& b) {
            if (a.size() != b.size()) return false;
            for (std::size_t i = 0; i < a.size(); i++)
                if (a[i].wire_id != b[i].wire_id || a[i].x != b[i].x || a[i].y != b[i].y || a[i].width != b[i].width || a[i].height != b[i].height ||
                    a[i].primary != b[i].primary)
                    return false;
            return true;
        };
        if (!force && same(monitors_, next)) return;
        // A monitor that moved or resized is captured again on its new rectangle; one that left ends.
        for (auto& [id, src] : captures_) {
            const Monitor* now = nullptr;
            for (const auto& m : next)
                if (m.id == id) now = &m;
            if (!now) continue;
            const Monitor* before = find(id);
            if (before && (before->x != now->x || before->y != now->y || before->width != now->width || before->height != now->height)) {
                src->set_available(false, "the monitor changed geometry");
                src->set_rect(now->x, now->y, now->width, now->height);
                src->set_available(true);
            }
        }
        monitors_ = std::move(next);
        say(1, std::to_string(monitors_.size()) + " monitor(s) on the X display");
        if (monitors_cb_) monitors_cb_(monitors_);
    }

    void read_cursor() {
        if (!usable()) return;
        XFixesCursorImage* img = XFixesGetCursorImage(dpy_);
        if (!img) return;
        // XFixes hands premultiplied ARGB32, one pixel per `unsigned long`: to bytes, then the shared conversion.
        std::vector<std::uint8_t> bgra(static_cast<std::size_t>(img->width) * img->height * 4);
        for (int i = 0; i < img->width * img->height; i++) {
            const unsigned long p = img->pixels[i];
            bgra[i * 4 + 0] = p & 0xff;
            bgra[i * 4 + 1] = (p >> 8) & 0xff;
            bgra[i * 4 + 2] = (p >> 16) & 0xff;
            bgra[i * 4 + 3] = (p >> 24) & 0xff;
        }
        desktop::mutter::CursorImage c;
        c.width = img->width;
        c.height = img->height;
        c.hot_x = img->xhot;
        c.hot_y = img->yhot;
        c.visible = img->width > 0 && img->height > 0;
        c.rgba = desktop::mutter::to_straight_rgba(bgra.data(), img->width, img->height, img->width * 4, /*bgra=*/true);
        c.shape_id = desktop::mutter::shape_id(c);
        XFree(img);
        if (have_shape_ && c.shape_id == shape_.shape_id) return;
        shape_.shape_id = c.shape_id;
        shape_.visible = c.visible;
        shape_.width = c.width;
        shape_.height = c.height;
        shape_.hot_x = c.hot_x;
        shape_.hot_y = c.hot_y;
        shape_.rgba = std::move(c.rgba);
        have_shape_ = true;
        if (shape_cb_) shape_cb_(shape_);
    }

    void poll_pointer() {
        if (!position_cb_ || !usable()) return;
        Window r, child;
        int rx = 0, ry = 0, wx, wy;
        unsigned mask;
        if (!XQueryPointer(dpy_, root_, &r, &child, &rx, &ry, &wx, &wy, &mask)) return;
        if (rx == last_x_ && ry == last_y_) return;
        last_x_ = rx, last_y_ = ry;
        for (const auto& m : monitors_)
            if (rx >= m.x && rx < m.x + m.width && ry >= m.y && ry < m.y + m.height) {
                position_cb_(m.id, (rx - m.x) / double(std::max(1, m.width - 1)), (ry - m.y) / double(std::max(1, m.height - 1)));
                return;
            }
    }

    /// The layout from the names the X server publishes (`_XKB_RULES_NAMES`: rules, model, layout,
    /// variant, options), through the shared xkbcommon index, exactly as on GNOME (docs/23#desktop-x11).
    void load_keymap() {
        std::string layout = "us", variant;
        const Atom names = XInternAtom(dpy_, "_XKB_RULES_NAMES", True);
        Atom type;
        int format;
        unsigned long items = 0, after = 0;
        unsigned char* data = nullptr;
        if (names != None && XGetWindowProperty(dpy_, root_, names, 0, 1024, False, XA_STRING, &type, &format, &items, &after, &data) == Success && data) {
            std::vector<std::string> parts;
            for (unsigned long i = 0, start = 0; i <= items; i++)
                if (i == items || data[i] == 0) {
                    parts.emplace_back(reinterpret_cast<const char*>(data) + start, i - start);
                    start = i + 1;
                }
            XFree(data);
            if (parts.size() > 2 && !parts[2].empty()) layout = parts[2].substr(0, parts[2].find(','));
            if (parts.size() > 3) variant = parts[3].substr(0, parts[3].find(','));
        }
        std::string err;
        keymap_ = desktop::KeymapIndex::from_names(layout, variant, &err);
        if (keymap_) say(1, "typing through the X server's layout " + layout + (variant.empty() ? "" : "(" + variant + ")"));
        else say(2, "the X server's layout " + layout + " did not compile: " + err);
    }

    void lost(const std::string& why) {
        if (!dpy_) return;
        say(2, why + ": every capture ends, reconnecting every 2 s");
        for (auto& [id, src] : captures_) {
            src->set_available(false, why);
            if (lost_cb_) lost_cb_(id, CaptureLost::SessionEnded);
        }
        disconnect();
        monitors_.clear();
        held_keys_.clear();
        held_buttons_.clear();
        if (monitors_cb_) monitors_cb_(monitors_);
        if (session_cb_) session_cb_(false);
    }
    void disconnect() {
        if (watch_) g_source_destroy(watch_), g_source_unref(watch_);
        watch_ = nullptr;
        if (dpy_) {
            dead_displays().erase(dpy_);
            XCloseDisplay(dpy_);
        }
        dpy_ = nullptr;
    }

    desktop::ModuleHost host_;
    std::string display_name_;
    Display* dpy_ = nullptr;
    Window root_ = 0;
    int rr_event_ = 0, fixes_event_ = 0;
    bool warned_ = false;
    GSource* watch_ = nullptr;
    GSource* tick_ = nullptr;
    unsigned reconnect_ticks_ = 0; // the 500 ms ticks with no display: every fourth reconnects
    GSource* pointer_tick_ = nullptr;
    std::vector<Monitor> monitors_;
    std::map<std::string, MonitorId> ids_by_wire_; // a returning monitor keeps its id (docs/23#desktop-monitors)
    MonitorId next_monitor_ = 1;
    std::map<MonitorId, std::shared_ptr<X11MonitorSource>> captures_;
    std::set<LinuxKeycode> held_keys_;
    std::set<unsigned> held_buttons_;
    double wheel_x_ = 0, wheel_y_ = 0;
    std::unique_ptr<desktop::KeymapIndex> keymap_;
    CursorShape shape_;
    bool have_shape_ = false;
    int last_x_ = -1, last_y_ = -1;
    std::function<void(std::vector<Monitor>)> monitors_cb_;
    std::function<void(MonitorId, CaptureLost)> lost_cb_;
    std::function<void(bool)> session_cb_;
    std::function<void(const CursorShape&)> shape_cb_;
    std::function<void(MonitorId, double, double)> position_cb_;
};

const char* probe() { return nullptr; } // usable wherever it is installed: `create` says why the display will not open

DesktopBackend* create(const desktop::ModuleHost* host) {
    if (!host || !host->context) return nullptr; // tools without a loop (--check) get no backend
    json config = json::parse(host->config_json ? host->config_json : "{}", nullptr, false);
    if (config.is_discarded() || !config.is_object()) config = json::object();
    XInitThreads(); // ximagesrc opens its own connections on streaming threads
    auto b = std::make_unique<X11Backend>(*host, config);
    b->start();
    return b.release();
}

const desktop::ModuleV1 MODULE{
    /*display_server=*/"x11",
    /*package=*/"fjarr-desktop-x11",
    /*probe=*/&probe,
    /*create=*/&create,
};

} // namespace
} // namespace fjarr

extern "C" __attribute__((visibility("default"))) const fjarr::desktop::ModuleV1* fjarr_desktop_module_v1() { return &fjarr::MODULE; }
