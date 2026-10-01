// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol (Input, Typing text)
#include "eis_input.hpp"

#include <algorithm>
#include <cstring>

#include <sys/mman.h>
#include <unistd.h>

#include <glib-unix.h>
#include <libei.h>
#include <linux/input-event-codes.h>

#include "desktop/keymap.hpp"

namespace fjarr::desktop::mutter {

EisInput::EisInput(GMainContext* context, Log log) : ctx_(context), log_(std::move(log)) {}

EisInput::~EisInput() { detach(); }

void EisInput::attach(int fd) {
    detach();
    ei_ = ei_new_sender(nullptr);
    ei_configure_name(ei_, "fjarr-agent");
    if (ei_setup_backend_fd(ei_, fd) != 0) { // takes the descriptor either way
        log_(3, "the EIS socket from the desktop session is unusable");
        ei_ = ei_unref(ei_);
        return;
    }
    watch_ = g_unix_fd_source_new(ei_get_fd(ei_), static_cast<GIOCondition>(G_IO_IN | G_IO_HUP | G_IO_ERR));
    GUnixFDSourceFunc on_io = [](gint, GIOCondition, gpointer d) -> gboolean {
        static_cast<EisInput*>(d)->dispatch();
        return G_SOURCE_CONTINUE;
    };
    g_source_set_callback(watch_, G_SOURCE_FUNC(on_io), this, nullptr); // GLib's own cast for fd sources
    g_source_attach(watch_, ctx_);
    dispatch(); // the handshake may already be readable
}

void EisInput::detach() {
    if (watch_) {
        g_source_destroy(watch_);
        g_source_unref(watch_);
        watch_ = nullptr;
    }
    for (ei_device* d : devices_) ei_device_unref(d);
    devices_.clear();
    resumed_.clear();
    held_keys_.clear(); // the compositor releases a dead sender's keys itself
    held_buttons_.clear();
    keymap_.reset();
    if (ei_) ei_ = ei_unref(ei_);
}

void EisInput::dispatch() {
    if (!ei_) return;
    ei_dispatch(ei_);
    handle_events();
}

void EisInput::handle_events() {
    bool disconnected = false;
    while (ei_event* ev = ei_get_event(ei_)) {
        switch (ei_event_get_type(ev)) {
        case EI_EVENT_SEAT_ADDED:
            ei_seat_bind_capabilities(ei_event_get_seat(ev), EI_DEVICE_CAP_POINTER_ABSOLUTE, EI_DEVICE_CAP_KEYBOARD, EI_DEVICE_CAP_BUTTON,
                                      EI_DEVICE_CAP_SCROLL, nullptr);
            break;
        case EI_EVENT_DEVICE_ADDED: {
            ei_device* d = ei_device_ref(ei_event_get_device(ev));
            devices_.push_back(d);
            if (ei_device_has_capability(d, EI_DEVICE_CAP_KEYBOARD)) load_keymap(d);
            break;
        }
        case EI_EVENT_DEVICE_RESUMED: {
            ei_device* d = ei_event_get_device(ev);
            ei_device_start_emulating(d, sequence_++);
            resumed_.insert(d);
            if (resumed_.size() == 1) log_(1, "desktop input ready (EIS)");
            break;
        }
        case EI_EVENT_DEVICE_PAUSED: resumed_.erase(ei_event_get_device(ev)); break;
        case EI_EVENT_DEVICE_REMOVED: {
            ei_device* d = ei_event_get_device(ev);
            resumed_.erase(d);
            for (auto it = devices_.begin(); it != devices_.end(); ++it)
                if (*it == d) {
                    ei_device_unref(d);
                    devices_.erase(it);
                    break;
                }
            break;
        }
        case EI_EVENT_KEYBOARD_MODIFIERS: group_ = ei_event_keyboard_get_xkb_group(ev); break;
        case EI_EVENT_DISCONNECT: disconnected = true; break;
        default: break;
        }
        ei_event_unref(ev);
    }
    if (disconnected) {
        log_(1, "the compositor closed desktop input");
        detach();
        if (disconnected_) disconnected_();
    }
}

void EisInput::load_keymap(ei_device* keyboard) {
    ei_keymap* km = ei_device_keyboard_get_keymap(keyboard);
    if (!km || ei_keymap_get_type(km) != EI_KEYMAP_TYPE_XKB) {
        log_(2, "the compositor sent no XKB keymap: text cannot be typed");
        return;
    }
    const std::size_t size = ei_keymap_get_size(km);
    void* map = ::mmap(nullptr, size, PROT_READ, MAP_PRIVATE, ei_keymap_get_fd(km), 0);
    if (map == MAP_FAILED) {
        log_(2, std::string("cannot read the compositor's keymap: ") + std::strerror(errno));
        return;
    }
    const auto* text = static_cast<const char*>(map);
    std::string err;
    keymap_ = KeymapIndex::from_string(std::string(text, ::strnlen(text, size)), &err);
    ::munmap(map, size);
    if (!keymap_) log_(2, "the compositor's keymap: " + err);
}

ei_device* EisInput::device(std::uint32_t capability) const {
    for (ei_device* d : devices_)
        if (resumed_.count(d) && ei_device_has_capability(d, static_cast<ei_device_capability>(capability))) return d;
    return nullptr;
}

void EisInput::frame(ei_device* d) {
    ei_device_frame(d, ei_now(ei_));
    ei_dispatch(ei_); // flush now: input latency is the point
}

void EisInput::pointer_absolute(double x, double y) {
    ei_device* d = ei_ ? device(EI_DEVICE_CAP_POINTER_ABSOLUTE) : nullptr;
    if (!d) return;
    ei_device_pointer_motion_absolute(d, x, y);
    frame(d);
}

void EisInput::button(std::uint32_t code, bool down) {
    ei_device* d = ei_ ? device(EI_DEVICE_CAP_BUTTON) : nullptr;
    if (!d) return;
    if (down == static_cast<bool>(held_buttons_.count(code))) return; // no double press, no stray release
    ei_device_button_button(d, code, down);
    frame(d);
    if (down) held_buttons_.insert(code);
    else held_buttons_.erase(code);
}

void EisInput::scroll(double dx, double dy) {
    ei_device* d = ei_ ? device(EI_DEVICE_CAP_SCROLL) : nullptr;
    if (!d) return;
    ei_device_scroll_delta(d, dx, dy);
    frame(d);
}

void EisInput::key(std::uint32_t code, bool down) {
    ei_device* d = ei_ ? device(EI_DEVICE_CAP_KEYBOARD) : nullptr;
    if (!d) {
        if (!warned_no_device_) log_(2, "a key arrived before desktop input was ready; dropped");
        warned_no_device_ = true;
        return;
    }
    if (down == static_cast<bool>(held_keys_.count(code))) return;
    ei_device_keyboard_key(d, code, down);
    frame(d);
    if (down) held_keys_.insert(code);
    else held_keys_.erase(code);
}

void EisInput::release_all() {
    for (const std::uint32_t k : std::set<std::uint32_t>(held_keys_)) key(k, false);
    for (const std::uint32_t b : std::set<std::uint32_t>(held_buttons_)) button(b, false);
    held_keys_.clear();
    held_buttons_.clear();
}

std::vector<std::string> EisInput::type_text(const std::string& utf8, std::string* error) {
    ei_device* kbd = ei_ ? device(EI_DEVICE_CAP_KEYBOARD) : nullptr;
    if (!kbd || !keymap_) {
        if (error) *error = kbd ? "the compositor sent no usable keymap" : "desktop input is not ready";
        return {};
    }
    // Every character first: a text is typed whole or not at all (docs/08 `text`).
    std::vector<KeyStroke> strokes;
    std::vector<std::string> untypable;
    for (const char32_t cp : decode_utf8(utf8)) {
        auto s = keymap_->lookup(cp, group_);
        if (s) strokes.push_back(std::move(*s));
        else if (std::find(untypable.begin(), untypable.end(), encode_utf8(cp)) == untypable.end()) untypable.push_back(encode_utf8(cp));
    }
    if (!untypable.empty()) return untypable;
    release_all(); // keys the session holds would change the level of what is typed
    for (const auto& s : strokes) {
        for (const auto m : s.modifiers) key(m, true);
        key(s.key, true);
        key(s.key, false);
        for (auto it = s.modifiers.rbegin(); it != s.modifiers.rend(); ++it) key(*it, false);
    }
    return {};
}

} // namespace fjarr::desktop::mutter
