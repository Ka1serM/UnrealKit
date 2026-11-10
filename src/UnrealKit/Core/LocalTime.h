#pragma once

#include <chrono>
#include <ctime>
#include <string>

namespace UnrealKit::Core {

//! The libc++ of the web build has no std::chrono time zones.
inline std::string localTime(const std::chrono::system_clock::time_point time, const char* format) {
    const std::time_t seconds = std::chrono::system_clock::to_time_t(time);
    std::tm local{};
#if PLATFORM_WINDOWS
    localtime_s(&local, &seconds);
#else
    localtime_r(&seconds, &local);
#endif
    char text[64];
    return std::string(text, std::strftime(text, sizeof(text), format, &local));
}

}
