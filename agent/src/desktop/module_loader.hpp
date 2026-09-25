#pragma once
// Discovery and dlopen of desktop backend modules (ADR-0021).
// spec: docs/23-agent-core-architecture.md
#include <memory>
#include <string>
#include <vector>

#include "desktop/module.hpp"

namespace fjarr::desktop {

/// One module found on disk, whether or not it can be used here.
struct Found {
    std::string path;
    std::string display_server; // empty when the file could not be read as a module
    std::string package;
    bool usable = false;
    std::string reason; // why not usable, or why it could not be loaded at all
};

/// Loads modules from a directory and hands out backends. Handles stay open for the process's
/// life: a backend outlives the call that made it, and unloading its code under it is a crash
/// nobody would diagnose twice.
class ModuleLoader {
  public:
    ModuleLoader();
    ~ModuleLoader();
    ModuleLoader(const ModuleLoader&) = delete;
    ModuleLoader& operator=(const ModuleLoader&) = delete;

    /// Scan `dir` for `*.so`. Missing or empty directory is not an error — it is the normal
    /// state of a robot that streams a camera and has no desktop.
    void scan(const std::string& dir);
    const std::vector<Found>& found() const;

    /// The module for `want` ("auto" picks the first usable one), or null.
    const Found* select(const std::string& want) const;
    /// Create a backend from a selected module; null (with `error`) when it refuses.
    std::unique_ptr<DesktopBackend> create(const Found& module, std::string* error) const;

    /// The line a robot should be told when nothing usable is installed: which package to install,
    /// or why the installed one cannot run here. Never a bare "unavailable".
    std::string unavailable_reason(const std::string& want) const;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fjarr::desktop
