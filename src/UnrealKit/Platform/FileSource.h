#pragma once

#include <filesystem>
#include <memory>

#include "CppUE5Parse/Core/Result.h"
#include "CppUE5Parse/IO/FileSource.h"

namespace UnrealKit::Platform {

ue5parse::Result<std::unique_ptr<ue5parse::FileSource>> openFileSource(const std::filesystem::path& path);

}
