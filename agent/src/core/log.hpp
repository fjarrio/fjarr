#pragma once
// Structured logging: one line per event on stderr, text or JSON.
// spec: docs/23-agent-core-architecture.md#observability
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>

namespace fjarr::log {

enum class Level { Trace, Debug, Info, Warn, Error };

void set_level(Level level);
bool set_level(std::string_view name); // trace|debug|info|warn|error
void set_json(bool json);
Level level();

using KV = std::pair<std::string_view, std::string>;

void write(Level level, std::string_view component, std::string_view message,
           std::initializer_list<KV> fields = {});

inline void trace(std::string_view c, std::string_view m, std::initializer_list<KV> f = {}) { write(Level::Trace, c, m, f); }
inline void debug(std::string_view c, std::string_view m, std::initializer_list<KV> f = {}) { write(Level::Debug, c, m, f); }
inline void info(std::string_view c, std::string_view m, std::initializer_list<KV> f = {}) { write(Level::Info, c, m, f); }
inline void warn(std::string_view c, std::string_view m, std::initializer_list<KV> f = {}) { write(Level::Warn, c, m, f); }
inline void error(std::string_view c, std::string_view m, std::initializer_list<KV> f = {}) { write(Level::Error, c, m, f); }

/// A short id for log lines (first 8 chars of a session id).
std::string short_id(std::string_view id);

} // namespace fjarr::log
