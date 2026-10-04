#pragma once
// The X11 clipboard for backend A (M3 3.7c): the CLIPBOARD selection through a window the module
// owns. Reads convert the selection into a property, writes own it and answer SelectionRequest;
// contents above INCR_THRESHOLD travel with the INCR protocol in both directions. Everything runs
// on the core loop, fed by the backend's event drain.
// spec: docs/23-agent-core-architecture.md#desktop-x11 (Clipboard) · docs/08 clipboard-*
#include <X11/Xlib.h>
#include <glib.h>
// X.h names a constant CursorShape, which is also a type in desktop_backend.hpp.
#undef CursorShape

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <fjarr/desktop_backend.hpp>

namespace fjarr::desktop::x11 {

class X11Clipboard final : public ClipboardHandle {
  public:
    /// Contents above this go out in chunks of this size, with INCR (ICCCM 2.7.2).
    static constexpr std::size_t INCR_THRESHOLD = 256 * 1024;

    X11Clipboard(GMainContext* context, std::function<void(int level, const std::string&)> log);
    ~X11Clipboard() override;

    /// A display opened: the module's window, and the selection's owner changes from XFixes.
    void attach(Display* dpy, int fixes_event_base);
    /// The display is going: every pending read fails with `why` (or silently, when the backend
    /// itself is going and its listeners with it), nothing more is served. No X call.
    void detach(const std::string& why, bool notify = true);
    /// An event from the backend's drain; true when it was the clipboard's.
    bool handle(XEvent& e);

    void on_changed(std::function<void(std::vector<std::string> types)> callback) override { changed_cb_ = std::move(callback); }
    void read(const std::string& type, std::size_t max_bytes, std::function<void(std::optional<std::string> bytes, std::string error)> done) override;
    void write(const std::string& type, std::string bytes, std::function<void(bool ok, std::string error)> done) override;

  private:
    /// One conversion at a time: TARGETS first, then (for a read) the chosen target.
    struct Fetch {
        bool announce = false; // an owner change to report, not a read
        std::string fjarr_type;
        std::size_t max_bytes = 0;
        std::function<void(std::optional<std::string>, std::string)> done;
        bool data_phase = false;
        bool incr = false;
        std::string buffer;
    };
    /// One INCR transfer to a requestor: the next chunk goes when it deletes the property.
    struct Send {
        Window requestor;
        Atom property, type;
        std::shared_ptr<const std::string> data;
        std::size_t offset = 0;
        bool finished = false; // the zero-length end chunk is out
        GSource* timer = nullptr;
        std::uint64_t id = 0;
    };

    Atom atom(const char* name);
    void start_next();
    void finish(std::optional<std::string> bytes, std::string error);
    void arm_timer();
    void on_selection_notify(const XSelectionEvent& e);
    void on_incoming_chunk();
    void on_request(const XSelectionRequestEvent& e);
    bool on_property_deleted(const XPropertyEvent& e);
    void send_chunk(Send& s);
    void drop_send(std::list<Send>::iterator it);
    void arm_send_timer(Send& s);
    Time server_time();
    std::optional<std::string> take_property(Atom& type, std::size_t max_bytes, bool& too_big);

    GMainContext* context_;
    std::function<void(int, const std::string&)> log_;
    std::function<void(std::vector<std::string>)> changed_cb_;
    Display* dpy_ = nullptr;
    Window win_ = 0;
    int fixes_event_ = 0;
    Atom clipboard_ = 0, targets_ = 0, timestamp_ = 0, incr_ = 0, multiple_ = 0, utf8_ = 0, text_ = 0, prop_ = 0, stamp_prop_ = 0;

    std::deque<Fetch> fetches_;
    GSource* fetch_timer_ = nullptr;

    bool owning_ = false;
    Time owned_time_ = CurrentTime;
    std::string owned_type_;                                  // Fjarr's name
    std::map<Atom, std::shared_ptr<const std::string>> owned_; // every name it is offered under
    std::list<Send> sends_;
    std::uint64_t next_send_ = 0;
};

} // namespace fjarr::desktop::x11
