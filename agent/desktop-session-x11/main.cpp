// fjarr-x11-session: the X11 kiosk session's side of backend A (docs/23#desktop-x11, ADR-0006,
// ADR-0028, ADR-0034). Started by the session's XDG autostart entry, as the session's user, it
//   1. grants the agent's account, as `xhost +si:localuser:fjarr` does (XAddHost, the localuser
//      server-interpreted family: X's own per-user grant, no authority file changing hands),
//   2. lays out the outputs, and lays them out again on every RandR change: a bare X kiosk lays out
//      nothing and never reacts to a hot-plug (ADR-0006's spikes), and
//   3. hands the agent the desktop's sound: it connects to the agent's helper socket as a helper for
//      the sound only, and serves `speakers` and `start-audio` as the GNOME helper does, with a
//      PipeWire connection narrowed to the speakers (docs/23#desktop-audio).
// It exits when the X server goes, with the session.
//   fjarr-x11-session [--account <agent user>] [--no-layout]
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <glib-unix.h>
#include <glib.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <cstdlib>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

#include "../desktop-session/pipewire.hpp"
#include "../desktop-session/sound.hpp"
#include "desktop/edid.hpp"
#include "desktop/helper_protocol.hpp"
#include "desktop/x11_layout.hpp"

namespace {

using fjarr::desktop::x11::Output;

void say(const std::string& msg) {
    std::fprintf(stderr, "fjarr-x11-session: %s\n", msg.c_str());
    std::fflush(stderr);
}

bool grant(Display* dpy, const std::string& account) {
    XServerInterpretedAddress si;
    si.type = const_cast<char*>("localuser");
    si.typelength = 9;
    si.value = const_cast<char*>(account.c_str());
    si.valuelength = static_cast<int>(account.size());
    XHostAddress host;
    host.family = FamilyServerInterpreted;
    host.address = reinterpret_cast<char*>(&si);
    host.length = sizeof si;
    XAddHost(dpy, &host);
    XSync(dpy, False);
    return true;
}

struct Snapshot {
    std::vector<Output> outputs;
    std::map<std::string, RROutput> ids;
    std::map<std::string, RRCrtc> crtc_of;               // the CRTC each output holds now
    std::map<std::string, std::vector<RRCrtc>> usable;   // the CRTCs each output may use
    std::map<std::string, RRMode> preferred;
};

/// A ghost screen's EDID says vendor FJR (docs/26#ghost-screens), as the GNOME helper checks it.
bool is_ghost(Display* dpy, RROutput output, const std::string& name) {
    const Atom edid = XInternAtom(dpy, "EDID", True);
    if (edid == None) return false;
    Atom type;
    int format;
    unsigned long items = 0, after = 0;
    unsigned char* data = nullptr;
    bool ghost = false;
    if (XRRGetOutputProperty(dpy, output, edid, 0, 128, False, False, AnyPropertyType, &type, &format, &items, &after, &data) == Success && data) {
        if (format == 8) ghost = fjarr::desktop::edid_key(data, items, name).vendor == "FJR";
        XFree(data);
    }
    return ghost;
}

Snapshot read(Display* dpy, Window root, XRRScreenResources* res) {
    Snapshot s;
    const RROutput primary = XRRGetOutputPrimary(dpy, root);
    std::map<RRMode, std::pair<int, int>> sizes;
    for (int i = 0; i < res->nmode; i++) sizes[res->modes[i].id] = {static_cast<int>(res->modes[i].width), static_cast<int>(res->modes[i].height)};
    for (int i = 0; i < res->noutput; i++) {
        XRROutputInfo* oi = XRRGetOutputInfo(dpy, res, res->outputs[i]);
        if (!oi) continue;
        Output o;
        o.name = oi->name;
        o.connected = oi->connection == RR_Connected;
        o.has_crtc = oi->crtc != None;
        o.primary = res->outputs[i] == primary;
        o.ghost = o.connected && is_ghost(dpy, res->outputs[i], o.name);
        if (oi->crtc != None)
            if (XRRCrtcInfo* ci = XRRGetCrtcInfo(dpy, res, oi->crtc)) {
                o.x = ci->x, o.y = ci->y, o.width = static_cast<int>(ci->width), o.height = static_cast<int>(ci->height);
                XRRFreeCrtcInfo(ci);
            }
        if (oi->nmode > 0) {
            const RRMode m = oi->modes[oi->npreferred > 0 ? 0 : 0];
            s.preferred[o.name] = m;
            o.preferred_w = sizes[m].first, o.preferred_h = sizes[m].second;
        }
        s.ids[o.name] = res->outputs[i];
        s.crtc_of[o.name] = oi->crtc;
        s.usable[o.name].assign(oi->crtcs, oi->crtcs + oi->ncrtc);
        s.outputs.push_back(o);
        XRRFreeOutputInfo(oi);
    }
    return s;
}

/// One layout pass: plan, and apply what changed. The server is grabbed so no client sees a half state.
void layout(Display* dpy, Window root) {
    XRRScreenResources* res = XRRGetScreenResourcesCurrent(dpy, root);
    if (!res) return;
    const Snapshot s = read(dpy, root, res);
    const auto plan = fjarr::desktop::x11::plan(s.outputs);
    if (!plan.primary.empty()) {
        // docs/26#ghost-screens: never a ghost while a real monitor is connected. No CRTC moves for it.
        XRRSetOutputPrimary(dpy, root, s.ids.at(plan.primary));
        XSync(dpy, False);
        say("primary: " + plan.primary);
    }
    if (!plan.changes) {
        XRRFreeScreenResources(res);
        return;
    }
    XGrabServer(dpy);
    // Switched off first: disconnected outputs, and any CRTC the new root would not contain.
    std::set<RRCrtc> taken;
    auto off = [&](RRCrtc c) { XRRSetCrtcConfig(dpy, res, c, CurrentTime, 0, 0, None, RR_Rotate_0, nullptr, 0); };
    for (const auto& name : plan.off) off(s.crtc_of.at(name));
    for (const auto& p : plan.on)
        if (s.crtc_of.at(p.name) != None) off(s.crtc_of.at(p.name));
    if (plan.screen_w > 0 && plan.screen_h > 0) {
        // Millimetres at 96 dpi: RandR wants a physical size for the root.
        const int mm_w = static_cast<int>(std::lround(plan.screen_w * 25.4 / 96)), mm_h = static_cast<int>(std::lround(plan.screen_h * 25.4 / 96));
        XRRSetScreenSize(dpy, root, plan.screen_w, plan.screen_h, mm_w, mm_h);
    }
    for (const auto& p : plan.on) {
        RRCrtc crtc = s.crtc_of.at(p.name);
        if (crtc == None || taken.count(crtc))
            for (const RRCrtc c : s.usable.at(p.name))
                if (!taken.count(c)) {
                    crtc = c;
                    break;
                }
        if (crtc == None) {
            say("no free CRTC for " + p.name + ": left off");
            continue;
        }
        taken.insert(crtc);
        RROutput out = s.ids.at(p.name);
        XRRSetCrtcConfig(dpy, res, crtc, CurrentTime, p.x, p.y, s.preferred.at(p.name), RR_Rotate_0, &out, 1);
    }
    XUngrabServer(dpy);
    XSync(dpy, False);
    XRRFreeScreenResources(res);
    std::string summary;
    for (const auto& p : plan.on) summary += " " + p.name + " " + std::to_string(p.width) + "x" + std::to_string(p.height) + "+" + std::to_string(p.x);
    for (const auto& n : plan.off) summary += " " + n + " off";
    say("layout:" + (summary.empty() ? std::string(" no monitor") : summary));
}

/// The agent's helper socket, for the sound only (ADR-0034): connects out and reconnects, as the GNOME
/// helper does, and serves the sound part of fjarr-desktop-1 with the same speaker watch and
/// narrowing. spec: docs/23-agent-core-architecture.md#desktop-audio
class SoundLink {
  public:
    explicit SoundLink(std::string path) : path_(std::move(path)) {
        sound_ = std::make_unique<fjarr::desktop::helper::SoundWatch>([this] { send_speakers(); });
        try_connect();
    }

  private:
    void try_connect() {
        const int fd = ::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path_.c_str());
        if (fd < 0 || ::connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
            if (fd >= 0) ::close(fd);
            if (!waiting_logged_) say("waiting for the agent at " + path_ + " (the desktop's sound)");
            waiting_logged_ = true;
            g_timeout_add_seconds(backoff_s_, [](gpointer d) -> gboolean {
                static_cast<SoundLink*>(d)->try_connect();
                return G_SOURCE_REMOVE;
            }, this);
            backoff_s_ = std::min(backoff_s_ * 2, 10u);
            return;
        }
        waiting_logged_ = false;
        backoff_s_ = 1;
        sock_ = fd;
        watch_ = g_unix_fd_add(sock_, static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR), &SoundLink::on_readable, this);
        const char* desktop = std::getenv("XDG_CURRENT_DESKTOP");
        send({{"type", "hello"}, {"protocol", fjarr::desktop::proto::PROTOCOL}, {"helper", "fjarr-x11-session"},
              {"session", {{"type", "x11"}, {"desktop", desktop && *desktop ? desktop : "X11"}, {"compositor", "x11"}}}});
    }

    void disconnect(const std::string& why) {
        say("agent connection closed: " + why);
        if (watch_) g_source_remove(watch_);
        watch_ = 0;
        if (sock_ >= 0) ::close(sock_);
        sock_ = -1;
        welcomed_ = false;
        sent_.reset();
        try_connect();
    }

    void send(const nlohmann::json& body, const std::vector<int>& fds = {}) {
        std::string err;
        if (sock_ >= 0 && !fjarr::desktop::proto::send(sock_, body, fds, &err)) say("send " + body.value("type", "?") + ": " + err);
    }

    static gboolean on_readable(gint, GIOCondition, gpointer d) {
        auto* self = static_cast<SoundLink*>(d);
        fjarr::desktop::proto::Message m;
        std::string err;
        switch (fjarr::desktop::proto::receive(self->sock_, m, &err)) {
        case fjarr::desktop::proto::Received::Message: self->handle(m); return G_SOURCE_CONTINUE;
        case fjarr::desktop::proto::Received::WouldBlock:
        case fjarr::desktop::proto::Received::Invalid: return G_SOURCE_CONTINUE;
        case fjarr::desktop::proto::Received::Closed: self->watch_ = 0; self->disconnect("the agent hung up"); return G_SOURCE_REMOVE;
        case fjarr::desktop::proto::Received::Error: self->watch_ = 0; self->disconnect(err); return G_SOURCE_REMOVE;
        }
        return G_SOURCE_CONTINUE;
    }

    void handle(const fjarr::desktop::proto::Message& m) {
        const std::string type = m.type();
        if (type == "welcome") {
            welcomed_ = true;
            say("welcomed by the agent: the desktop's sound is handed over from here");
            send_speakers();
        } else if (type == "refuse") {
            say("refused by the agent: " + m.body.value("reason", std::string{"no reason given"}));
        } else if (welcomed_ && type == "start-audio") {
            start_audio(m.body.value("id", 0));
        } // the rest of fjarr-desktop-1 is the GNOME helper's: backend A reads the screen from X itself
    }

    void send_speakers() {
        if (!welcomed_) return;
        const auto sink = sound_->default_sink();
        const std::string now = sink ? sink->name : std::string{};
        if (sent_ && *sent_ == now) return;
        sent_ = now;
        if (sink) {
            send({{"type", "speakers"}, {"sink", {{"name", sink->name}, {"description", sink->description}}}});
            say("speakers: " + sink->name);
        } else {
            send({{"type", "speakers"}, {"sink", nullptr}});
            say("speakers: none");
        }
    }

    void start_audio(int id) {
        const auto sink = sound_->default_sink();
        if (!sink) {
            send({{"type", "audio-failed"}, {"id", id}, {"reason", "the desktop has no output"}});
            return;
        }
        std::string err;
        const int pw = fjarr::desktop::helper::open_narrowed_pipewire(sink->node, &err); // the speakers and nothing else
        if (pw < 0) {
            send({{"type", "audio-failed"}, {"id", id}, {"reason", err}});
            say("sound " + std::to_string(id) + " failed: " + err);
            return;
        }
        send({{"type", "audio-started"}, {"id", id}, {"node", sink->node}, {"name", sink->name}}, {pw});
        ::close(pw);
        say("sound " + std::to_string(id) + " handed over: " + sink->name);
    }

    std::string path_;
    std::unique_ptr<fjarr::desktop::helper::SoundWatch> sound_;
    int sock_ = -1;
    guint watch_ = 0;
    unsigned backoff_s_ = 1;
    bool welcomed_ = false;
    bool waiting_logged_ = false;
    std::optional<std::string> sent_; // the sink name last sent ("" = none); unset = not yet told
};

/// The X connection on the GLib loop: RandR events, and every 5 s besides (a change that sent no event
/// is still laid out). Xlib may already hold events it read with a reply, so the queue is drained until
/// empty, not just when the descriptor is readable.
struct Layout {
    Display* dpy;
    Window root;
    int rr_event;
    void drain(bool changed) {
        do {
            while (XPending(dpy)) {
                XEvent e;
                XNextEvent(dpy, &e);
                if (e.type == rr_event + RRScreenChangeNotify || e.type == rr_event + RRNotify) {
                    XRRUpdateConfiguration(&e);
                    changed = true;
                }
            }
            if (changed) layout(dpy, root);
            changed = false;
        } while (XPending(dpy));
    }
};

} // namespace

int main(int argc, char** argv) {
    std::string account = "fjarr";
    bool do_layout = true;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--account" && i + 1 < argc) account = argv[++i];
        else if (a == "--no-layout") do_layout = false;
        else {
            std::fprintf(stderr, "usage: fjarr-x11-session [--account <agent user>] [--no-layout]\n");
            return 2;
        }
    }
    Display* dpy = XOpenDisplay(nullptr);
    if (!dpy) {
        say("cannot open the display (is this the kiosk session?)");
        return 1;
    }
    grant(dpy, account);
    say("granted the agent's account: localuser:" + account);
    GMainLoop* loop = g_main_loop_new(nullptr, FALSE);
    const char* sock = std::getenv("FJARR_DESKTOP_SOCK");
    SoundLink sound(sock && *sock ? sock : "/run/fjarr/desktop.sock");
    int rr_event = 0, rr_error = 0;
    std::optional<Layout> lay;
    if (!do_layout) {
        say("--no-layout: the outputs are left as they are");
    } else if (!XRRQueryExtension(dpy, &rr_event, &rr_error)) {
        say("no RandR: the outputs are left as they are");
    } else {
        const Window root = DefaultRootWindow(dpy);
        XRRSelectInput(dpy, root, RRScreenChangeNotifyMask | RROutputChangeNotifyMask);
        layout(dpy, root);
        lay = Layout{dpy, root, rr_event};
        g_timeout_add_seconds(5, [](gpointer d) -> gboolean {
            static_cast<Layout*>(d)->drain(true);
            return G_SOURCE_CONTINUE;
        }, &*lay);
    }
    // The X server going ends the session, and this program with it.
    struct XWatch {
        Display* dpy;
        std::optional<Layout>* lay;
        GMainLoop* loop;
    } xw{dpy, &lay, loop};
    g_unix_fd_add(ConnectionNumber(dpy), static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR), [](gint, GIOCondition cond, gpointer d) -> gboolean {
        auto* w = static_cast<XWatch*>(d);
        if (cond & (G_IO_HUP | G_IO_ERR)) {
            g_main_loop_quit(w->loop);
            return G_SOURCE_REMOVE;
        }
        if (*w->lay) (*w->lay)->drain(false);
        else
            while (XPending(w->dpy)) {
                XEvent e;
                XNextEvent(w->dpy, &e);
            }
        return G_SOURCE_CONTINUE;
    }, &xw);
    g_main_loop_run(loop);
    g_main_loop_unref(loop);
    XCloseDisplay(dpy);
    return 0;
}
