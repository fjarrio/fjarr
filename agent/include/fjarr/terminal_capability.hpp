#pragma once
// fjarr.terminal — a pty on the robot, the deliberately media-free second consumer of the
// extension API (docs/05: two unrelated capabilities is the minimum bar for "generic").
// spec: docs/06-capabilities.md · docs/08-protocol.md#terminal · docs/10-security.md#terminal
#include <memory>

#include <fjarr/capability.hpp>

namespace fjarr {

class TerminalCapability final : public Capability {
  public:
    TerminalCapability();
    ~TerminalCapability() override;

    CapabilityManifest manifest() const override;
    /// Config: `{ enabled, user, shell }`. **`user` is required and has no default**: the account
    /// a shell runs as is a security decision, and defaulting to the agent's own unprivileged user
    /// would be one taken by omission (docs/10#terminal). It is verified, not switched to — the
    /// agent runs unprivileged and cannot become another account, so a mismatch reports
    /// `unavailable` rather than silently running the shell as the wrong user.
    void configure(const nlohmann::json& validated_config, const SourceFactory& sources) override;
    void session_attached(SessionContext& ctx, const nlohmann::json& granted_params) override;
    void session_detached(const SessionId& id, DetachReason reason, std::string_view detail) override;
    /// Safety (docs/15): the pty dies with the session, so a dropped connection leaves no orphan
    /// shell holding the robot. Called first on every detach path.
    void release_all_input(const SessionId& id) override;
    void on_message(SessionContext& ctx, const Envelope& msg) override;
    /// Keystrokes. Bytes for a session with no open pty are dropped, which is also what keeps a
    /// read-only operator out: `open` is denied to non-owners by the lease, so they never have one.
    void on_binary(SessionContext& ctx, std::span<const std::byte> bytes) override;
    void shutdown() override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fjarr
