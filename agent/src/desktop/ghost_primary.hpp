#pragma once
// docs/26#ghost-screens: a ghost is never primary while a real monitor is connected. The decision,
// pure, so the session helper's use of it is tested without mutter.
#include <optional>
#include <string>
#include <vector>

namespace fjarr::desktop {

struct Screen {
    std::string connector;
    bool primary = false;
    bool ghost = false;      // a Fjarr ghost screen (EDID vendor FJR)
    bool is_virtual = false; // a virtual monitor: never made primary by this rule
    int x = 0, y = 0;
};

/// The real monitor to make primary, or nothing: nothing when no ghost is primary, when a real
/// monitor is primary already (a ghost mirrored with it in the primary logical monitor), or when
/// there is no real monitor. Otherwise the leftmost real one (then the topmost).
std::optional<std::string> primary_to_make(const std::vector<Screen>& screens);

/// The one monitor to report as primary when mutter reports several (it did, after a re-plugged
/// monitor came back under a new connector: two logical monitors primary, mini-PC 2026-10-11):
/// a real one before a ghost, then the leftmost. Nothing when at most one is flagged.
std::optional<std::string> sole_primary(const std::vector<Screen>& screens);

} // namespace fjarr::desktop
