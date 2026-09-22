// Slice 3c: the log ring, the memory census with checkpoints, the bundle writer, and the
// endpoint's new routes (/memory, /log, /stats, /events, /diagnostics.tar.gz) over a real
// libsoup client against a server on the core loop.
#include <chrono>
#include <thread>

#include <gst/gst.h>
#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>
#include <libsoup/soup.h>

#include "core/glib/raii.hpp"
#include "core/log.hpp"
#include "core/loop.hpp"
#include "introspect/bundle.hpp"
#include "introspect/introspector.hpp"
#include "introspect/memory.hpp"
#include "introspect/server.hpp"

namespace glib = fjarr::glib;
using namespace fjarr::introspect;

namespace {

struct Http {
    glib::GObjectPtr<SoupSession> session{soup_session_new_with_options("timeout", 5u, nullptr)};
    struct Reply {
        unsigned status = 0;
        std::string body;
    };
    struct Header {
        const char* name;
        std::string value;
    };
    std::string last_content_type, last_cache_control;
    Reply call(const char* method, const std::string& url, std::vector<Header> headers = {}) {
        glib::GObjectPtr<SoupMessage> msg(soup_message_new(method, url.c_str()));
        for (const auto& h : headers) soup_message_headers_append(soup_message_get_request_headers(msg.get()), h.name, h.value.c_str());
        GError* err = nullptr;
        glib::GBytesPtr bytes(soup_session_send_and_read(session.get(), msg.get(), nullptr, &err));
        if (err) {
            glib::GErrorPtr e(err);
            ADD_FAILURE() << url << ": " << err->message;
            return {};
        }
        gsize n = 0;
        const auto* d = static_cast<const char*>(g_bytes_get_data(bytes.get(), &n));
        const char* ct = soup_message_headers_get_one(soup_message_get_response_headers(msg.get()), "Content-Type");
        const char* cc = soup_message_headers_get_one(soup_message_get_response_headers(msg.get()), "Cache-Control");
        last_content_type = ct ? ct : "";
        last_cache_control = cc ? cc : "";
        return {soup_message_get_status(msg.get()), std::string(d ? d : "", n)};
    }
    /// Read an SSE stream until `until` appears or the deadline passes.
    std::string stream_until(const std::string& url, const std::string& until, std::function<void()> after_open, int ms = 3000) {
        glib::GObjectPtr<SoupMessage> msg(soup_message_new("GET", url.c_str()));
        GError* err = nullptr;
        glib::GObjectPtr<GInputStream> in(soup_session_send(session.get(), msg.get(), nullptr, &err));
        if (err) {
            glib::GErrorPtr e(err);
            ADD_FAILURE() << url << ": " << err->message;
            return "";
        }
        EXPECT_EQ(soup_message_get_status(msg.get()), 200u);
        after_open();
        std::string out;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
        char buf[4096];
        auto complete = [&] { // the marker and the frame terminator after it (frames can span reads)
            const auto at = out.find(until);
            return at != std::string::npos && out.find("\n\n", at) != std::string::npos;
        };
        while (std::chrono::steady_clock::now() < deadline && !complete()) {
            const gssize n = g_input_stream_read(in.get(), buf, sizeof buf, nullptr, nullptr);
            if (n <= 0) break;
            out.append(buf, static_cast<std::size_t>(n));
        }
        return out;
    }
};

glib::GstElementPtr fake_pipeline(const char* name) {
    glib::GstElementPtr p = glib::sink_element(gst_pipeline_new(name));
    GstElement* src = gst_element_factory_make("fakesrc", (std::string(name) + "/source").c_str());
    GstElement* sink = gst_element_factory_make("fakesink", (std::string(name) + "/sink").c_str());
    gst_bin_add_many(GST_BIN(p.get()), src, sink, nullptr);
    gst_element_link(src, sink);
    return p;
}

} // namespace

TEST(LogRing, keepsInfoAndAboveInOrderAndBounded) {
    fjarr::log::clear_ring();
    fjarr::log::set_level(fjarr::log::Level::Warn); // the filter does not decide what the ring keeps
    fjarr::log::info("test", "ring-info", {{"k", "v"}});
    fjarr::log::debug("test", "ring-debug-not-kept");
    fjarr::log::warn("test", "ring-warn");
    fjarr::log::set_level(fjarr::log::Level::Info);
    const auto lines = fjarr::log::recent();
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_NE(lines[0].find("ring-info k=v"), std::string::npos);
    EXPECT_NE(lines[1].find("ring-warn"), std::string::npos);
    for (int i = 0; i < 4100; i++) fjarr::log::info("test", "fill");
    EXPECT_EQ(fjarr::log::recent().size(), 4000u);
    fjarr::log::clear_ring();
}

TEST(MemoryCensus, checkpointDiffSeesWrappersCreatedSince) {
    MemoryCensus census([] { return nlohmann::json{{"extra_number", 7}}; });
    const nlohmann::json now = census.now();
    EXPECT_TRUE(now.contains("rss_bytes"));
    EXPECT_GT(now["rss_bytes"].get<std::uint64_t>(), 0u);
    EXPECT_EQ(now["extra_number"], 7);
    const std::string token = census.checkpoint().value("checkpoint", "");
    ASSERT_FALSE(token.empty());
    {
        glib::GstElementPtr held = glib::make_element("identity", "census-probe");
        const nlohmann::json d = census.since(token);
        EXPECT_EQ(d["diff"]["census"]["elements"], 1) << d.dump();
        EXPECT_EQ(d["diff"]["extra_number"], 0);
    }
    EXPECT_EQ(census.since(token)["diff"]["census"]["elements"], 0);
    EXPECT_TRUE(census.since("cp-nope").contains("error"));
}

TEST(Bundle, tarGzRoundTripListsEveryMember) {
    BundleFiles files{{"fjarr-diagnostics/README.txt", "hello\n"},
                      {"fjarr-diagnostics/pipelines/session:0192abcd-3-select-tracks.json", std::string(1500, 'x')},
                      {"fjarr-diagnostics/log.txt", ""}};
    const std::string gz = gzip(tar(files));
    ASSERT_GT(gz.size(), 10u);
    EXPECT_EQ(static_cast<unsigned char>(gz[0]), 0x1f); // gzip magic
    EXPECT_EQ(static_cast<unsigned char>(gz[1]), 0x8b);
    const std::string archive = gunzip(gz);
    EXPECT_EQ(archive.size() % 512, 0u);
    const auto names = tar_names(archive);
    ASSERT_EQ(names.size(), 3u);
    EXPECT_EQ(names[1], "fjarr-diagnostics/pipelines/session:0192abcd-3-select-tracks.json");
}

TEST(Introspect, endpointServesMemoryLogStatsEventsAndTheBundle) {
    fjarr::CoreLoop loop;
    loop.start();
    SnapshotStore store(8, "", [&loop](std::chrono::milliseconds d, std::function<void()> fn) {
        return loop.add_timeout(d, [fn] {
            fn();
            return false;
        });
    });
    MemoryCensus census;
    fjarr::AgentConfig::IntrospectSection cfg;
    cfg.port = 0; // ephemeral
    Providers providers;
    providers.memory = &census;
    providers.stats = [] { return nlohmann::json{{"sessions", nlohmann::json::array()}}; };
    providers.sources = [] { return nlohmann::json{{"sources", nlohmann::json::array()}}; };
    providers.bundle_extra = [] { return nlohmann::json{{"config", {{"agent", {{"robot_id", "r"}}}}}, {"versions", {{"fjarr", "t"}}}, {"check", "ok\n"}}; };
    std::unique_ptr<Server> server;
    loop.call_sync([&] {
        server = std::make_unique<Server>(cfg, store, std::move(providers));
        server->start();
    });
    const std::string base = "http://127.0.0.1:" + std::to_string(server->port());
    Http http;

    auto mem = http.call("GET", base + "/memory");
    EXPECT_EQ(mem.status, 200u);
    EXPECT_TRUE(nlohmann::json::parse(mem.body).contains("census"));
    auto cp = http.call("POST", base + "/memory/checkpoint");
    const std::string token = nlohmann::json::parse(cp.body).value("checkpoint", "");
    ASSERT_FALSE(token.empty());
    auto since = http.call("GET", base + "/memory?since=" + token);
    EXPECT_TRUE(nlohmann::json::parse(since.body).contains("diff"));

    fjarr::log::info("introspect-test", "a-line-for-the-ring");
    EXPECT_NE(http.call("GET", base + "/log?minutes=1").body.find("a-line-for-the-ring"), std::string::npos);

    auto stats = http.call("GET", base + "/stats");
    EXPECT_EQ(nlohmann::json::parse(stats.body)["events_clients"], 0);

    // /events: a snapshot taken on the loop reaches an open stream as one SSE frame.
    glib::GstElementPtr pipeline = fake_pipeline("producer:evt");
    const std::string stream = http.stream_until(
        base + "/events?body=txt", "event: snapshot",
        [&] {
            loop.call_sync([&] {
                SnapshotMeta meta;
                meta.pipeline_id = "producer:evt";
                meta.kind = "producer";
                meta.trigger = "state-changed";
                store.take(GST_BIN(pipeline.get()), meta, true);
            });
        });
    EXPECT_NE(stream.find("retry: 1000"), std::string::npos);
    EXPECT_NE(stream.find("id: producer:evt@1\nevent: snapshot\ndata: {"), std::string::npos) << stream;
    EXPECT_NE(stream.find("\ndata: producer:evt"), std::string::npos) << stream; // the txt body, newlines escaped

    auto bundle = http.call("GET", base + "/diagnostics.tar.gz");
    EXPECT_EQ(bundle.status, 200u);
    const auto names = tar_names(gunzip(bundle.body));
    EXPECT_NE(std::find(names.begin(), names.end(), "fjarr-diagnostics/README.txt"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "fjarr-diagnostics/pipelines/producer:evt-1-state-changed.txt"), names.end());
    EXPECT_NE(std::find(names.begin(), names.end(), "fjarr-diagnostics/log.txt"), names.end());

    loop.call_sync([&] { server.reset(); });
    loop.stop();
}

// docs/24#the-viewer, slice 5b: the viewer's files at `/`, the API shadowing them, traversal refused,
// the token gating the API only.
TEST(Introspect, servesTheViewerDirectoryUnderTheApiAndTheTokenGatesOnlyTheApi) {
    namespace fs = std::filesystem;
    const fs::path dir = fs::temp_directory_path() / ("fjarr-viewer-" + std::to_string(::getpid()));
    fs::create_directories(dir / "assets");
    auto write = [&](const fs::path& p, const std::string& body) {
        std::ofstream f(p);
        f << body;
    };
    write(dir / "index.html", "<html><body>fjarr viewer</body></html>");
    write(dir / "assets" / "app-Ab12Cd34.js", "console.log('app')");
    write(dir / "plain.css", "body{}");
    write(dir / "pipelines", "a file the API route shadows");
    fs::create_symlink("/etc/hostname", dir / "escape"); // a symlink out of the directory

    fjarr::CoreLoop loop;
    loop.start();
    SnapshotStore store(8);
    MemoryCensus census;
    fjarr::AgentConfig::IntrospectSection cfg;
    cfg.port = 0;
    cfg.token = "t0k";
    cfg.viewer_dir = dir.string();
    Providers providers;
    providers.memory = &census;
    std::unique_ptr<Server> server;
    loop.call_sync([&] {
        server = std::make_unique<Server>(cfg, store, std::move(providers));
        server->start();
    });
    const std::string base = "http://127.0.0.1:" + std::to_string(server->port());
    Http http;

    auto index = http.call("GET", base + "/"); // no token: the viewer is public, its data is not
    EXPECT_EQ(index.status, 200u);
    EXPECT_NE(index.body.find("fjarr viewer"), std::string::npos);
    EXPECT_EQ(http.last_content_type.rfind("text/html", 0), 0u) << http.last_content_type;
    auto js = http.call("GET", base + "/assets/app-Ab12Cd34.js");
    EXPECT_EQ(js.status, 200u);
    EXPECT_EQ(http.last_content_type.rfind("text/javascript", 0), 0u);
    EXPECT_NE(http.last_cache_control.find("immutable"), std::string::npos) << "a hashed asset is cached forever";
    http.call("GET", base + "/plain.css");
    EXPECT_EQ(http.last_cache_control, "no-cache");
    EXPECT_EQ(http.call("GET", base + "/nope.js").status, 404u);
    EXPECT_EQ(http.call("GET", base + "/escape").status, 404u) << "a symlink out of the directory";
    auto trav = http.call("GET", base + "/assets/..%2F..%2F..%2Fetc%2Fpasswd");
    EXPECT_EQ(trav.body.find("root:"), std::string::npos) << "traversal must never leave the directory";
    EXPECT_EQ(http.call("POST", base + "/index.html").status, 405u);
    // the API shadows a file of the same name, and needs the token
    EXPECT_EQ(http.call("GET", base + "/pipelines").status, 401u);
    auto list = http.call("GET", base + "/pipelines", {{"Authorization", "Bearer t0k"}});
    EXPECT_EQ(list.status, 200u);
    EXPECT_NE(list.body.find("\"pipelines\""), std::string::npos) << list.body;
    loop.call_sync([&] { server.reset(); });

    // without the directory, `/` is the text index naming the key
    cfg.viewer_dir.clear();
    cfg.token.clear();
    Providers p2;
    p2.memory = &census;
    loop.call_sync([&] {
        server = std::make_unique<Server>(cfg, store, std::move(p2));
        server->start();
    });
    auto text = http.call("GET", "http://127.0.0.1:" + std::to_string(server->port()) + "/");
    EXPECT_EQ(text.status, 200u);
    EXPECT_NE(text.body.find("viewer_dir"), std::string::npos);
    EXPECT_EQ(http.call("GET", "http://127.0.0.1:" + std::to_string(server->port()) + "/index.html").status, 404u);
    loop.call_sync([&] { server.reset(); });
    loop.stop();
    fs::remove_all(dir);
}

// The gate's self-check (docs/23 ladder layer 3): run with --gtest_also_run_disabled_tests and it
// MUST fail — a deliberately leaked element is reported by the leaks-tracer bracketing in
// test_main.cpp. `make agent-leaks-selftest` asserts exactly that.
TEST(LeaksGate, DISABLED_deliberateLeakIsCaughtByTheBracketing) {
    GstElement* leaked = gst_element_factory_make("identity", "deliberately-leaked");
    ASSERT_NE(leaked, nullptr);
    // (no unref: the listener must fail this test)
}
