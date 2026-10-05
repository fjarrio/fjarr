// fjarr-x11-session: the X11 kiosk session's side of backend A (docs/23#desktop-x11, ADR-0006,
// ADR-0028). Started by the session's XDG autostart entry, as the session's user, it
//   1. grants the agent's account, as `xhost +si:localuser:fjarr` does (XAddHost, the localuser
//      server-interpreted family: X's own per-user grant, no authority file changing hands), and
//   2. lays out the outputs, and lays them out again on every RandR change: a bare X kiosk lays out
//      nothing and never reacts to a hot-plug (ADR-0006's spikes).
// It exits when the X server goes, with the session.
//   fjarr-x11-session [--account <agent user>] [--no-layout]
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/Xrandr.h>
#include <sys/select.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "desktop/edid.hpp"
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
    if (!do_layout) {
        // Keep the grant's session alive is not needed: the grant lasts as long as the X server does.
        XCloseDisplay(dpy);
        return 0;
    }
    int rr_event = 0, rr_error = 0;
    if (!XRRQueryExtension(dpy, &rr_event, &rr_error)) {
        say("no RandR: the outputs are left as they are");
        XCloseDisplay(dpy);
        return 0;
    }
    const Window root = DefaultRootWindow(dpy);
    XRRSelectInput(dpy, root, RRScreenChangeNotifyMask | RROutputChangeNotifyMask);
    layout(dpy, root);
    const int fd = ConnectionNumber(dpy);
    for (;;) {
        // On RandR events, and every 5 s besides: a change that sent no event is still laid out.
        fd_set rd;
        FD_ZERO(&rd);
        FD_SET(fd, &rd);
        timeval tv{5, 0};
        const int n = XPending(dpy) ? 1 : select(fd + 1, &rd, nullptr, nullptr, &tv);
        if (n < 0) break;
        bool changed = n == 0;
        while (XPending(dpy)) {
            XEvent e;
            XNextEvent(dpy, &e);
            if (e.type == rr_event + RRScreenChangeNotify || e.type == rr_event + RRNotify) {
                XRRUpdateConfiguration(&e);
                changed = true;
            }
        }
        if (changed) layout(dpy, root);
    }
    XCloseDisplay(dpy);
    return 0;
}
