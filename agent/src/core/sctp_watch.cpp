// spec: docs/08-protocol.md#datachannel-topology · docs/27-network-tunnel.md#testing
#include "core/sctp_watch.hpp"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string_view>

#include <gst/gst.h>

namespace fjarr::sctp_watch {
namespace {

constexpr const char* CATEGORY = "sctpassociation";
constexpr std::string_view MARKER = "SCTP_SEND_FAILED";

std::atomic<unsigned long> g_abandoned{0};
std::atomic<unsigned> g_last_error{0};
std::mutex g_line_mutex;
std::string g_last_line;
std::once_flag g_installed;

/// Runs on whichever thread usrsctp delivers the notification on — hence the atomics.
void on_log(GstDebugCategory* category, GstDebugLevel level, const gchar*, const gchar*, gint, GObject*,
            GstDebugMessage* message, gpointer) {
    if (level > GST_LEVEL_ERROR || !category || std::strcmp(gst_debug_category_get_name(category), CATEGORY) != 0) return;
    const gchar* text = gst_debug_message_get(message);
    if (!text) return;
    const std::string_view line(text);
    if (line.find(MARKER) == std::string_view::npos) return;
    // "Event: SCTP_SEND_FAILED_EVENT (12)" — the number is usrsctp's ssfe_error; the older
    // SCTP_SEND_FAILED form carries none.
    unsigned code = 0;
    if (const auto open = line.rfind('('); open != std::string_view::npos) code = static_cast<unsigned>(std::strtoul(text + open + 1, nullptr, 10));
    g_abandoned.fetch_add(1, std::memory_order_relaxed);
    g_last_error.store(code, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_line_mutex);
    g_last_line.assign(line);
}

} // namespace

void install() {
    std::call_once(g_installed, [] {
        // Without GST_DEBUG the default logger is still registered and would print every raised
        // ERROR line to stderr; an abandonment burst is thousands of them. Someone who set
        // GST_DEBUG asked for stderr and keeps it.
        if (!std::getenv("GST_DEBUG")) gst_debug_remove_log_function(gst_debug_log_default);
        gst_debug_add_log_function(&on_log, nullptr, nullptr);
        // Applied when the category is created too, so it does not matter that the sctp plugin
        // loads long after this runs.
        gst_debug_set_threshold_for_name(CATEGORY, GST_LEVEL_ERROR);
        gst_debug_set_active(TRUE);
    });
}

unsigned long abandoned() { return g_abandoned.load(std::memory_order_relaxed); }
unsigned last_error() { return g_last_error.load(std::memory_order_relaxed); }
std::string last_line() {
    std::lock_guard<std::mutex> lk(g_line_mutex);
    return g_last_line;
}

} // namespace fjarr::sctp_watch
