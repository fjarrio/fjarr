#pragma once
// The clipboard's types: Fjarr's names on the wire and on ClipboardHandle ("text/plain" = UTF-8 text),
// and the compositor's names for the same content. Compiled into the helper and backend module E.
// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol (Clipboard) · docs/08 clipboard-*
#include <string>
#include <vector>

namespace fjarr::desktop::clipboard {

inline constexpr const char* TEXT = "text/plain";
inline constexpr std::size_t MAX_BYTES = 1024 * 1024; // docs/08: 1 MiB in either direction

/// Every name an application may ask text for. Offering fewer, `wl-paste --type text/plain` found
/// nothing (spike 2026-10-03).
const std::vector<std::string>& text_aliases();

/// What the compositor's types can be read as, in Fjarr's names: {"text/plain"} when any text type
/// is there, else empty (images come later).
std::vector<std::string> fjarr_types(const std::vector<std::string>& compositor_types);

/// The compositor type to read `fjarr_type` as, from what was offered: UTF-8 first. Empty when none fits.
std::string compositor_type_for(const std::string& fjarr_type, const std::vector<std::string>& offered);

} // namespace fjarr::desktop::clipboard
