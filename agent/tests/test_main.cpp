// The test runner: GStreamer's `leaks` tracer brackets every test case (docs/23 memory ladder,
// layer 3): an element, pad, buffer, sample or promise created during a test and still alive
// when it ends fails that test, with the object list. FJARR_LEAKS_TRACES=1 adds stack traces.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

#include <gst/gst.h>
#include <gtest/gtest.h>

#include "core/log.hpp"
#include "introspect/memory.hpp"

namespace {

struct LeaksListener final : ::testing::EmptyTestEventListener {
    fjarr::introspect::MemoryCensus census;
    std::string token;
    void OnTestStart(const ::testing::TestInfo&) override { token = census.checkpoint().value("checkpoint", ""); }
    void OnTestEnd(const ::testing::TestInfo& info) override {
        if (!fjarr::introspect::MemoryCensus::leaks_tracer_active() || info.result()->Failed()) return;
        // Frees that trail the test body (a streaming thread's last unref) get a short settle first.
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        const nlohmann::json diff = census.since(token);
        if (std::getenv("FJARR_LEAKS_DEBUG")) std::fprintf(stderr, "[leaks] %s\n", diff.dump().c_str());
        const auto created = diff.value("leaks", nlohmann::json::object()).value("created", nlohmann::json::array());
        if (!created.empty()) {
            ADD_FAILURE() << "GStreamer objects created during the test and still alive at its end (leaks tracer):\n" << created.dump(2)
                          << "\ncensus diff: " << diff["diff"]["census"].dump();
        }
    }
};

} // namespace

int main(int argc, char** argv) {
    if (!std::getenv("GST_TRACERS")) {
        const bool traces = std::getenv("FJARR_LEAKS_TRACES") != nullptr;
        setenv("GST_TRACERS",
               traces ? "leaks(filters=\"GstElement,GstPad,GstBuffer,GstSample,GstPromise\",stack-traces-flags=full)"
                      : "leaks(filters=\"GstElement,GstPad,GstBuffer,GstSample,GstPromise\")",
               0);
        setenv("GST_DEBUG", std::getenv("GST_DEBUG") ? std::getenv("GST_DEBUG") : "GST_TRACER:1", 0);
    }
    gst_init(&argc, &argv);
    if (const char* lvl = std::getenv("FJARR_LOG_LEVEL")) fjarr::log::set_level(std::string_view(lvl));
    ::testing::InitGoogleTest(&argc, argv);
    // Our listener runs before the default printer so a leak shows as the test's failure, not after its OK line.
    auto& listeners = ::testing::UnitTest::GetInstance()->listeners();
    auto* printer = listeners.Release(listeners.default_result_printer());
    listeners.Append(new LeaksListener);
    listeners.Append(printer);
    return RUN_ALL_TESTS();
}
