#pragma once

#include <fmt/format.h>

#include <string_view>

namespace rfu::log {

enum class Level { Trace, Debug, Info, Warn, Error };

void set_level(Level level);
bool enabled(Level level);
void write(Level level, std::string_view text);

template <typename... Args>
void trace(fmt::format_string<Args...> f, Args&&... args) {
    if (enabled(Level::Trace)) write(Level::Trace, fmt::format(f, std::forward<Args>(args)...));
}
template <typename... Args>
void debug(fmt::format_string<Args...> f, Args&&... args) {
    if (enabled(Level::Debug)) write(Level::Debug, fmt::format(f, std::forward<Args>(args)...));
}
template <typename... Args>
void info(fmt::format_string<Args...> f, Args&&... args) {
    if (enabled(Level::Info)) write(Level::Info, fmt::format(f, std::forward<Args>(args)...));
}
template <typename... Args>
void warn(fmt::format_string<Args...> f, Args&&... args) {
    if (enabled(Level::Warn)) write(Level::Warn, fmt::format(f, std::forward<Args>(args)...));
}
template <typename... Args>
void error(fmt::format_string<Args...> f, Args&&... args) {
    if (enabled(Level::Error)) write(Level::Error, fmt::format(f, std::forward<Args>(args)...));
}

}  // namespace rfu::log
