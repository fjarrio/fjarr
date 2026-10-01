#pragma once
// Typing text through the robot's own keymap: mutter and libei have no Unicode input, so a
// character is typed as the key and modifiers that produce it on the active layout — or not at
// all. spec: docs/23-agent-core-architecture.md#desktop-helper-protocol · docs/08 `text`
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace fjarr::desktop {

/// One character as keys: hold `modifiers` (evdev codes, in order), press and release `key`.
struct KeyStroke {
    std::uint16_t key = 0;
    std::vector<std::uint16_t> modifiers;
};

/// The code points of `utf8`; invalid UTF-8 yields U+FFFD.
std::vector<char32_t> decode_utf8(const std::string& utf8);
std::string encode_utf8(char32_t cp);

class KeymapIndex {
  public:
    /// From the XKB text format a compositor hands its EIS keyboard; null when it does not compile.
    static std::unique_ptr<KeymapIndex> from_string(const std::string& keymap, std::string* error = nullptr);
    /// From RMLVO names (tests, and a fallback when no keymap was sent).
    static std::unique_ptr<KeymapIndex> from_names(const std::string& layout, const std::string& variant = "", std::string* error = nullptr);
    ~KeymapIndex();

    /// The keys that type `cp` on `layout` (the keymap's group), lowest level first; nullopt when
    /// no key produces it. '\n' types Return.
    std::optional<KeyStroke> lookup(char32_t cp, std::uint32_t layout = 0) const;

  private:
    struct Impl;
    explicit KeymapIndex(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};

} // namespace fjarr::desktop
