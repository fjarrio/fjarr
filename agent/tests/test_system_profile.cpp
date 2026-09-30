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
)";

struct Fake {
    std::map<std::string, unsigned> users{{"fjarr", 997}};
    std::map<std::string, std::vector<std::string>> groups{{"fjarr", {"fjarr", "video", "render"}}};
    std::map<std::string, System::Stat> files{{"/etc/fjarr/fjarr.toml", {0, 0644, false}},
                                              {"/var/lib/fjarr", {997, 0700, true}},
                                              {"/run/fjarr", {997, 0755, true}}};
    std::set<std::string> enabled{"fjarr-agent.service", "fjarr-net.service"};
    std::map<std::string, System::NetDev> devs{{"fjarr0", {true, 997u, 1184}}};

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
