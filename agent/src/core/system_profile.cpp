#include "system_profile.hpp"

#include <algorithm>
#include <climits>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <toml++/toml.hpp>

namespace fjarr::profile {

namespace {

std::string octal(unsigned mode) {
    char buf[8];
    std::snprintf(buf, sizeof buf, "%04o", mode & 07777);
    return buf;
}

std::string read_line(const std::filesystem::path& p) {
    std::ifstream in(p);
    std::string s;
    std::getline(in, s);
    return s;
}

std::vector<std::string> strings(const toml::node_view<const toml::node>& v) {
    std::vector<std::string> out;
    if (auto arr = v.as_array())
        for (const auto& e : *arr)
            if (auto s = e.value<std::string>()) out.push_back(*s);
    return out;
}

void check_core(const toml::table& core, const System& sys, std::vector<Row>& rows) {
    const std::string account = core["account"].value_or(std::string("fjarr"));
    const auto uid = sys.uid_of(account);
    rows.push_back({"core", "account " + account, uid.has_value(), uid ? "uid " + std::to_string(*uid) : "no such user",
                    uid ? "" : "sudo systemd-sysusers  # the package's sysusers entry creates it"});
    if (uid) {
        const auto have = sys.groups_of(account);
        for (const auto& g : strings(core["groups"])) {
            const bool in = std::find(have.begin(), have.end(), g) != have.end();
            rows.push_back({"core", account + " in " + g, in, in ? "member" : "not a member", in ? "" : "sudo usermod -aG " + g + " " + account});
        }
    }
    if (auto cfg = core["config"].value<std::string>()) {
        const bool there = sys.stat(*cfg).has_value();
        rows.push_back({"core", *cfg, there, there ? "present" : "missing: the service waits for it", there ? "" : "sudo fjarr-agent setup"});
    }
    for (const auto& unit : strings(core["units"])) {
        if (!sys.systemd) {
            rows.push_back({"core", "unit " + unit, true, "no systemd (a container): the container runtime starts the agent", ""});
            continue;
        }
        const bool on = sys.unit_enabled(unit);
        rows.push_back({"core", "unit " + unit, on, on ? "enabled" : "not enabled", on ? "" : "sudo systemctl enable --now " + unit});
    }
    if (auto dirs = core["dirs"].as_array()) {
        for (const auto& d : *dirs) {
            const auto* t = d.as_table();
            if (!t) continue;
            const std::string path = (*t)["path"].value_or(std::string());
            const std::string owner = (*t)["owner"].value_or(std::string());
            const auto want_mode = (*t)["mode"].value<std::string>();
            const auto st = sys.stat(path);
            const auto owner_uid = owner.empty() ? std::nullopt : sys.uid_of(owner);
            std::string detail, fix;
            bool ok = st && st->is_dir;
            if (!ok) {
                detail = "missing";
                fix = path == "/var/lib/fjarr" ? "created when the service first starts (sudo fjarr-agent setup starts it)"
                                               : "sudo systemd-tmpfiles --create fjarr-agent.conf";
            } else {
                detail = "owner uid " + std::to_string(st->uid) + ", mode " + octal(st->mode);
                if (owner_uid && st->uid != *owner_uid) {
                    ok = false;
                    fix = "sudo chown " + owner + ": " + path;
                } else if (want_mode && octal(st->mode) != *want_mode) {
                    ok = false;
                    fix = "sudo chmod " + *want_mode + " " + path;
                }
            }
            rows.push_back({"core", path, ok, detail, fix});
        }
    }
}

void check_net(const toml::table& net, const System& sys, std::vector<Row>& rows) {
    const std::string dev = net["device"].value_or(std::string("fjarr0"));
    const std::string owner = net["owner"].value_or(std::string("fjarr"));
    const int mtu = net["mtu"].value_or(1184);
    // spec: docs/27#lifecycle — created before the robot's software starts, owned by the agent's
    // account so it attaches without privilege; the one piece of setup that needs root. `net setup`
    // creates it now and enables the unit that recreates it at every boot (docs/26#fjarr-agent-net-setup).
    const std::string create = "sudo fjarr-agent net setup";
    for (const auto& unit : strings(net["units"])) {
        if (!sys.systemd) {
            // docs/26#containerized-robots: the entrypoint runs `net up` at every container start.
            rows.push_back({"net", "unit " + unit, true, "no systemd (a container): the image's entrypoint creates the device at every start", ""});
            continue;
        }
        const bool on = sys.unit_enabled(unit);
        rows.push_back({"net", "unit " + unit, on, on ? "enabled" : "not enabled: the device would be gone after a reboot", on ? "" : create});
    }
    const auto nd = sys.netdev(dev);
    if (!nd || !nd->tun) {
        rows.push_back({"net", "device " + dev, false, nd ? "exists but is not a tun device" : "missing", create});
        return;
    }
    const auto owner_uid = sys.uid_of(owner);
    const bool owned = owner_uid && nd->owner && *nd->owner == *owner_uid;
    rows.push_back({"net", "device " + dev, owned, nd->owner ? "tun, owner uid " + std::to_string(*nd->owner) : "tun, no owner",
                    owned ? "" : "sudo ip tuntap del dev " + dev + " mode tun && " + create});
    const bool mtu_ok = nd->mtu == mtu;
    rows.push_back({"net", dev + " mtu", mtu_ok, std::to_string(nd->mtu), mtu_ok ? "" : "sudo ip link set " + dev + " mtu " + std::to_string(mtu)});
}

} // namespace

std::vector<Row> check(const std::string& path, bool net_wanted, const System& sys) {
    std::vector<Row> rows;
    std::ifstream in(path);
    if (!in) {
        rows.push_back({"profile", path, true, "not installed — a development build; nothing to verify", ""});
        return rows;
    }
    std::stringstream text;
    text << in.rdbuf();
    toml::table tbl;
    try {
        tbl = toml::parse(text.str());
    } catch (const toml::parse_error& e) {
        rows.push_back({"profile", path, false, std::string("unreadable: ") + std::string(e.description()), "reinstall fjarr-agent"});
        return rows;
    }
    if (auto core = tbl["core"].as_table()) check_core(*core, sys, rows);
    if (net_wanted)
        if (auto net = tbl["net"].as_table()) check_net(*net, sys, rows);
    return rows;
}

bool all_ok(const std::vector<Row>& rows) {
    return std::all_of(rows.begin(), rows.end(), [](const Row& r) { return r.ok; });
}

System System::real() {
    System s;
    s.uid_of = [](const std::string& user) -> std::optional<unsigned> {
        if (const passwd* pw = ::getpwnam(user.c_str())) return pw->pw_uid;
        return std::nullopt;
    };
    s.groups_of = [](const std::string& user) {
        std::vector<std::string> out;
        const passwd* pw = ::getpwnam(user.c_str());
        if (!pw) return out;
        int n = 64;
        std::vector<gid_t> gids(n);
        if (::getgrouplist(user.c_str(), pw->pw_gid, gids.data(), &n) < 0) {
            gids.resize(n);
            ::getgrouplist(user.c_str(), pw->pw_gid, gids.data(), &n);
        }
        gids.resize(n);
        for (gid_t g : gids)
            if (const group* gr = ::getgrgid(g)) out.emplace_back(gr->gr_name);
        return out;
    };
    s.stat = [](const std::string& path) -> std::optional<Stat> {
        struct ::stat st {};
        if (::stat(path.c_str(), &st) != 0) return std::nullopt;
        return Stat{static_cast<unsigned>(st.st_uid), static_cast<unsigned>(st.st_mode & 07777), S_ISDIR(st.st_mode)};
    };
    s.systemd = std::filesystem::is_directory("/run/systemd/system");
    s.unit_enabled = [](const std::string& unit) {
        // Enabled = linked into some target's .wants, where `systemctl enable` and the package's
        // postinst put it. Read directly so the check needs neither systemctl nor a running systemd
        // (containers, a chroot).
        std::error_code ec;
        for (const auto& e : std::filesystem::directory_iterator("/etc/systemd/system", ec)) {
            const auto name = e.path().filename().string();
            if (name.size() > 6 && name.ends_with(".wants") && std::filesystem::exists(e.path() / unit, ec)) return true;
        }
        return false;
    };
    s.netdev = [](const std::string& name) -> std::optional<NetDev> {
        const std::filesystem::path base = std::filesystem::path("/sys/class/net") / name;
        std::error_code ec;
        if (!std::filesystem::exists(base, ec)) return std::nullopt;
        NetDev d;
        d.tun = std::filesystem::exists(base / "tun_flags", ec);
        const std::string owner = read_line(base / "owner");
        if (!owner.empty() && owner != "-1") d.owner = static_cast<unsigned>(std::stoul(owner));
        const std::string mtu = read_line(base / "mtu");
        d.mtu = mtu.empty() ? 0 : std::stoi(mtu);
        return d;
    };
    return s;
}

} // namespace fjarr::profile
