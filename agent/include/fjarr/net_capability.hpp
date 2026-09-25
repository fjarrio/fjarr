#pragma once
// fjarr.net — a session-scoped IP link between one operator machine and this robot, so ssh, scp,
// UDP bridges and ROS 2 tooling work against it without a capability per tool.
// spec: docs/27-network-tunnel.md · docs/08-protocol.md#net-packets · docs/10-security.md#network-tunnel
#include <memory>
#include <string>

#include <fjarr/capability.hpp>

namespace fjarr {

class NetCapability final : public Capability {
  public:
    explicit NetCapability(std::string robot_id);
    ~NetCapability() override;

    CapabilityManifest manifest() const override;
    /// Config: `{ enabled, interface, range, address, mtu, allow_ports }`. The interface is
    /// **attached to, never created**: the installer makes it, owned by the agent's user, before
    /// the robot's software starts, because DDS binds its interfaces at participant creation and
    /// one that appears later is invisible forever (docs/27#lifecycle). So the agent needs no
    /// CAP_NET_ADMIN, and a missing device reports `unavailable` with the command that fixes it.
    void configure(const nlohmann::json& validated_config, const SourceFactory& sources) override;
    void session_attached(SessionContext& ctx, const nlohmann::json& granted_params) override;
    void session_detached(const SessionId& id, DetachReason reason, std::string_view detail) override;
    /// Safety (docs/15): the link dies with the session. It is the robot's whole network surface,
    /// so it is released first on every detach path, like any other input-bearing capability.
    void release_all_input(const SessionId& id) override;
    void on_message(SessionContext& ctx, const Envelope& msg) override;
    /// One binary message is exactly one IP packet (docs/08#net-packets). Packets that fail
    /// either policy rule are dropped and counted here, before the kernel ever sees them.
    void on_binary(SessionContext& ctx, std::span<const std::byte> bytes) override;
    void shutdown() override;

    /// This robot's tunnel address, as configured or derived (docs/27#addressing). Empty until
    /// configure().
    std::string address() const;

    /// The same answer from config alone — no device, no session, no source factory. The
    /// installer needs the address *before* the agent runs (docs/27#lifecycle), which is what
    /// `fjarr-agent --net-address` prints. Throws FjarrError on a range or address that does not
    /// parse, so a typo is caught at setup rather than at the first connection.
    static std::string address_from_config(const std::string& robot_id, const nlohmann::json& config);

    /// Test seam (docs/15): pump packets to and from an already-open fd — a socketpair — instead
    /// of a TUN interface, so the policy rules, the MTU bound and the tail-drop can be tested
    /// without a device and without privileges. Takes ownership of the fd.
    void test_attach_fd(int fd);

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace fjarr
