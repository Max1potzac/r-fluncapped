#include "core/log.h"

#include <atomic>
#include <cstdio>
#include <mutex>

namespace rfu::log {

namespace {
std::atomic<Level> g_level{Level::Info};
std::mutex g_mutex;

const char* tag(Level level) {
    switch (level) {
    case Level::Trace: return "trace";
    case Level::Debug: return "debug";
    case Level::Info: return "info";
    case Level::Warn: return "warn";
    case Level::Error: return "error";
    }
    return "?";
}
}  // namespace

void set_level(Level level) { g_level = level; }

bool enabled(Level level) { return level >= g_level.load(std::memory_order_relaxed); }

void write(Level level, std::string_view text) {
    std::lock_guard lock(g_mutex);
    std::fprintf(stderr, "[%s] %.*s\n", tag(level), static_cast<int>(text.size()), text.data());
}

}  // namespace rfu::log
