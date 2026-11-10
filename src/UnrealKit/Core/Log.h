#pragma once

#include <format>
#include <string_view>
#include <utility>

namespace UnrealKit::Core {

enum class LogLevel { Debug, Warning };
using LogSink = void (*)(LogLevel level, std::string_view line);

//! The app sets the sink before the first job starts; without one, the lines go to stderr.
void setLogSink(LogSink sink);
void writeLog(LogLevel level, std::string_view line);

template <class... Args>
void logDebug([[maybe_unused]] std::format_string<Args...> format, [[maybe_unused]] Args&&... args) {
#if UNREALKIT_DEBUG_LOG
    writeLog(LogLevel::Debug, std::format(format, std::forward<Args>(args)...));
#endif
}

template <class... Args>
void logWarning(std::format_string<Args...> format, Args&&... args) {
    writeLog(LogLevel::Warning, std::format(format, std::forward<Args>(args)...));
}

}
