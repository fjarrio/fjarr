#pragma once
// A monitor's wire id (docs/08#track-manifest): its EDID vendor, model and serial, lowercased, each
// run of other characters one '-'. The connector name when there is no serial or the identity
// collides within the set; virtual-<n> for a virtual monitor. Shared by every backend module, so
// the same monitor gets the same id whichever backend sees it.
#include <string>
#include <vector>

namespace fjarr::desktop {

struct MonitorKey {
    std::string vendor, model, serial, connector;
    bool is_virtual = false;
};

/// "del-dell-u2720q-8xk2n13"; "" when every part is empty.
std::string identity_slug(const std::string& vendor, const std::string& model, const std::string& serial);

/// The wire id of each monitor in `set`, in order, applying the fallbacks across the whole set.
std::vector<std::string> wire_ids(const std::vector<MonitorKey>& set);

} // namespace fjarr::desktop
