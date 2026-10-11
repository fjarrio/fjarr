// spec: docs/26-robot-install-and-drivers.md#ghost-screens
#include "desktop/ghost_primary.hpp"

#include <algorithm>

namespace fjarr::desktop {

std::optional<std::string> primary_to_make(const std::vector<Screen>& screens) {
    auto real = [](const Screen& s) { return !s.ghost && !s.is_virtual; };
    const bool ghost_primary = std::any_of(screens.begin(), screens.end(), [](const Screen& s) { return s.primary && s.ghost; });
    if (!ghost_primary) return std::nullopt;
    // Mirrored with a real one, the ghost is primary only because that real monitor is: asking again
    // changes nothing, and asking in a loop ran the mini-PC out of memory (2026-10-11).
    if (std::any_of(screens.begin(), screens.end(), [&](const Screen& s) { return real(s) && s.primary; })) return std::nullopt;
    const Screen* best = nullptr;
    for (const auto& s : screens)
        if (real(s) && (!best || s.x < best->x || (s.x == best->x && s.y < best->y))) best = &s;
    if (!best) return std::nullopt; // only ghosts: one of them is primary, as it must be
    return best->connector;
}

std::optional<std::string> sole_primary(const std::vector<Screen>& screens) {
    if (std::count_if(screens.begin(), screens.end(), [](const Screen& s) { return s.primary; }) < 2) return std::nullopt;
    const Screen* best = nullptr;
    auto better = [](const Screen& a, const Screen& b) {
        if (a.ghost != b.ghost) return !a.ghost;
        return a.x < b.x || (a.x == b.x && a.y < b.y);
    };
    for (const auto& s : screens)
        if (s.primary && (!best || better(s, *best))) best = &s;
    return best->connector;
}

} // namespace fjarr::desktop
