// Envelope + signaling guards against the golden fixtures (docs/08, shared with TS and Rust).
#include <filesystem>
#include <fstream>
#include <sstream>

#include <gtest/gtest.h>

#include <fjarr/errors.hpp>

#include "core/protocol.hpp"

namespace fs = std::filesystem;
using namespace fjarr::protocol;
using fjarr::Envelope;

namespace {
std::string read(const fs::path& p) {
    std::ifstream f(p);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}
} // namespace

TEST(Protocol, validEnvelopeFixturesParseAndInvalidOnesDoNot) {
    int valid = 0, invalid = 0;
    for (const auto& e : fs::directory_iterator(fs::path(FJARR_FIXTURES_DIR) / "valid")) {
        if (e.path().filename().string().rfind("env-", 0) != 0) continue;
        EXPECT_TRUE(parse_envelope(read(e.path())).has_value()) << e.path();
        valid++;
    }
    for (const auto& e : fs::directory_iterator(fs::path(FJARR_FIXTURES_DIR) / "invalid")) {
        if (e.path().filename().string().rfind("env-", 0) != 0) continue;
        EXPECT_FALSE(parse_envelope(read(e.path())).has_value()) << e.path();
        invalid++;
    }
    EXPECT_GT(valid, 3);
    EXPECT_GT(invalid, 2);
}

TEST(Protocol, validSignalingFixturesParseAndInvalidOnesDoNot) {
    for (const auto& e : fs::directory_iterator(fs::path(FJARR_FIXTURES_DIR) / "valid")) {
        if (e.path().filename().string().rfind("sig-", 0) != 0) continue;
        EXPECT_TRUE(parse_signaling(read(e.path())).has_value()) << e.path();
    }
    for (const auto& e : fs::directory_iterator(fs::path(FJARR_FIXTURES_DIR) / "invalid")) {
        const auto name = e.path().filename().string();
        if (name.rfind("sig-", 0) != 0) continue;
        bool higher = false;
        auto m = parse_signaling(read(e.path()), &higher);
        if (name == "sig-unknown-major-version.json") {
            EXPECT_FALSE(m.has_value());
            EXPECT_TRUE(higher);
            continue;
        }
        // Agent-bound types are validated structurally; operator-bound types are tolerated (ignored later).
        if (m) EXPECT_TRUE(m->type == "offer" || m->type == "hello" || m->type == "hello-ack" || m->type == "error" || m->type == "ice") << name;
    }
}

TEST(Protocol, envelopeRoundTripAndSizeLimit) {
    Envelope e = make_envelope("fjarr.test", "echo", "request", nlohmann::json{{"x", 1}});
    auto back = parse_envelope(serialize_envelope(e));
    ASSERT_TRUE(back);
    EXPECT_EQ(back->cap, "fjarr.test");
    EXPECT_EQ(back->event_id, e.event_id);
    Envelope big = make_envelope("fjarr.test", "echo", "event", nlohmann::json{{"blob", std::string(17000, 'x')}});
    EXPECT_THROW(serialize_envelope(big), fjarr::FjarrError);
}

TEST(Protocol, capNamesAndEventIds) {
    EXPECT_TRUE(valid_cap_name("fjarr.camera"));
    EXPECT_TRUE(valid_cap_name("com.acme.arm-teach"));
    EXPECT_FALSE(valid_cap_name("camera"));
    EXPECT_FALSE(valid_cap_name("Fjarr.camera"));
    EXPECT_FALSE(valid_cap_name("fjarr."));
    const auto a = new_event_id(), b = new_event_id();
    EXPECT_NE(a, b);
    EXPECT_EQ(a.size(), 36u);
}

TEST(Protocol, manifestJsonMatchesTheWireShape) {
    ManifestEntry t;
    t.track_id = "test-pattern";
    t.cap = "fjarr.test";
    t.label = "Test pattern";
    t.codec = "H264";
    t.pt = 96;
    t.mid = "0";
    auto j = manifest_to_json({t});
    EXPECT_EQ(j[0]["kind"], "video");
    EXPECT_TRUE(j[0]["monitor"].is_null());
    EXPECT_EQ(j[0]["mid"], "0");
}
