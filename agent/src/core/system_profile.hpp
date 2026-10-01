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
    std::string feature; // "core", "net", "desktop", or "profile" for the file itself
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
        /// It may exist, but this user cannot look (EACCES): run --check as root. Never "missing".
        bool denied = false;
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
    /// systemd runs here (`/run/systemd/system`). Without it — a container — units are satisfied
    /// by what does their job there (docs/26#the-system-profile).
    bool systemd = true;
    std::function<std::optional<NetDev>(const std::string& name)> netdev;
    /// A small text file's contents (GDM's configuration, the ghost snippet, /proc/cmdline).
    std::function<std::optional<std::string>(const std::string& path)> read_file;
    /// The name a sink gives over a connector's DDC bus, bypassing any EDID the kernel forces: on a
    /// ghost connector, a real monitor (docs/18 #37). None when nothing answers or it cannot be read.
    std::function<std::optional<std::string>(const std::string& connector)> ddc_monitor;

    /// The running machine: getpwnam/getgrouplist, stat, systemd's .wants links, /sys/class/net.
    static System real();
};

/// Rows for the profile at `path`. `core` always; `net` when the tunnel is configured. A missing
/// profile file is one informational row (a development build), not a failure.
std::vector<Row> check(const std::string& path, bool net_wanted, const System& system);
/// As above, and `desktop` when `setup desktop` configured a desktop account (`helper.user`).
std::vector<Row> check(const std::string& path, bool net_wanted, const std::optional<std::string>& desktop_account, const System& system);

/// True when no row failed.
bool all_ok(const std::vector<Row>& rows);

} // namespace fjarr::profile
