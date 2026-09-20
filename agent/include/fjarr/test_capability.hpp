#pragma once
// fjarr.test — the built-in test capability: the install smoke test and the
// core's end-to-end proof (docs/06#fjarr.test). Enabled by config; its hooks
// are inert unless `test_hooks = true`.
// spec: docs/06-capabilities.md#fjarrtest--the-built-in-test-capability-slice-3
#include <memory>

#include <fjarr/capability.hpp>

namespace fjarr {

class TestCapability final : public Capability {
  public:
    TestCapability();
    ~TestCapability() override;

    CapabilityManifest manifest() const override;
    void configure(const nlohmann::json& validated_config) override;
    void session_attached(SessionContext& ctx, const nlohmann::json& granted_params) override;
    void session_detached(const SessionId& id, DetachReason reason, std::string_view detail) override;
    void release_all_input(const SessionId& id) override;
    void on_message(SessionContext& ctx, const Envelope& msg) override;
    void shutdown() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fjarr
