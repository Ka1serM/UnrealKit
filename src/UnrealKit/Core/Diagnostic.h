#pragma once

#include <string>

namespace UnrealKit::Core {

enum class Severity { Info, Warning, Error };

struct Diagnostic {
    Severity severity = Severity::Warning;
    std::string code;
    std::string message;
    std::string subject;
};

}
