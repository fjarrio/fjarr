// spec: docs/23-agent-core-architecture.md#desktop-x11 (fjarr-x11-session)
#include "desktop/x11_layout.hpp"

#include <algorithm>
#include <cctype>

namespace fjarr::desktop::x11 {
namespace {

/// "DisplayPort-4" < "DisplayPort-10": digit runs compare as numbers.
bool natural_less(const std::string& a, const std::string& b) {
    std::size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (std::isdigit(static_cast<unsigned char>(a[i])) && std::isdigit(static_cast<unsigned char>(b[j]))) {
            std::size_t ie = i, je = j;
            while (ie < a.size() && std::isdigit(static_cast<unsigned char>(a[ie]))) ie++;
            while (je < b.size() && std::isdigit(static_cast<unsigned char>(b[je]))) je++;
            const unsigned long na = std::stoul(a.substr(i, ie - i)), nb = std::stoul(b.substr(j, je - j));
            if (na != nb) return na < nb;
            i = ie, j = je;
        } else {
            if (a[i] != b[j]) return a[i] < b[j];
            i++, j++;
        }
    }
    return a.size() - i < b.size() - j;
}

} // namespace

Plan plan(const std::vector<Output>& outputs) {
    Plan p;
    std::vector<const Output*> on;
    for (const auto& o : outputs) {
        if (!o.connected && o.has_crtc) p.off.push_back(o.name);
        if (o.connected && o.preferred_w > 0 && o.preferred_h > 0) on.push_back(&o);
    }
    std::sort(on.begin(), on.end(), [](const Output* a, const Output* b) { return natural_less(a->name, b->name); });
    int x = 0;
    for (const Output* o : on) {
        p.on.push_back({o->name, x, 0, o->preferred_w, o->preferred_h});
        // Already where and how the plan wants it: nothing to change for this one.
        if (!o->has_crtc || o->x != x || o->y != 0 || o->width != o->preferred_w || o->height != o->preferred_h) p.changes = true;
        x += o->preferred_w;
        p.screen_h = std::max(p.screen_h, o->preferred_h);
    }
    p.screen_w = x;
    if (!p.off.empty()) p.changes = true;
    return p;
}

} // namespace fjarr::desktop::x11
