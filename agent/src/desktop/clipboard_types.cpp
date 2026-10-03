// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol (Clipboard)
#include "clipboard_types.hpp"

#include <algorithm>

namespace fjarr::desktop::clipboard {

const std::vector<std::string>& text_aliases() {
    // In order of preference when reading: UTF-8 by name first, then the X11 atoms Xwayland apps use.
    static const std::vector<std::string> aliases{"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT"};
    return aliases;
}

std::vector<std::string> fjarr_types(const std::vector<std::string>& compositor_types) {
    const auto& t = text_aliases();
    for (const auto& c : compositor_types)
        if (std::find(t.begin(), t.end(), c) != t.end()) return {TEXT};
    return {};
}

std::string compositor_type_for(const std::string& fjarr_type, const std::vector<std::string>& offered) {
    if (fjarr_type != TEXT) return {};
    for (const auto& a : text_aliases())
        if (std::find(offered.begin(), offered.end(), a) != offered.end()) return a;
    return {};
}

} // namespace fjarr::desktop::clipboard
