#pragma once

#include <filesystem>
#include <string>
#include <string_view>

namespace UnrealKit::Core {

inline std::string utf8(const std::filesystem::path& path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

inline std::filesystem::path pathFromUtf8(const char* text) {
    return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(text)));
}

}
