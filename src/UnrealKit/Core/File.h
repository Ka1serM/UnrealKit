#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

#include "UnrealKit/Core/Error.h"

namespace UnrealKit::Core {

std::expected<std::vector<std::byte>, Core::Error> readFile(const std::filesystem::path& file);
std::expected<void, Core::Error> readFileAt(const std::filesystem::path& file, std::uint64_t offset, std::span<std::byte> out);

std::expected<void, Core::Error> replaceFile(const std::filesystem::path& file, const std::filesystem::path& temporary, std::span<const std::byte> data);

}
