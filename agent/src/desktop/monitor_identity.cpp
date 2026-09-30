#include "desktop/monitor_identity.hpp"

#include <cctype>
#include <map>

namespace fjarr::desktop {

std::string identity_slug(const std::string& vendor, const std::string& model, const std::string& serial) {
    std::string out;
    bool dash = false;
    for (const std::string* part : {&vendor, &model, &serial}) {
        for (unsigned char c : *part) {
            if (std::isalnum(c)) {
                if (dash && !out.empty()) out += '-';
                dash = false;
                out += static_cast<char>(std::tolower(c));
            } else {
                dash = true;
            }
        }
        dash = true; // parts are separated like any other run
    }
    return out;
}

std::vector<std::string> wire_ids(const std::vector<MonitorKey>& set) {
    std::vector<std::string> ids(set.size());
    std::map<std::string, int> seen;
    for (const auto& m : set)
        if (!m.is_virtual && !m.serial.empty()) seen[identity_slug(m.vendor, m.model, m.serial)]++;
    int virtual_n = 0;
    for (std::size_t i = 0; i < set.size(); i++) {
        const auto& m = set[i];
        if (m.is_virtual) {
            ids[i] = "virtual-" + std::to_string(++virtual_n);
            continue;
        }
        const std::string slug = m.serial.empty() ? "" : identity_slug(m.vendor, m.model, m.serial);
        // No serial, or two monitors that claim the same one: the connector, for as long as it lasts.
        ids[i] = (!slug.empty() && seen[slug] == 1) ? slug : identity_slug("", m.connector, "");
    }
    return ids;
}

} // namespace fjarr::desktop
