#include <cstdlib>

#include <gtest/gtest.h>

#include <fjarr/agent.hpp>
#include <fjarr/errors.hpp>

TEST(Config, parsesTheDocs23Example) {
    const char* toml = R"(
[agent]
robot_id = "robot-024"
server_url = "wss://fjarr.acme.com/ws"
ice_policy = "relay"
[media]
encoder = "software"
gop_seconds = 3
[introspect]
port = 7391
[capabilities."fjarr.test"]
enabled = true
test_hooks = true
[capabilities."fjarr.camera".tracks.front]
label = "Front"
source = "videotestsrc"
)";
    auto c = fjarr::AgentConfig::from_toml(toml);
    EXPECT_EQ(c.agent.robot_id, "robot-024");
    EXPECT_EQ(c.agent.ice_policy, "relay");
    EXPECT_EQ(c.media.encoder, "software");
    EXPECT_EQ(c.media.gop_seconds, 3);
    EXPECT_EQ(c.introspect.port, 7391);
    EXPECT_TRUE(c.capabilities.at("fjarr.test").value("test_hooks", false));
    EXPECT_EQ(c.capabilities.at("fjarr.camera")["tracks"]["front"]["source"], "videotestsrc");
    c.validate();
}

TEST(Config, envOverridesWin) {
    auto c = fjarr::AgentConfig::from_toml("[agent]\nrobot_id = \"a\"\nserver_url = \"ws://x/ws\"\n");
    setenv("FJARR_ROBOT_ID", "from-env", 1);
    setenv("FJARR_INTROSPECT_PORT", "7999", 1);
    setenv("FJARR_TEST_HOOKS", "1", 1);
    c.apply_env();
    unsetenv("FJARR_ROBOT_ID");
    unsetenv("FJARR_INTROSPECT_PORT");
    unsetenv("FJARR_TEST_HOOKS");
    EXPECT_EQ(c.agent.robot_id, "from-env");
    EXPECT_EQ(c.introspect.port, 7999);
    EXPECT_TRUE(c.capabilities["fjarr.test"]["test_hooks"].get<bool>());
}

TEST(Config, rejectsWhatItMust) {
    EXPECT_THROW(fjarr::AgentConfig::from_toml("[agent\nrobot_id = 1"), fjarr::FjarrError);
    auto c = fjarr::AgentConfig::from_toml("[agent]\nrobot_id = \"a\"\nserver_url = \"http://x\"\n");
    EXPECT_THROW(c.validate(), fjarr::FjarrError);
    auto d = fjarr::AgentConfig::from_toml("[agent]\nrobot_id = \"a\"\n[introspect]\nbind = \"0.0.0.0\"\n");
    EXPECT_THROW(d.validate(), fjarr::FjarrError); // LAN exposure needs a token (docs/24)
}
