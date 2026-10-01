// spec: docs/08-protocol.md#input-events-fjarrdesktop — `key {code}` is the physical key.
#include "desktop/keycodes.hpp"

#include <unordered_map>

#include <linux/input-event-codes.h>

namespace fjarr::desktop {

std::optional<std::uint16_t> evdev_for_code(std::string_view code) {
    // The writing-system and functional keys of the UI Events code table that a PC keyboard has.
    // IntlBackslash is the ISO key left of Z (< > on a Swedish keyboard).
    static const std::unordered_map<std::string_view, std::uint16_t> table{
        {"Escape", KEY_ESC}, {"Digit1", KEY_1}, {"Digit2", KEY_2}, {"Digit3", KEY_3}, {"Digit4", KEY_4},
        {"Digit5", KEY_5}, {"Digit6", KEY_6}, {"Digit7", KEY_7}, {"Digit8", KEY_8}, {"Digit9", KEY_9},
        {"Digit0", KEY_0}, {"Minus", KEY_MINUS}, {"Equal", KEY_EQUAL}, {"Backspace", KEY_BACKSPACE},
        {"Tab", KEY_TAB}, {"KeyQ", KEY_Q}, {"KeyW", KEY_W}, {"KeyE", KEY_E}, {"KeyR", KEY_R}, {"KeyT", KEY_T},
        {"KeyY", KEY_Y}, {"KeyU", KEY_U}, {"KeyI", KEY_I}, {"KeyO", KEY_O}, {"KeyP", KEY_P},
        {"BracketLeft", KEY_LEFTBRACE}, {"BracketRight", KEY_RIGHTBRACE}, {"Enter", KEY_ENTER},
        {"ControlLeft", KEY_LEFTCTRL}, {"KeyA", KEY_A}, {"KeyS", KEY_S}, {"KeyD", KEY_D}, {"KeyF", KEY_F},
        {"KeyG", KEY_G}, {"KeyH", KEY_H}, {"KeyJ", KEY_J}, {"KeyK", KEY_K}, {"KeyL", KEY_L},
        {"Semicolon", KEY_SEMICOLON}, {"Quote", KEY_APOSTROPHE}, {"Backquote", KEY_GRAVE},
        {"ShiftLeft", KEY_LEFTSHIFT}, {"Backslash", KEY_BACKSLASH}, {"KeyZ", KEY_Z}, {"KeyX", KEY_X},
        {"KeyC", KEY_C}, {"KeyV", KEY_V}, {"KeyB", KEY_B}, {"KeyN", KEY_N}, {"KeyM", KEY_M},
        {"Comma", KEY_COMMA}, {"Period", KEY_DOT}, {"Slash", KEY_SLASH}, {"ShiftRight", KEY_RIGHTSHIFT},
        {"NumpadMultiply", KEY_KPASTERISK}, {"AltLeft", KEY_LEFTALT}, {"Space", KEY_SPACE},
        {"CapsLock", KEY_CAPSLOCK}, {"F1", KEY_F1}, {"F2", KEY_F2}, {"F3", KEY_F3}, {"F4", KEY_F4},
        {"F5", KEY_F5}, {"F6", KEY_F6}, {"F7", KEY_F7}, {"F8", KEY_F8}, {"F9", KEY_F9}, {"F10", KEY_F10},
        {"NumLock", KEY_NUMLOCK}, {"ScrollLock", KEY_SCROLLLOCK}, {"Numpad7", KEY_KP7}, {"Numpad8", KEY_KP8},
        {"Numpad9", KEY_KP9}, {"NumpadSubtract", KEY_KPMINUS}, {"Numpad4", KEY_KP4}, {"Numpad5", KEY_KP5},
        {"Numpad6", KEY_KP6}, {"NumpadAdd", KEY_KPPLUS}, {"Numpad1", KEY_KP1}, {"Numpad2", KEY_KP2},
        {"Numpad3", KEY_KP3}, {"Numpad0", KEY_KP0}, {"NumpadDecimal", KEY_KPDOT},
        {"IntlBackslash", KEY_102ND}, {"F11", KEY_F11}, {"F12", KEY_F12}, {"IntlRo", KEY_RO},
        {"NumpadEnter", KEY_KPENTER}, {"ControlRight", KEY_RIGHTCTRL}, {"NumpadDivide", KEY_KPSLASH},
        {"PrintScreen", KEY_SYSRQ}, {"AltRight", KEY_RIGHTALT}, {"Home", KEY_HOME}, {"ArrowUp", KEY_UP},
        {"PageUp", KEY_PAGEUP}, {"ArrowLeft", KEY_LEFT}, {"ArrowRight", KEY_RIGHT}, {"End", KEY_END},
        {"ArrowDown", KEY_DOWN}, {"PageDown", KEY_PAGEDOWN}, {"Insert", KEY_INSERT}, {"Delete", KEY_DELETE},
        {"NumpadEqual", KEY_KPEQUAL}, {"Pause", KEY_PAUSE}, {"IntlYen", KEY_YEN},
        {"MetaLeft", KEY_LEFTMETA}, {"MetaRight", KEY_RIGHTMETA}, {"OSLeft", KEY_LEFTMETA}, {"OSRight", KEY_RIGHTMETA},
        {"ContextMenu", KEY_COMPOSE}, {"F13", KEY_F13}, {"F14", KEY_F14}, {"F15", KEY_F15}, {"F16", KEY_F16},
        {"F17", KEY_F17}, {"F18", KEY_F18}, {"F19", KEY_F19}, {"F20", KEY_F20}, {"F21", KEY_F21},
        {"F22", KEY_F22}, {"F23", KEY_F23}, {"F24", KEY_F24}, {"AudioVolumeMute", KEY_MUTE},
        {"AudioVolumeDown", KEY_VOLUMEDOWN}, {"AudioVolumeUp", KEY_VOLUMEUP}, {"NumpadComma", KEY_KPCOMMA},
        {"Lang1", KEY_HANGEUL}, {"Lang2", KEY_HANJA}, {"KanaMode", KEY_KATAKANAHIRAGANA},
        {"Convert", KEY_HENKAN}, {"NonConvert", KEY_MUHENKAN},
    };
    const auto it = table.find(code);
    if (it == table.end()) return std::nullopt;
    return it->second;
}

} // namespace fjarr::desktop
