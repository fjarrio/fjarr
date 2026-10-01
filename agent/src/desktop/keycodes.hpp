#pragma once
// KeyboardEvent.code (the physical key, W3C UI Events KeyboardEvent code values) to the Linux evdev
// code that key sends, as docs/08 requires the agent to map them. The robot's own layout then
// decides what the key types. spec: docs/08-protocol.md#input-events-fjarrdesktop
#include <cstdint>
#include <optional>
#include <string_view>

namespace fjarr::desktop {

/// The evdev code (KEY_*) for a `code`; nullopt for one this table does not know.
std::optional<std::uint16_t> evdev_for_code(std::string_view code);

} // namespace fjarr::desktop
