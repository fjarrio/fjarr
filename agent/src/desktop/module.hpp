#pragma once
// The desktop backend module seam (ADR-0021): backends are built in-tree and shipped as separate
// packages, so the core carries no X11, Wayland, PipeWire or libei dependency. This is an
// INTERNAL seam between the core and modules released together with it — not a public plugin ABI,
// which docs/05 does not promise before M6.
// spec: docs/23-agent-core-architecture.md · docs/adr/0021-desktop-backends-as-runtime-modules.md
#include <fjarr/desktop_backend.hpp>

namespace fjarr::desktop {

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
    /// Create a backend. The caller owns it; null means the module changed its mind after `probe`.
    DesktopBackend* (*create)();
};

/// The one exported symbol. The version is in the NAME, not a field: a module built against an
/// older seam is then simply not found, instead of being loaded and misread.
using EntryV1 = const ModuleV1* (*)();
inline constexpr const char* ENTRY_SYMBOL_V1 = "fjarr_desktop_module_v1";

} // namespace fjarr::desktop
