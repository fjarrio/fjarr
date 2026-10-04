// The system profile check (docs/26#the-system-profile) against a fake machine: every row names what
// it found and, when it fails, the command that fixes it — the same answer on a .deb robot, an image,
// or later a NixOS or Yocto one.
#include <gtest/gtest.h>

#include <cstdio>
#include <fstream>
#include <map>
#include <set>

#include "core/system_profile.hpp"

using namespace fjarr::profile;

namespace {

const char* PROFILE = R"(
[core]
account = "fjarr"
groups  = ["video", "render"]
config  = "/etc/fjarr/fjarr.toml"
units   = ["fjarr-agent.service"]
dirs    = [ { path = "/var/lib/fjarr", owner = "fjarr", mode = "0700" }, { path = "/run/fjarr", owner = "fjarr" } ]
[net]
device = "fjarr0"
owner  = "fjarr"
mtu    = 1184
units  = ["fjarr-net.service"]

[desktop]
module = "/usr/lib/fjarr/desktop/libfjarr-desktop-mutter.so"
group  = "fjarr-desktop"
gdm    = "/etc/gdm3/custom.conf"
units  = ["fjarr-desktop-watchdog.timer"]
ghosts = "/etc/default/grub.d/fjarr-ghosts.cfg"

[desktop-x11]
module    = "/usr/lib/fjarr/desktop/libfjarr-desktop-x11.so"
autostart = "/etc/xdg/autostart/fjarr-x11-session.desktop"
)";

struct Fake {
    std::map<std::string, unsigned> users{{"fjarr", 997}};
    std::map<std::string, std::vector<std::string>> groups{{"fjarr", {"fjarr", "video", "render"}}};
    std::map<std::string, System::Stat> files{{"/etc/fjarr/fjarr.toml", {0, 0644, false}},
                                              {"/var/lib/fjarr", {997, 0700, true}},
                                              {"/run/fjarr", {997, 0755, true}}};
    std::set<std::string> enabled{"fjarr-agent.service", "fjarr-net.service"};
    std::map<std::string, System::NetDev> devs{{"fjarr0", {true, 997u, 1184}}};
    std::map<std::string, std::string> texts;

    System system() const {
        System s;
        s.uid_of = [this](const std::string& u) -> std::optional<unsigned> {
            auto it = users.find(u);
            return it == users.end() ? std::nullopt : std::optional<unsigned>(it->second);
        };
        s.groups_of = [this](const std::string& u) {
            auto it = groups.find(u);
            return it == groups.end() ? std::vector<std::string>{} : it->second;
        };
        s.stat = [this](const std::string& p) -> std::optional<System::Stat> {
            auto it = files.find(p);
            return it == files.end() ? std::nullopt : std::optional<System::Stat>(it->second);
        };
        s.unit_enabled = [this](const std::string& u) { return enabled.count(u) > 0; };
        s.read_file = [this](const std::string& p) -> std::optional<std::string> {
            auto it = texts.find(p);
            return it == texts.end() ? std::nullopt : std::optional<std::string>(it->second);
        };
        s.netdev = [this](const std::string& n) -> std::optional<System::NetDev> {
            auto it = devs.find(n);
            return it == devs.end() ? std::nullopt : std::optional<System::NetDev>(it->second);
        };
        return s;
    }
};

std::string profile_file() {
    const std::string path = ::testing::TempDir() + "fjarr-profile.toml";
    std::ofstream(path) << PROFILE;
    return path;
}

const Row* find(const std::vector<Row>& rows, const std::string& item) {
    for (const auto& r : rows)
        if (r.item == item) return &r;
    return nullptr;
}

} // namespace

TEST(SystemProfile, aMissingProfileIsADevelopmentBuildNotAFailure) {
    const auto rows = check("/nonexistent/profile.toml", true, Fake{}.system());
    ASSERT_EQ(rows.size(), 1u);
    EXPECT_TRUE(rows[0].ok);
    EXPECT_EQ(rows[0].feature, "profile");
}

TEST(SystemProfile, aCorrectRobotPassesEveryRow) {
    const auto rows = check(profile_file(), true, Fake{}.system());
    EXPECT_TRUE(all_ok(rows));
    EXPECT_NE(find(rows, "device fjarr0"), nullptr) << "net is checked when wanted";
}

TEST(SystemProfile, netIsCheckedOnlyWhenTheTunnelIsConfigured) {
    Fake f;
    f.devs.clear(); // no device: must not matter when fjarr.net is off
    const auto rows = check(profile_file(), false, f.system());
    EXPECT_TRUE(all_ok(rows));
    EXPECT_EQ(find(rows, "device fjarr0"), nullptr);
}

TEST(SystemProfile, eachFailureNamesItsFix) {
    Fake f;
    f.groups["fjarr"] = {"fjarr", "video"};                   // not in render
    f.files.erase("/etc/fjarr/fjarr.toml");                  // setup never ran
    f.files["/var/lib/fjarr"] = {997, 0755, true};           // key directory too open
    f.enabled.clear();                                       // unit not enabled
    const auto rows = check(profile_file(), false, f.system());
    EXPECT_FALSE(all_ok(rows));
    const Row* g = find(rows, "fjarr in render");
    ASSERT_TRUE(g);
    EXPECT_FALSE(g->ok);
    EXPECT_EQ(g->fix, "sudo usermod -aG render fjarr");
    EXPECT_EQ(find(rows, "/etc/fjarr/fjarr.toml")->fix, "sudo fjarr-agent setup");
    EXPECT_EQ(find(rows, "/var/lib/fjarr")->fix, "sudo chmod 0700 /var/lib/fjarr");
    EXPECT_EQ(find(rows, "unit fjarr-agent.service")->fix, "sudo systemctl enable --now fjarr-agent.service");
    EXPECT_TRUE(find(rows, "fjarr in video")->ok) << "only what is wrong fails";
}

TEST(SystemProfile, aTunnelDeviceMustBeATunOwnedByTheAgentWithTheChunkMtu) {
    Fake f;
    f.devs["fjarr0"] = {true, 1000u, 1280}; // owned by someone else, the old MTU
    auto rows = check(profile_file(), true, f.system());
    EXPECT_FALSE(find(rows, "device fjarr0")->ok);
    EXPECT_NE(find(rows, "device fjarr0")->fix.find("fjarr-agent net setup"), std::string::npos);
    EXPECT_EQ(find(rows, "fjarr0 mtu")->fix, "sudo ip link set fjarr0 mtu 1184");

    f.devs.clear();
    rows = check(profile_file(), true, f.system());
    EXPECT_FALSE(find(rows, "device fjarr0")->ok);
    EXPECT_EQ(find(rows, "device fjarr0")->detail, "missing");
    EXPECT_EQ(find(rows, "device fjarr0")->fix, "sudo fjarr-agent net setup");
}

TEST(SystemProfile, theBootUnitMustBeEnabledOrTheDeviceIsGoneAfterAReboot) {
    Fake f;
    f.enabled.erase("fjarr-net.service"); // the device exists now, but nothing recreates it at boot
    auto rows = check(profile_file(), true, f.system());
    const Row* u = find(rows, "unit fjarr-net.service");
    ASSERT_TRUE(u);
    EXPECT_FALSE(u->ok);
    EXPECT_EQ(u->fix, "sudo fjarr-agent net setup");
    EXPECT_TRUE(find(rows, "device fjarr0")->ok) << "the device row is judged on its own";
    EXPECT_EQ(find(check(profile_file(), false, f.system()), "unit fjarr-net.service"), nullptr) << "not a row when the tunnel is off";
}

TEST(SystemProfile, inAContainerUnitsAreSatisfiedByWhatDoesTheirJobThere) {
    // docs/26#the-system-profile: no systemd, no enabled links — the image's entrypoint recreates
    // the device at every container start and the runtime starts the agent. The device itself is
    // still checked like everywhere else.
    Fake f;
    f.enabled.clear();
    System sys = f.system();
    sys.systemd = false;
    auto rows = check(profile_file(), true, sys);
    const Row* net = find(rows, "unit fjarr-net.service");
    const Row* core = find(rows, "unit fjarr-agent.service");
    ASSERT_TRUE(net && core);
    EXPECT_TRUE(net->ok) << net->detail;
    EXPECT_NE(net->detail.find("entrypoint"), std::string::npos) << net->detail;
    EXPECT_TRUE(core->ok) << core->detail;
    f.devs.clear();
    sys = f.system();
    sys.systemd = false;
    EXPECT_FALSE(find(check(profile_file(), true, sys), "device fjarr0")->ok) << "a container without the device still fails on it";
}

TEST(SystemProfile, aPathThisUserCannotSeeIsReportedAsSuchNeverAsMissing) {
    // `fjarr-agent --check` without sudo: /etc/fjarr is 0750 root:fjarr, so stat fails with EACCES.
    // It said "missing" and named setup, and the operator went looking for a config that was there.
    Fake f;
    f.files["/etc/fjarr/fjarr.toml"] = {0, 0, false, true};
    auto rows = check(profile_file(), false, f.system());
    const Row* r = find(rows, "/etc/fjarr/fjarr.toml");
    ASSERT_TRUE(r);
    EXPECT_FALSE(r->ok);
    EXPECT_EQ(r->detail.find("missing"), std::string::npos) << r->detail;
    EXPECT_NE(r->detail.find("permission denied"), std::string::npos) << r->detail;
    EXPECT_EQ(r->fix, "sudo fjarr-agent --check");
    f.files["/var/lib/fjarr"] = {0, 0, false, true};
    const auto rows2 = check(profile_file(), false, f.system());
    const Row* d = find(rows2, "/var/lib/fjarr");
    ASSERT_TRUE(d);
    EXPECT_EQ(d->fix, "sudo fjarr-agent --check") << d->detail;
}

TEST(SystemProfile, aMissingAccountStopsTheRowsThatNeedIt) {
    Fake f;
    f.users.clear();
    const auto rows = check(profile_file(), false, f.system());
    EXPECT_FALSE(find(rows, "account fjarr")->ok);
    EXPECT_EQ(find(rows, "fjarr in video"), nullptr) << "no group rows for a user that does not exist";
}

// docs/26#fjarr-agent-setup-desktop: `setup desktop`'s work, verified.
namespace {
Fake desktop_robot() {
    Fake f;
    f.users["desktop"] = 1001;
    f.groups["desktop"] = {"desktop", "fjarr-desktop"};
    f.groups["fjarr"].push_back("fjarr-desktop");
    f.files["/usr/lib/fjarr/desktop/libfjarr-desktop-mutter.so"] = {0, 0644, false};
    f.texts["/etc/gdm3/custom.conf"] = "# GDM\n[daemon]\n#  AutomaticLogin = user1\nAutomaticLoginEnable=true\nAutomaticLogin=desktop\n\n[security]\n";
    f.enabled.insert("fjarr-desktop-watchdog.timer");
    return f;
}
} // namespace

TEST(SystemProfile, aSetUpDesktopPassesItsRowsAndTheyAreOnlyCheckedWhenConfigured) {
    const auto rows = check(profile_file(), false, std::optional<std::string>("desktop"), desktop_robot().system());
    EXPECT_TRUE(all_ok(rows));
    for (const auto* item : {"module", "account desktop", "desktop in fjarr-desktop", "fjarr in fjarr-desktop", "automatic login", "unit fjarr-desktop-watchdog.timer"})
        EXPECT_NE(find(rows, item), nullptr) << item;
    EXPECT_EQ(find(check(profile_file(), false, std::nullopt, Fake{}.system()), "automatic login"), nullptr) << "no desktop account configured: no desktop rows";
}

TEST(SystemProfile, eachDesktopFailureNamesItsFix) {
    Fake f = desktop_robot();
    f.files.erase("/usr/lib/fjarr/desktop/libfjarr-desktop-mutter.so");
    f.groups["fjarr"] = {"fjarr", "video", "render"}; // the agent left out of the helper's group
    f.texts["/etc/gdm3/custom.conf"] = "[daemon]\nAutomaticLoginEnable=true\nAutomaticLogin=someoneelse\n";
    f.enabled.erase("fjarr-desktop-watchdog.timer");
    const auto rows = check(profile_file(), false, std::optional<std::string>("desktop"), f.system());
    EXPECT_FALSE(all_ok(rows));
    EXPECT_EQ(find(rows, "module")->fix, "sudo apt install fjarr-desktop-wayland");
    EXPECT_EQ(find(rows, "fjarr in fjarr-desktop")->fix, "sudo usermod -aG fjarr-desktop fjarr");
    EXPECT_FALSE(find(rows, "automatic login")->ok) << "a login for another account is not ours";
    EXPECT_EQ(find(rows, "unit fjarr-desktop-watchdog.timer")->fix, "sudo fjarr-agent setup desktop");
}

TEST(SystemProfile, aGhostIsActiveOnceBootedAndPendingIsNotAFailure) {
    Fake f = desktop_robot();
    f.texts["/etc/default/grub.d/fjarr-ghosts.cfg"] = "# fjarr-ghosts: DP-2=1:1920x1080@60 HDMI-A-2=2:1280x720@60\nGRUB_CMDLINE_LINUX_DEFAULT=...\n";
    f.texts["/proc/cmdline"] = "BOOT_IMAGE=/vmlinuz quiet video=DP-2:1920x1080@60e drm.edid_firmware=DP-2:edid/fjarr-ghost-1.bin";
    const auto rows = check(profile_file(), false, std::optional<std::string>("desktop"), f.system());
    EXPECT_TRUE(all_ok(rows));
    EXPECT_EQ(find(rows, "ghost DP-2")->detail, "active");
    EXPECT_EQ(find(rows, "ghost HDMI-A-2")->detail, "reboot pending");
}

TEST(SystemProfile, aRealMonitorOnAGhostConnectorIsAFailureFoundOverDdc) {
    // The mini-PC, 2026-10-01 (#37): a DELL on the ghost's HDMI-A-1 reads as the ghost through the
    // kernel and as itself over the port's DDC bus.
    Fake f = desktop_robot();
    f.texts["/etc/default/grub.d/fjarr-ghosts.cfg"] = "# fjarr-ghosts: HDMI-A-1=1:1920x1080@60\n";
    f.texts["/proc/cmdline"] = "quiet video=HDMI-A-1:1920x1080@60e";
    auto sys = f.system();
    sys.ddc_monitor = [](const std::string&) -> std::optional<std::string> { return "DELL U2422H"; };
    auto rows = check(profile_file(), false, std::optional<std::string>("desktop"), sys);
    EXPECT_FALSE(find(rows, "ghost HDMI-A-1")->ok);
    EXPECT_EQ(find(rows, "ghost HDMI-A-1")->fix, "unplug it, or: sudo fjarr-agent display remove-ghost HDMI-A-1");
    sys.ddc_monitor = [](const std::string&) -> std::optional<std::string> { return std::nullopt; }; // nothing answers
    rows = check(profile_file(), false, std::optional<std::string>("desktop"), sys);
    EXPECT_TRUE(find(rows, "ghost HDMI-A-1")->ok);
}

TEST(SystemProfile, anX11KioskNeedsItsModuleAndTheSessionEntryEachWithItsFix) {
    // docs/26#fjarr-agent-setup-desktop: the X11 branch links the kiosk session's autostart entry.
    Fake f;
    auto rows = fjarr::profile::check_desktop_x11(profile_file(), f.system());
    ASSERT_NE(find(rows, "module"), nullptr);
    EXPECT_FALSE(find(rows, "module")->ok);
    EXPECT_EQ(find(rows, "module")->fix, "sudo apt install fjarr-desktop-x11");
    ASSERT_NE(find(rows, "kiosk session entry"), nullptr);
    EXPECT_EQ(find(rows, "kiosk session entry")->fix, "sudo fjarr-agent setup desktop --x11");
    f.files["/usr/lib/fjarr/desktop/libfjarr-desktop-x11.so"] = {0, 0644, false};
    f.files["/etc/xdg/autostart/fjarr-x11-session.desktop"] = {0, 0644, false};
    rows = fjarr::profile::check_desktop_x11(profile_file(), f.system());
    EXPECT_TRUE(fjarr::profile::all_ok(rows));
    EXPECT_EQ(rows.size(), 2u);
}
