#pragma once

#include <cstddef>
#include <filesystem>
#include <span>
#include <string_view>
#include <vector>

#include "UnrealKit/Core/Error.h"

namespace UnrealKit::Transport {

struct ImporterFile {
    std::filesystem::path path;
    std::span<const std::byte> bytes;
};

//! Replaces `Plugins/<name>` of the project with the files. Unreal enables a project plugin by default, so the .uproject stays unchanged.
std::expected<std::filesystem::path, Core::Error> installUnrealPlugin(const std::filesystem::path& project, std::string_view name,
                                                                      std::span<const ImporterFile> files);

//! Replaces `extensions/user_default/<id>` with the files in each Blender version from 4.2 that has a config folder.
std::expected<std::vector<std::filesystem::path>, Core::Error> installBlenderExtension(std::string_view id, std::span<const ImporterFile> files);

}
