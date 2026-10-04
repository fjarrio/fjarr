// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol (Clipboard)
#include "clipboard_types.hpp"

#include <algorithm>

namespace fjarr::desktop::clipboard {

const std::vector<std::string>& text_aliases() {
    // In order of preference when reading: UTF-8 by name first, then the X11 atoms Xwayland apps use.
    static const std::vector<std::string> aliases{"text/plain;charset=utf-8", "UTF8_STRING", "text/plain", "STRING", "TEXT"};
    return aliases;
}

std::size_t max_bytes(const std::string& fjarr_type) {
    if (fjarr_type == TEXT) return 1024 * 1024;
    if (fjarr_type == PNG) return 8 * 1024 * 1024; // the blob pending store's bound (docs/08#blob-frames)
    return 0;
}

std::vector<std::string> compositor_names(const std::string& fjarr_type) {
    if (fjarr_type == TEXT) return text_aliases();
    if (fjarr_type == PNG) return {"image/png"};
    return {};
}

std::vector<std::string> fjarr_types(const std::vector<std::string>& compositor_types) {
    std::vector<std::string> out;
    const auto offered = [&](const std::string& name) { return std::find(compositor_types.begin(), compositor_types.end(), name) != compositor_types.end(); };
    const auto& t = text_aliases();
    if (std::any_of(t.begin(), t.end(), offered)) out.push_back(TEXT);
    if (offered("image/png")) out.push_back(PNG);
    return out;
}

std::string compositor_type_for(const std::string& fjarr_type, const std::vector<std::string>& offered) {
    // In order of preference: UTF-8 text by name first (text_aliases' order).
    for (const auto& a : compositor_names(fjarr_type))
        if (std::find(offered.begin(), offered.end(), a) != offered.end()) return a;
    return {};
}

} // namespace fjarr::desktop::clipboard
