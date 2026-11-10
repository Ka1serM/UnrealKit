#pragma once

#include <expected>
#include <filesystem>
#include <string_view>

#include "UnrealKit/Core/Error.h"

namespace UnrealKit::Platform {

inline const std::filesystem::path browserStorageDirectory = "/storage";
inline const std::filesystem::path browserMountDirectory = "/mnt";

//! Call it from a job: the folder's worker waits for the main thread in Safari.
std::expected<std::filesystem::path, Core::Error> openBrowserCacheDirectory();

void keepBrowserFile(const std::filesystem::path& file, std::string_view contents);

}
