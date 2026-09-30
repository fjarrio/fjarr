#pragma once
// The desktop backend module seam (ADR-0021): backends are built in-tree and shipped as separate
// packages, so the core carries no X11, Wayland, PipeWire or libei dependency. This is an
// INTERNAL seam between the core and modules released together with it — not a public plugin ABI,
// which docs/05 does not promise before M6.
// spec: docs/23-agent-core-architecture.md · docs/adr/0021-desktop-backends-as-runtime-modules.md
#include <glib.h>

#include <fjarr/desktop_backend.hpp>

namespace fjarr::desktop {

/// What the core hands a module's factory (amended 2026-09-30 for backend E, M3 3.1).
struct ModuleHost {
    /// The capability's configuration for its backend, a JSON object (e.g. the helper's socket and uid).
    const char* config_json;
    /// The core loop's context. Every watch, timeout and callback of the backend runs here, which is
    /// the thread every DesktopBackend callback lands on (docs/23: all callbacks on one loop).
    GMainContext* context;
    /// Into the agent's structured log: level 0 debug, 1 info, 2 warn, 3 error.
    void (*log)(int level, const char* component, const char* message);
};

/// What a module tells the core about itself.
struct ModuleV1 {
    /// The display server it serves: "x11" | "wayland". Matches `backend` in config.
    const char* display_server;
    /// The package a robot must install to get it — the whole point of the `unavailable` message.
    const char* package;
    /// Null when this module can run here, otherwise WHY not (no DISPLAY, no portal, wrong
    /// session type). "Installed" and "usable" are different questions and a robot needs both
    /// answered: the doctor prints this verbatim.
    const char* (*probe)();
    /// Create a backend. The caller owns it; null means the module changed its mind after `probe`,
    /// or refused `host`'s configuration (it logs why through host->log).
    DesktopBackend* (*create)(const ModuleHost* host);
};

/// The one exported symbol. The version is in the NAME, not a field: a module built against an
/// older seam is then simply not found, instead of being loaded and misread. V1 was amended in
/// place twice (2026-09-27, ADR-0006's findings; 2026-09-30, ModuleHost and start_capture returning
/// a VideoSource, for backend E) because no module has shipped; from the first released module on
/// — the M3 packages — any change to ModuleV1 or DesktopBackend bumps the name.
using EntryV1 = const ModuleV1* (*)();
inline constexpr const char* ENTRY_SYMBOL_V1 = "fjarr_desktop_module_v1";

} // namespace fjarr::desktop
