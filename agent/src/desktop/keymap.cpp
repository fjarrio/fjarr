// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol ("Typing text")
#include "desktop/keymap.hpp"

#include <algorithm>
#include <map>

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

namespace fjarr::desktop {

std::vector<char32_t> decode_utf8(const std::string& s) {
    std::vector<char32_t> out;
    for (std::size_t i = 0; i < s.size();) {
        const auto b = static_cast<unsigned char>(s[i]);
        int n = b < 0x80 ? 0 : (b >> 5) == 0x6 ? 1 : (b >> 4) == 0xE ? 2 : (b >> 3) == 0x1E ? 3 : -1;
        char32_t cp = n == 0 ? b : n == 1 ? (b & 0x1F) : n == 2 ? (b & 0x0F) : (b & 0x07);
        bool ok = n >= 0;
        for (int k = 1; ok && k <= n; k++) {
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) >> 6) != 0x2) ok = false;
            else cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3F);
        }
        if (!ok) {
            out.push_back(0xFFFD);
            i++;
            continue;
        }
        out.push_back(cp);
        i += static_cast<std::size_t>(n) + 1;
    }
    return out;
}

std::string encode_utf8(char32_t cp) {
    std::string o;
    if (cp < 0x80) o += static_cast<char>(cp);
    else if (cp < 0x800) o += static_cast<char>(0xC0 | (cp >> 6)), o += static_cast<char>(0x80 | (cp & 0x3F));
    else if (cp < 0x10000)
        o += static_cast<char>(0xE0 | (cp >> 12)), o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), o += static_cast<char>(0x80 | (cp & 0x3F));
    else
        o += static_cast<char>(0xF0 | (cp >> 18)), o += static_cast<char>(0x80 | ((cp >> 12) & 0x3F)),
            o += static_cast<char>(0x80 | ((cp >> 6) & 0x3F)), o += static_cast<char>(0x80 | (cp & 0x3F));
    return o;
}

namespace {
constexpr std::uint32_t EVDEV_OFFSET = 8; // xkb keycode = evdev code + 8
} // namespace

struct KeymapIndex::Impl {
    xkb_context* ctx = nullptr;
    xkb_keymap* keymap = nullptr;
    /// Keys that set modifiers when held, with the modifier mask each one sets.
    std::vector<std::pair<xkb_keycode_t, xkb_mod_mask_t>> modifier_keys;
    xkb_mod_mask_t locks = 0; // Lock and NumLock: never used to reach a level

    ~Impl() {
        if (keymap) xkb_keymap_unref(keymap);
        if (ctx) xkb_context_unref(ctx);
    }

    void index() {
        for (const char* name : {XKB_MOD_NAME_CAPS, XKB_MOD_NAME_NUM}) {
            const xkb_mod_index_t i = xkb_keymap_mod_get_index(keymap, name);
            if (i != XKB_MOD_INVALID) locks |= 1u << i;
        }
        // What each modifier key really sets, read from a state rather than assumed (layouts move
        // AltGr between Mod5 and others).
        static const xkb_keysym_t wanted[] = {XKB_KEY_Shift_L, XKB_KEY_ISO_Level3_Shift, XKB_KEY_ISO_Level5_Shift, XKB_KEY_Control_L, XKB_KEY_Alt_L};
        // A key a real keyboard has comes first: the keymap also binds AltGr to <LVL3>, a keycode
        // no keyboard sends.
        static const std::uint32_t preferred[] = {KEY_LEFTSHIFT, KEY_RIGHTALT, KEY_LEFTCTRL, KEY_LEFTALT};
        for (const xkb_keysym_t sym : wanted) {
            std::vector<xkb_keycode_t> candidates;
            for (xkb_keycode_t kc = xkb_keymap_min_keycode(keymap); kc <= xkb_keymap_max_keycode(keymap); kc++) {
                const xkb_keysym_t* syms = nullptr;
                if (xkb_keymap_key_get_syms_by_level(keymap, kc, 0, 0, &syms) >= 1 && syms[0] == sym) candidates.push_back(kc);
            }
            std::stable_partition(candidates.begin(), candidates.end(), [](xkb_keycode_t kc) {
                return std::find(std::begin(preferred), std::end(preferred), kc - EVDEV_OFFSET) != std::end(preferred);
            });
            for (const xkb_keycode_t kc : candidates) {
                xkb_state* st = xkb_state_new(keymap);
                xkb_state_update_key(st, kc, XKB_KEY_DOWN);
                const xkb_mod_mask_t mask = xkb_state_serialize_mods(st, XKB_STATE_MODS_DEPRESSED);
                xkb_state_unref(st);
                if (mask && !(mask & locks)) {
                    modifier_keys.emplace_back(kc, mask);
                    break; // one key per modifier is enough
                }
            }
        }
    }

    bool is_modifier(xkb_keycode_t kc) const {
        return std::any_of(modifier_keys.begin(), modifier_keys.end(), [&](const auto& m) { return m.first == kc; });
    }

    /// Modifier keys whose masks add up to exactly `mask`; nullopt when they cannot.
    std::optional<std::vector<std::uint16_t>> keys_for(xkb_mod_mask_t mask) const {
        std::vector<std::uint16_t> keys;
        xkb_mod_mask_t got = 0;
        for (const auto& [kc, m] : modifier_keys) {
            if ((m & mask) != m || (got & m) == m) continue;
            got |= m;
            keys.push_back(static_cast<std::uint16_t>(kc - EVDEV_OFFSET));
        }
        if (got != mask) return std::nullopt;
        return keys;
    }
};

KeymapIndex::KeymapIndex(std::unique_ptr<Impl> impl) : impl_(std::move(impl)) {}
KeymapIndex::~KeymapIndex() = default;

std::unique_ptr<KeymapIndex> KeymapIndex::from_string(const std::string& text, std::string* error) {
    auto impl = std::make_unique<Impl>();
    impl->ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (impl->ctx) impl->keymap = xkb_keymap_new_from_buffer(impl->ctx, text.c_str(), text.size(), XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!impl->keymap) {
        if (error) *error = "the keymap does not compile";
        return nullptr;
    }
    impl->index();
    return std::unique_ptr<KeymapIndex>(new KeymapIndex(std::move(impl)));
}

std::unique_ptr<KeymapIndex> KeymapIndex::from_names(const std::string& layout, const std::string& variant, std::string* error) {
    auto impl = std::make_unique<Impl>();
    impl->ctx = xkb_context_new(XKB_CONTEXT_NO_ENVIRONMENT_NAMES);
    const xkb_rule_names names{"evdev", "pc105", layout.c_str(), variant.c_str(), ""};
    if (impl->ctx) impl->keymap = xkb_keymap_new_from_names(impl->ctx, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!impl->keymap) {
        if (error) *error = "no keymap for layout '" + layout + "'";
        return nullptr;
    }
    impl->index();
    return std::unique_ptr<KeymapIndex>(new KeymapIndex(std::move(impl)));
}

std::optional<KeyStroke> KeymapIndex::lookup(char32_t cp, std::uint32_t layout) const {
    if (cp == U'\n') cp = U'\r'; // Return's keysym maps to CR
    xkb_keymap* km = impl_->keymap;
    if (layout >= xkb_keymap_num_layouts(km)) layout = 0;
    std::optional<KeyStroke> best;
    xkb_level_index_t best_level = 0;
    for (xkb_keycode_t kc = xkb_keymap_min_keycode(km); kc <= xkb_keymap_max_keycode(km); kc++) {
        if (impl_->is_modifier(kc)) continue;
        const xkb_level_index_t levels = xkb_keymap_num_levels_for_key(km, kc, layout);
        for (xkb_level_index_t level = 0; level < levels; level++) {
            if (best && level >= best_level) break;
            const xkb_keysym_t* syms = nullptr;
            if (xkb_keymap_key_get_syms_by_level(km, kc, layout, level, &syms) != 1 || xkb_keysym_to_utf32(syms[0]) != cp) continue;
            xkb_mod_mask_t masks[16];
            const std::size_t n = xkb_keymap_key_get_mods_for_level(km, kc, layout, level, masks, 16);
            for (std::size_t i = 0; i < n; i++) {
                if (masks[i] & impl_->locks) continue;
                auto mods = impl_->keys_for(masks[i]);
                if (!mods) continue;
                best = KeyStroke{static_cast<std::uint16_t>(kc - EVDEV_OFFSET), std::move(*mods)};
                best_level = level;
                break;
            }
            if (best && best_level == level) break;
        }
        if (best && best_level == 0) break; // nothing beats a plain key
    }
    return best;
}

} // namespace fjarr::desktop
