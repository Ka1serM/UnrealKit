#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <stop_token>
#include <vector>

#include "CppUE5Parse/IO/FileSource.h"
#include "UnrealKit/Core/Error.h"

namespace UnrealKit::Assets {

//! The Zstandard-compressed .usmap of the game executable.
std::expected<std::vector<std::byte>, Core::Error> generateMappings(const ue5parse::FileSource& executable, std::stop_token stop);
std::expected<std::vector<std::byte>, Core::Error> generateMappings(std::span<const char> executable, std::stop_token stop);

//! The file when it exists. Otherwise, it writes the result of generate to it and removes the other .usmap files of its folder.
std::expected<std::filesystem::path, Core::Error> cachedMappings(const std::filesystem::path& file, const std::function<std::expected<std::vector<std::byte>, Core::Error>()>& generate);

}
