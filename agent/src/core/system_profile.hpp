#pragma once
// The system profile check (docs/26#the-system-profile): what each feature needs from the robot's OS,
// verified by `fjarr-agent --check` however the robot was installed (a .deb, an image, later a NixOS
// module or a Yocto layer). The machine's facts are behind `System` so the tests can fake one.
// spec: docs/26-robot-install-and-drivers.md#the-system-profile · ADR-0031
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fjarr::profile {

struct Row {
    std::string feature; // "core", "net", or "profile" for the file itself
    std::string item;    // what was checked: "account fjarr", "/var/lib/fjarr", "unit fjarr-agent.service"
    bool ok = false;
    std::string detail;  // what was found
    std::string fix;     // the command that fixes it; empty when ok
};

struct System {
    struct Stat {
        unsigned uid = 0;
        unsigned mode = 0; // permission bits only
        bool is_dir = false;
    };
    struct NetDev {
        bool tun = false;
        std::optional<unsigned> owner; // none: the device has no owner
        int mtu = 0;
    };
    std::function<std::optional<unsigned>(const std::string& user)> uid_of;
    std::function<std::vector<std::string>(const std::string& user)> groups_of;
    std::function<std::optional<Stat>(const std::string& path)> stat;
    std::function<bool(const std::string& unit)> unit_enabled;
    std::function<std::optional<NetDev>(const std::string& name)> netdev;

    /// The running machine: getpwnam/getgrouplist, stat, systemd's .wants links, /sys/class/net.
    static System real();
};

/// Rows for the profile at `path`. `core` always; `net` when the tunnel is configured. A missing
/// profile file is one informational row (a development build), not a failure.
std::vector<Row> check(const std::string& path, bool net_wanted, const System& system);

/// True when no row failed.
bool all_ok(const std::vector<Row>& rows);

} // namespace fjarr::profile
