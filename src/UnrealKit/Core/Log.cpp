#include "UnrealKit/Core/Log.h"

#include <cstdio>

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
    //! std::println throws when stderr has no handle, which is the case in a Windows GUI app without a console.
    std::fprintf(stderr, "%s: %.*s\n", level == LogLevel::Debug ? "DEBUG" : "WARNING", static_cast<int>(line.size()), line.data());
}

}
