// spec: docs/23-agent-core-architecture.md · docs/adr/0021-desktop-backends-as-runtime-modules.md
#include "desktop/module_loader.hpp"

#include <algorithm>
#include <filesystem>

#include <dlfcn.h>

#include "core/log.hpp"

namespace fjarr::desktop {
namespace {
/// Every package we ship a module in, so an empty directory can still name what to install
/// rather than saying "nothing found" and leaving the operator to search.
constexpr const char* KNOWN_PACKAGES[] = {"fjarr-desktop-x11 (X11/Xorg)", "fjarr-desktop-wayland (Wayland/PipeWire)"};
} // namespace

struct ModuleLoader::Impl {
    std::vector<void*> handles; // kept open for the process's life, deliberately
    std::vector<Found> found;
    std::vector<const ModuleV1*> descriptors; // parallel to `found` for usable entries
};

ModuleLoader::ModuleLoader() : impl_(std::make_unique<Impl>()) {}
ModuleLoader::~ModuleLoader() = default; // handles leak by design: see the header

void ModuleLoader::scan(const std::string& dir) {
    impl_->found.clear();
    impl_->descriptors.clear();
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) {
        log::debug("desktop", "no module directory", {{"dir", dir}});
        return;
    }
    std::vector<std::filesystem::path> files;
    for (const auto& e : std::filesystem::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && e.path().extension() == ".so") files.push_back(e.path());
    std::sort(files.begin(), files.end()); // deterministic order: `auto` must not depend on readdir

    for (const auto& path : files) {
        Found f;
        f.path = path.string();
        void* h = ::dlopen(f.path.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (!h) {
            // dlerror() CLEARS the error as it returns it, so it must be read exactly once —
            // asking twice in one expression yields null the second time (found by the test that
            // drops a stray file in the directory, which is the only path that reaches here).
            const char* err = ::dlerror();
            f.reason = err ? err : "dlopen failed";
            log::warn("desktop", "module did not load", {{"path", f.path}, {"error", f.reason}});
            impl_->found.push_back(std::move(f));
            impl_->descriptors.push_back(nullptr);
            continue;
        }
        ::dlerror();
        auto entry = reinterpret_cast<EntryV1>(::dlsym(h, ENTRY_SYMBOL_V1));
        const char* sym_err = ::dlerror();
        const ModuleV1* m = (entry && !sym_err) ? entry() : nullptr;
        if (!m) {
            // The version lives in the symbol name, so this is either not our module or one built
            // against a different seam. Both are "ignore it and say so", never "load it anyway".
            f.reason = std::string("no ") + ENTRY_SYMBOL_V1 + " (not a Fjarr desktop module, or built against another version)";
            log::warn("desktop", "module ignored", {{"path", f.path}, {"reason", f.reason}});
            ::dlclose(h);
            impl_->found.push_back(std::move(f));
            impl_->descriptors.push_back(nullptr);
            continue;
        }
        impl_->handles.push_back(h);
        f.display_server = m->display_server ? m->display_server : "";
        f.package = m->package ? m->package : "";
        const char* why = m->probe ? m->probe() : nullptr;
        f.usable = why == nullptr;
        f.reason = why ? why : "";
        log::info("desktop", "module found",
                  {{"path", f.path}, {"display_server", f.display_server}, {"usable", f.usable ? "yes" : "no"}, {"reason", f.reason}});
        impl_->found.push_back(std::move(f));
        impl_->descriptors.push_back(m);
    }
}

const std::vector<Found>& ModuleLoader::found() const { return impl_->found; }

const Found* ModuleLoader::select(const std::string& want) const {
    for (const auto& f : impl_->found) {
        if (!f.usable) continue;
        if (want == "auto" || want.empty() || want == f.display_server) return &f;
    }
    return nullptr;
}

std::unique_ptr<DesktopBackend> ModuleLoader::create(const Found& module, std::string* error) const {
    for (std::size_t i = 0; i < impl_->found.size(); i++) {
        if (&impl_->found[i] != &module) continue;
        const ModuleV1* m = impl_->descriptors[i];
        if (!m || !m->create) {
            if (error) *error = "module has no factory";
            return nullptr;
        }
        DesktopBackend* b = m->create();
        if (!b && error) *error = "the module refused to create a backend";
        return std::unique_ptr<DesktopBackend>(b);
    }
    if (error) *error = "module not from this loader";
    return nullptr;
}

std::string ModuleLoader::unavailable_reason(const std::string& want) const {
    if (impl_->found.empty()) {
        std::string s = "no desktop backend module installed; install ";
        for (std::size_t i = 0; i < std::size(KNOWN_PACKAGES); i++) s += (i ? " or " : "") + std::string(KNOWN_PACKAGES[i]);
        return s;
    }
    // Something is installed but none of it applies: say which, and why, for each.
    std::string s;
    for (const auto& f : impl_->found) {
        if (!s.empty()) s += "; ";
        const std::string who = f.display_server.empty() ? f.path : f.display_server;
        if (!f.usable) s += who + ": " + (f.reason.empty() ? "unusable here" : f.reason);
        else if (want != "auto" && !want.empty() && want != f.display_server) s += who + ": installed and usable, but backend = \"" + want + "\" was configured";
    }
    return s.empty() ? "no module matches backend = \"" + want + "\"" : s;
}

} // namespace fjarr::desktop
