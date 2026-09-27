// The abandonment counter is a log function, so what it must get right is the filter: the one
// message that means usrsctp threw a message away, on the one category that reports it, at the one
// level it is reported at — and nothing else. The lines here are the exact text gst-plugins-bad
// 1.28 emits (ext/sctp/sctpassociation.c, handle_notification).
// spec: docs/08-protocol.md#datachannel-topology (link-stats `abandoned`)
#include <gst/gst.h>
#include <gtest/gtest.h>

#include "core/sctp_watch.hpp"

namespace {

// The real category is created by the sctp plugin when it loads, and the plugin never loads in
// this process. `_INIT` creates it by name — or returns the existing one — exactly as the plugin's
// own `GST_DEBUG_CATEGORY_INIT` would, so the watch sees the same category object either way.
// (`_GET` only finds an existing category, returns NULL here, and made the negative tests below pass
// without logging anything at all.)
GstDebugCategory* category(const char* name) {
    GstDebugCategory* cat = nullptr;
    GST_DEBUG_CATEGORY_INIT(cat, name, 0, "sctp watch test category");
    return cat;
}

void emit(const char* cat_name, GstDebugLevel level, const char* text) {
    gst_debug_log(category(cat_name), level, __FILE__, "emit", __LINE__, nullptr, "%s", text);
}

TEST(SctpWatch, countsEachAbandonedMessageAndKeepsUsrsctpsReason) {
    fjarr::sctp_watch::install();
    const auto before = fjarr::sctp_watch::abandoned();
    emit("sctpassociation", GST_LEVEL_ERROR, "Event: SCTP_SEND_FAILED_EVENT (12)");
    EXPECT_EQ(fjarr::sctp_watch::abandoned(), before + 1);
    EXPECT_EQ(fjarr::sctp_watch::last_error(), 12u);
    EXPECT_NE(fjarr::sctp_watch::last_line().find("SCTP_SEND_FAILED_EVENT"), std::string::npos);
    // The pre-event form, which carries no reason: counted, reason 0.
    emit("sctpassociation", GST_LEVEL_ERROR, "Event: SCTP_SEND_FAILED");
    EXPECT_EQ(fjarr::sctp_watch::abandoned(), before + 2);
    EXPECT_EQ(fjarr::sctp_watch::last_error(), 0u);
}

TEST(SctpWatch, everythingElseOnThatCategoryIsNotAnAbandonment) {
    fjarr::sctp_watch::install();
    const auto before = fjarr::sctp_watch::abandoned();
    emit("sctpassociation", GST_LEVEL_ERROR, "Event: SCTP_REMOTE_ERROR (3)");
    emit("sctpassociation", GST_LEVEL_DEBUG, "Event: SCTP_SENDER_DRY_EVENT");
    EXPECT_EQ(fjarr::sctp_watch::abandoned(), before) << "a remote error or a dry sender is not a dropped message";
}

TEST(SctpWatch, theSameWordsOnAnotherCategoryDoNotCount) {
    fjarr::sctp_watch::install();
    const auto before = fjarr::sctp_watch::abandoned();
    // A category that mentions the event in passing — a log line quoting one, say — must not
    // inflate a counter a support engineer reads as "messages the transport threw away".
    gst_debug_set_threshold_for_name("fjarr-test-other", GST_LEVEL_ERROR);
    emit("fjarr-test-other", GST_LEVEL_ERROR, "saw SCTP_SEND_FAILED_EVENT (1) upstream");
    EXPECT_EQ(fjarr::sctp_watch::abandoned(), before);
}

} // namespace
