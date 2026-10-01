#pragma once
// Backend module E's input: a libei sender on the EIS socket the session helper hands over
// (ADR-0028 addendum: the module owns libei). Keys are evdev codes, the pointer is absolute in the
// compositor's layout, and text is typed through the keymap mutter sends with its keyboard.
// Runs on the agent's loop (the ModuleHost context); every call returns at once.
// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol
#include <cstdint>
#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include <glib.h>

struct ei;
struct ei_device;

namespace fjarr::desktop {
class KeymapIndex;
}

namespace fjarr::desktop::mutter {

class EisInput {
  public:
    using Log = std::function<void(int level, const std::string& msg)>;
    EisInput(GMainContext* context, Log log);
    ~EisInput();
    EisInput(const EisInput&) = delete;
    EisInput& operator=(const EisInput&) = delete;

    /// Take an EIS socket (ownership passes). Replaces any previous one.
    void attach(int fd);
    /// Forget the socket: the compositor's session ended, or the helper left.
    void detach();
    bool attached() const { return ei_ != nullptr; }
    /// Called when the compositor disconnects us (the remote-desktop session ended).
    void on_disconnected(std::function<void()> cb) { disconnected_ = std::move(cb); }

    /// Absolute position in the compositor's layout (logical pixels).
    void pointer_absolute(double x, double y);
    void button(std::uint32_t evdev_button, bool down);
    void scroll(double dx, double dy);
    void key(std::uint32_t evdev_key, bool down);
    /// Every key and button this sender holds goes up. Safe with no socket.
    void release_all();

    /// Types `utf8` through the keymap. Returns the characters no key produces (as UTF-8); when
    /// any, nothing was typed. `error` is set when there is no keyboard to type with.
    std::vector<std::string> type_text(const std::string& utf8, std::string* error);

  private:
    void dispatch();
    void handle_events();
    ei_device* device(std::uint32_t capability) const;
    void frame(ei_device* d);
    void load_keymap(ei_device* keyboard);

    GMainContext* ctx_;
    Log log_;
    ei* ei_ = nullptr;
    GSource* watch_ = nullptr;
    std::vector<ei_device*> devices_; // referenced; resumed ones are emulating
    std::set<ei_device*> resumed_;
    std::uint32_t sequence_ = 1;
    std::set<std::uint32_t> held_keys_, held_buttons_;
    std::unique_ptr<KeymapIndex> keymap_;
    std::uint32_t group_ = 0; // the keyboard's active layout, from the compositor's modifier events
    std::function<void()> disconnected_;
    bool warned_no_device_ = false;
};

} // namespace fjarr::desktop::mutter
