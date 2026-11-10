#include "UnrealKit/Core/Log.h"

#include <cstdio>
#include <print>

namespace UnrealKit::Core {
namespace {

LogSink logSink = nullptr;

}

void setLogSink(const LogSink sink) {
    logSink = sink;
}

void writeLog(const LogLevel level, const std::string_view line) {
    if (logSink != nullptr)
        return logSink(level, line);
    std::println(stderr, "{}: {}", level == LogLevel::Debug ? "DEBUG" : "WARNING", line);
}

}
