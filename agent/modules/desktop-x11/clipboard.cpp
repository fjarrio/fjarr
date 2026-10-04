// spec: docs/23-agent-core-architecture.md#desktop-x11 (Clipboard) · ICCCM 2.6 (selections), 2.7.2 (INCR)
#include "clipboard.hpp"

#include <X11/Xatom.h>
#include <X11/extensions/Xfixes.h>

#include <algorithm>
#include <cstring>

#include "desktop/clipboard_types.hpp"

namespace fjarr::desktop::x11 {

namespace {
constexpr guint FETCH_TIMEOUT_MS = 5000; // an owner that stops answering, per step or INCR chunk
constexpr guint SEND_TIMEOUT_MS = 10000; // a requestor that stops deleting INCR chunks
constexpr std::size_t TARGETS_MAX = 64 * 1024;
} // namespace

X11Clipboard::X11Clipboard(GMainContext* context, std::function<void(int, const std::string&)> log)
    : context_(context), log_(std::move(log)) {}

X11Clipboard::~X11Clipboard() {
    // The capability may be gone with the backend: no callbacks from here.
    if (fetch_timer_) g_source_destroy(fetch_timer_), g_source_unref(fetch_timer_);
    for (auto& s : sends_)
        if (s.timer) g_source_destroy(s.timer), g_source_unref(s.timer);
}

Atom X11Clipboard::atom(const char* name) { return XInternAtom(dpy_, name, False); }

void X11Clipboard::attach(Display* dpy, int fixes_event_base) {
    dpy_ = dpy;
    fixes_event_ = fixes_event_base;
    clipboard_ = atom("CLIPBOARD");
    targets_ = atom("TARGETS");
    timestamp_ = atom("TIMESTAMP");
    incr_ = atom("INCR");
    multiple_ = atom("MULTIPLE");
    utf8_ = atom("UTF8_STRING");
    text_ = atom("TEXT");
    prop_ = atom("FJARR_CLIPBOARD");
    stamp_prop_ = atom("FJARR_TIMESTAMP");
    // An unmapped window of our own: the selection's owner when the agent writes, the requestor when it reads.
    win_ = XCreateSimpleWindow(dpy_, DefaultRootWindow(dpy_), -10, -10, 1, 1, 0, 0, 0);
    XSelectInput(dpy_, win_, PropertyChangeMask);
    XFixesSelectSelectionInput(dpy_, win_, clipboard_,
                               XFixesSetSelectionOwnerNotifyMask | XFixesSelectionWindowDestroyNotifyMask | XFixesSelectionClientCloseNotifyMask);
    XFlush(dpy_);
}

void X11Clipboard::detach(const std::string& why, bool notify) {
    dpy_ = nullptr; // first: a callback below that reads or writes again is refused, not queued
    win_ = 0;
    if (fetch_timer_) g_source_destroy(fetch_timer_), g_source_unref(fetch_timer_);
    fetch_timer_ = nullptr;
    for (auto& s : sends_)
        if (s.timer) g_source_destroy(s.timer), g_source_unref(s.timer);
    sends_.clear();
    owning_ = false;
    owned_.clear();
    auto pending = std::move(fetches_);
    fetches_.clear();
    for (auto& f : pending)
        if (notify && f.done) f.done(std::nullopt, why);
}

bool X11Clipboard::handle(XEvent& e) {
    if (!dpy_) return false;
    if (e.type == fixes_event_ + XFixesSelectionNotify) {
        const auto& se = reinterpret_cast<const XFixesSelectionNotifyEvent&>(e);
        if (se.selection != clipboard_ || se.owner == win_) return true; // our own write is not a robot copy
        if (se.owner == None) {
            if (changed_cb_) changed_cb_({});
            return true;
        }
        // A new owner: what it offers, in Fjarr's names, once its TARGETS are in.
        Fetch f;
        f.announce = true;
        fetches_.push_back(std::move(f));
        start_next();
        return true;
    }
    switch (e.type) {
    case SelectionNotify:
        if (e.xselection.requestor != win_) return false;
        on_selection_notify(e.xselection);
        return true;
    case SelectionRequest:
        if (e.xselectionrequest.owner != win_) return false;
        on_request(e.xselectionrequest);
        return true;
    case SelectionClear:
        if (e.xselectionclear.window != win_ || e.xselectionclear.selection != clipboard_) return false;
        owning_ = false; // another client copied; XFixes announces it
        owned_.clear();
        return true;
    case PropertyNotify:
        if (e.xproperty.window == win_) {
            if (e.xproperty.atom == prop_ && e.xproperty.state == PropertyNewValue && !fetches_.empty() && fetches_.front().incr) on_incoming_chunk();
            return true;
        }
        return e.xproperty.state == PropertyDelete && on_property_deleted(e.xproperty);
    default:
        return false;
    }
}

// --- Reading ---------------------------------------------------------------------------------------

void X11Clipboard::read(const std::string& type, std::size_t max_bytes, std::function<void(std::optional<std::string>, std::string)> done) {
    if (!dpy_) return done(std::nullopt, "the X display is not open");
    if (owning_) {
        // The agent's own content: no round trip through the server.
        const auto names = clipboard::compositor_names(type);
        const auto it = names.empty() ? owned_.end() : owned_.find(atom(names.front().c_str()));
        if (type != owned_type_ || it == owned_.end()) return done(std::nullopt, "the robot's clipboard offers no '" + type + "'");
        if (it->second->size() > max_bytes) return done(std::nullopt, "larger than " + std::to_string(max_bytes) + " bytes");
        return done(*it->second, "");
    }
    Fetch f;
    f.fjarr_type = type;
    f.max_bytes = max_bytes;
    f.done = std::move(done);
    fetches_.push_back(std::move(f));
    start_next();
}

void X11Clipboard::start_next() {
    if (!dpy_ || fetch_timer_ || fetches_.empty()) return;
    XConvertSelection(dpy_, clipboard_, targets_, prop_, win_, CurrentTime);
    XFlush(dpy_);
    arm_timer();
}

void X11Clipboard::arm_timer() {
    if (fetch_timer_) g_source_destroy(fetch_timer_), g_source_unref(fetch_timer_);
    fetch_timer_ = g_timeout_source_new(FETCH_TIMEOUT_MS);
    g_source_set_callback(fetch_timer_, [](gpointer d) -> gboolean {
        auto* self = static_cast<X11Clipboard*>(d);
        self->finish(std::nullopt, "the robot's clipboard owner did not answer within 5 s");
        return G_SOURCE_REMOVE;
    }, this, nullptr);
    g_source_attach(fetch_timer_, context_);
}

void X11Clipboard::finish(std::optional<std::string> bytes, std::string error) {
    if (fetch_timer_) g_source_destroy(fetch_timer_), g_source_unref(fetch_timer_);
    fetch_timer_ = nullptr;
    if (fetches_.empty()) return;
    Fetch f = std::move(fetches_.front());
    fetches_.pop_front();
    if (f.incr && dpy_) XDeleteProperty(dpy_, win_, prop_); // an abandoned INCR: whatever arrives next is dropped
    if (f.announce && !bytes && !error.empty() && log_) log_(2, "clipboard: " + error);
    if (f.done) f.done(std::move(bytes), std::move(error));
    start_next();
}

std::optional<std::string> X11Clipboard::take_property(Atom& type, std::size_t max_bytes, bool& too_big) {
    int format = 0;
    unsigned long items = 0, after = 0;
    unsigned char* data = nullptr;
    too_big = false;
    // Read and delete in one: deleting is what asks an INCR owner for its next chunk.
    if (XGetWindowProperty(dpy_, win_, prop_, 0, static_cast<long>(max_bytes / 4 + 1), True, AnyPropertyType, &type, &format, &items, &after, &data) != Success)
        return std::nullopt;
    std::string out;
    if (data) {
        // Xlib hands format 32 as longs and format 16 as shorts, whatever their width on the wire.
        const std::size_t unit = format == 32 ? sizeof(long) : format == 16 ? sizeof(short) : 1;
        out.assign(reinterpret_cast<const char*>(data), items * unit);
        XFree(data);
    }
    if (after > 0 || (format == 8 && out.size() > max_bytes)) {
        too_big = true;
        XDeleteProperty(dpy_, win_, prop_);
        return std::nullopt;
    }
    return out;
}

void X11Clipboard::on_selection_notify(const XSelectionEvent& e) {
    if (fetches_.empty() || e.selection != clipboard_) return;
    Fetch& f = fetches_.front();
    if (e.property == None) {
        if (!f.data_phase) {
            if (f.announce) {
                if (changed_cb_) changed_cb_({});
                return finish(std::nullopt, "");
            }
            return finish(std::nullopt, "the robot's clipboard is empty");
        }
        return finish(std::nullopt, "the robot's clipboard owner would not give '" + f.fjarr_type + "'");
    }
    Atom type = None;
    bool too_big = false;
    if (!f.data_phase) {
        auto raw = take_property(type, TARGETS_MAX, too_big);
        std::vector<std::string> names;
        if (raw)
            for (std::size_t i = 0; i + sizeof(long) <= raw->size(); i += sizeof(long)) {
                unsigned long a = 0;
                std::memcpy(&a, raw->data() + i, sizeof a);
                if (char* n = a ? XGetAtomName(dpy_, a) : nullptr) {
                    names.emplace_back(n);
                    XFree(n);
                }
            }
        if (f.announce) {
            if (changed_cb_) changed_cb_(clipboard::fjarr_types(names));
            return finish(std::nullopt, "");
        }
        const std::string target = clipboard::compositor_type_for(f.fjarr_type, names);
        if (target.empty()) return finish(std::nullopt, "the robot's clipboard offers no '" + f.fjarr_type + "'");
        f.data_phase = true;
        XConvertSelection(dpy_, clipboard_, atom(target.c_str()), prop_, win_, CurrentTime);
        XFlush(dpy_);
        return arm_timer();
    }
    auto raw = take_property(type, f.max_bytes, too_big);
    if (too_big) return finish(std::nullopt, "larger than " + std::to_string(f.max_bytes) + " bytes");
    if (!raw) return finish(std::nullopt, "the robot's clipboard could not be read");
    if (type == incr_) {
        // INCR: the property is deleted, which asks for the first chunk (ICCCM 2.7.2).
        f.incr = true;
        f.buffer.clear();
        XFlush(dpy_);
        return arm_timer();
    }
    finish(std::move(*raw), "");
}

void X11Clipboard::on_incoming_chunk() {
    Fetch& f = fetches_.front();
    Atom type = None;
    bool too_big = false;
    auto raw = take_property(type, f.max_bytes - std::min(f.max_bytes, f.buffer.size()), too_big);
    XFlush(dpy_);
    if (too_big) return finish(std::nullopt, "larger than " + std::to_string(f.max_bytes) + " bytes");
    if (!raw) return finish(std::nullopt, "the robot's clipboard could not be read");
    if (raw->empty()) {
        f.incr = false; // the zero-length chunk: done, and nothing left to delete
        return finish(std::move(f.buffer), "");
    }
    f.buffer += *raw;
    arm_timer();
}

// --- Writing and serving ---------------------------------------------------------------------------

namespace {
Bool is_stamp(Display*, XEvent* e, XPointer arg) {
    const auto* want = reinterpret_cast<const std::pair<Window, Atom>*>(arg);
    return e->type == PropertyNotify && e->xproperty.window == want->first && e->xproperty.atom == want->second;
}
} // namespace

Time X11Clipboard::server_time() {
    // ICCCM 2.1: own with a real timestamp, which a zero-length append to our own window yields.
    XChangeProperty(dpy_, win_, stamp_prop_, XA_STRING, 8, PropModeAppend, nullptr, 0);
    std::pair<Window, Atom> want{win_, stamp_prop_};
    XEvent e;
    XIfEvent(dpy_, &e, &is_stamp, reinterpret_cast<XPointer>(&want));
    return e.xproperty.time;
}

void X11Clipboard::write(const std::string& type, std::string bytes, std::function<void(bool, std::string)> done) {
    if (!dpy_) return done(false, "the X display is not open");
    const auto names = clipboard::compositor_names(type);
    const std::size_t limit = clipboard::max_bytes(type);
    if (names.empty() || limit == 0) return done(false, "the clipboard takes text/plain or image/png, not " + type);
    if (bytes.size() > limit) return done(false, "larger than " + std::to_string(limit) + " bytes");
    auto data = std::make_shared<const std::string>(std::move(bytes));
    std::map<Atom, std::shared_ptr<const std::string>> next;
    for (const auto& n : names) next[atom(n.c_str())] = data; // text under every text name (docs/23)
    const Time t = server_time();
    XSetSelectionOwner(dpy_, clipboard_, win_, t);
    if (XGetSelectionOwner(dpy_, clipboard_) != win_) return done(false, "another X client took the clipboard first");
    owned_ = std::move(next);
    owned_type_ = type;
    owned_time_ = t;
    owning_ = true;
    if (log_) log_(1, "clipboard: the agent set it: " + std::to_string(data->size()) + " bytes of " + type);
    done(true, "");
}

void X11Clipboard::on_request(const XSelectionRequestEvent& r) {
    XSelectionEvent n{};
    n.type = SelectionNotify;
    n.display = r.display;
    n.requestor = r.requestor;
    n.selection = r.selection;
    n.target = r.target;
    n.time = r.time;
    n.property = None; // refused, unless answered below
    const Atom prop = r.property == None ? r.target : r.property; // obsolete clients name no property
    const bool ours = owning_ && r.selection == clipboard_ && (r.time == CurrentTime || r.time >= owned_time_);
    if (ours && r.target == targets_) {
        std::vector<long> list{static_cast<long>(targets_), static_cast<long>(timestamp_)};
        for (const auto& [a, _] : owned_) list.push_back(static_cast<long>(a));
        XChangeProperty(dpy_, r.requestor, prop, XA_ATOM, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(list.data()), static_cast<int>(list.size()));
        n.property = prop;
    } else if (ours && r.target == timestamp_) {
        const long t = static_cast<long>(owned_time_);
        XChangeProperty(dpy_, r.requestor, prop, XA_INTEGER, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&t), 1);
        n.property = prop;
    } else if (auto it = ours ? owned_.find(r.target) : owned_.end(); it != owned_.end()) {
        const Atom type = r.target == text_ ? utf8_ : r.target;
        const auto& data = it->second;
        if (data->size() <= INCR_THRESHOLD) {
            XChangeProperty(dpy_, r.requestor, prop, type, 8, PropModeReplace, reinterpret_cast<const unsigned char*>(data->data()), static_cast<int>(data->size()));
        } else {
            // INCR: announce the size; each deletion of the property by the requestor asks for a chunk.
            XSelectInput(dpy_, r.requestor, PropertyChangeMask);
            const long size = static_cast<long>(data->size());
            XChangeProperty(dpy_, r.requestor, prop, incr_, 32, PropModeReplace, reinterpret_cast<const unsigned char*>(&size), 1);
            sends_.push_back(Send{r.requestor, prop, type, data});
            sends_.back().id = ++next_send_;
            arm_send_timer(sends_.back());
        }
        n.property = prop;
    }
    XSendEvent(dpy_, r.requestor, False, NoEventMask, reinterpret_cast<XEvent*>(&n));
    XFlush(dpy_);
}

bool X11Clipboard::on_property_deleted(const XPropertyEvent& e) {
    for (auto it = sends_.begin(); it != sends_.end(); ++it)
        if (it->requestor == e.window && it->property == e.atom) {
            send_chunk(*it);
            if (it->finished) drop_send(it);
            return true;
        }
    return false;
}

void X11Clipboard::send_chunk(Send& s) {
    // The last chunk is zero bytes long: that is how the requestor knows it has everything.
    const std::size_t n = std::min(INCR_THRESHOLD, s.data->size() - s.offset);
    XChangeProperty(dpy_, s.requestor, s.property, s.type, 8, PropModeReplace, reinterpret_cast<const unsigned char*>(s.data->data() + s.offset), static_cast<int>(n));
    XFlush(dpy_);
    s.offset += n;
    s.finished = n == 0;
    if (!s.finished) arm_send_timer(s); // each chunk taken restarts the requestor's clock
}

void X11Clipboard::arm_send_timer(Send& s) {
    if (s.timer) g_source_destroy(s.timer), g_source_unref(s.timer);
    s.timer = g_timeout_source_new(SEND_TIMEOUT_MS);
    struct Ctx { X11Clipboard* self; std::uint64_t id; };
    g_source_set_callback(s.timer, [](gpointer d) -> gboolean {
        auto* c = static_cast<Ctx*>(d);
        auto& sends = c->self->sends_;
        for (auto i = sends.begin(); i != sends.end(); ++i)
            if (i->id == c->id) {
                if (c->self->log_) c->self->log_(2, "clipboard: a paste on the robot stopped taking the content; dropped");
                g_source_unref(i->timer); // returning REMOVE destroys it; drop_send must not again
                i->timer = nullptr;
                c->self->drop_send(i);
                break;
            }
        return G_SOURCE_REMOVE;
    }, new Ctx{this, s.id}, [](gpointer d) { delete static_cast<Ctx*>(d); });
    g_source_attach(s.timer, context_);
}

void X11Clipboard::drop_send(std::list<Send>::iterator it) {
    if (it->timer) g_source_destroy(it->timer), g_source_unref(it->timer);
    const Window w = it->requestor;
    sends_.erase(it);
    const bool more = std::any_of(sends_.begin(), sends_.end(), [&](const Send& s) { return s.requestor == w; });
    if (dpy_ && !more) XSelectInput(dpy_, w, NoEventMask); // that window's events are its own again
}

} // namespace fjarr::desktop::x11
