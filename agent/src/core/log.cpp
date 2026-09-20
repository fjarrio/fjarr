#include "log.hpp"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

namespace fjarr::log {

namespace {
std::atomic<Level> g_level{Level::Info};
std::atomic<bool> g_json{false};
std::mutex g_mutex;

const char* name_of(Level l) {
    switch (l) {
    case Level::Trace: return "TRACE";
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warn: return "WARN";
    case Level::Error: return "ERROR";
    }
    return "?";
}

std::string timestamp() {
    using namespace std::chrono;
    const auto now = system_clock::now();
    const auto ms = duration_cast<milliseconds>(now.time_since_epoch()) % 1000;
    const std::time_t t = system_clock::to_time_t(now);
    std::tm tm{};
    gmtime_r(&t, &tm);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1,
                  tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec, static_cast<int>(ms.count()));
    return buf;
}
} // namespace

void set_level(Level level) { g_level = level; }
bool set_level(std::string_view name) {
    if (name == "trace") g_level = Level::Trace;
    else if (name == "debug") g_level = Level::Debug;
    else if (name == "info") g_level = Level::Info;
    else if (name == "warn") g_level = Level::Warn;
    else if (name == "error") g_level = Level::Error;
    else return false;
    return true;
}
void set_json(bool json) { g_json = json; }
Level level() { return g_level; }

void write(Level level, std::string_view component, std::string_view message, std::initializer_list<KV> fields) {
    if (level < g_level) return;
    std::string line;
    if (g_json) {
        nlohmann::json j{{"ts", timestamp()}, {"level", name_of(level)}, {"component", component}, {"message", message}};
        for (const auto& [k, v] : fields) j[std::string(k)] = v;
        line = j.dump();
    } else {
        line = timestamp();
        line += ' ';
        line += name_of(level);
        line += ' ';
        line += component;
        line += ' ';
        line += message;
        for (const auto& [k, v] : fields) {
            line += ' ';
            line += k;
            line += '=';
            if (v.find(' ') != std::string::npos || v.empty()) {
                line += '"';
                line += v;
                line += '"';
            } else line += v;
        }
    }
    line += '\n';
    std::lock_guard<std::mutex> lock(g_mutex);
    std::fputs(line.c_str(), stderr);
    std::fflush(stderr);
}

std::string short_id(std::string_view id) { return std::string(id.substr(0, 8)); }

} // namespace fjarr::log
