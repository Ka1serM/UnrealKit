#include "UnrealKit/Platform/FileSource.h"

ue5parse::Result<std::unique_ptr<ue5parse::FileSource>> UnrealKit::Platform::openFileSource(const std::filesystem::path& path) {
    return ue5parse::openLocalFile(path);
}
