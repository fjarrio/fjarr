#pragma once
// fjarr.desktop — remote desktop. Always present in the core (ADR-0021), so a robot can always be
// asked "can I see your screen?" and get a useful answer: the backend itself lives in a separate
// package loaded at runtime, and with none installed this reports what to install.
// The capture, input and clipboard behaviour is M3; M2 lands the module seam and availability.
// spec: docs/06-capabilities.md · docs/adr/0021-desktop-backends-as-runtime-modules.md
#include <memory>

#include <fjarr/capability.hpp>

namespace fjarr {

class DesktopCapability final : public Capability {
  public:
    DesktopCapability();
    ~DesktopCapability() override;

    CapabilityManifest manifest() const override;
    /// Config: `{ enabled, backend = "auto" | "x11" | "wayland", module_dir }`. `auto` takes the
    /// first module that says it can run here; a named backend is never silently substituted.
    void configure(const nlohmann::json& validated_config, const SourceFactory& sources) override;
    void session_attached(SessionContext& ctx, const nlohmann::json& granted_params) override;
    void session_detached(const SessionId& id, DetachReason reason, std::string_view detail) override;
    void on_message(SessionContext& ctx, const Envelope& msg) override;
    /// One row describing the desktop: available with the backend that serves it, or unavailable
    /// with the package to install — which is what `GET /sources` and `--check` print (docs/26).
    std::vector<ConfiguredSource> configured_sources() const override;
    void shutdown() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fjarr
