#pragma once
// The agent's end of the helper socket for a module whose helper serves only part of the protocol:
// module A, whose kiosk session program hands over the sound (ADR-0034). It listens, accepts one
// helper at a time whose SO_PEERCRED says the desktop account, answers `hello` with `welcome` or
// `refuse`, and hands every later message to the module. Everything runs on the core loop.
// spec: docs/23-agent-core-architecture.md#desktop-helper-protocol · #desktop-audio
#include <functional>
#include <memory>
#include <string>

#include <glib.h>
#include <nlohmann/json.hpp>

#include "desktop/helper_protocol.hpp"

namespace fjarr::desktop {

class HelperListener {
  public:
    using Log = std::function<void(int level, const std::string&)>;
    using OnMessage = std::function<void(proto::Message&)>;
    using OnPresence = std::function<void(bool welcomed)>;

    /// `uid`: the only account whose helper is accepted. `gid` >= 0 gives the socket that group.
    HelperListener(GMainContext* ctx, std::string path, int uid, int gid, Log log, OnMessage on_message, OnPresence on_presence);
    ~HelperListener();
    HelperListener(const HelperListener&) = delete;
    HelperListener& operator=(const HelperListener&) = delete;

    /// False (logged) when the socket cannot be made.
    bool listen();
    /// To the welcomed helper; dropped when there is none.
    void send(const nlohmann::json& body);

    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;
};

/// The desktop account and the socket's group from a module's `helper` config (`uid` or `user`,
/// `gid` or `group`); -1 when not configured or unknown.
int helper_uid(const nlohmann::json& config);
int helper_gid(const nlohmann::json& config);

} // namespace fjarr::desktop
