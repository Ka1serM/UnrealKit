#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

#include "CppUE5Parse/IO/FileSource.h"
#include "CppUE5Parse/Provider/FileProvider.h"
#include "Engine/Source/Runtime/CoreUObject/UObject/UObjectGlobals.h"
#include "UnrealKit/Assets/ShaderMaterials.h"
#include "UnrealKit/Core/Error.h"

namespace UnrealKit::Assets {

struct ArchiveKey {
    std::string guid;
    std::string key;
};

struct MountOptions {
    ue5parse::EGame game = ue5parse::EGame::GAME_UE5_LATEST;
    std::vector<std::unique_ptr<ue5parse::FileSource>> archives;
    std::filesystem::path mappings;
    //! Empty when the game does not encrypt its archives.
    std::string aesKey;
    std::vector<ArchiveKey> extraKeys;
    std::string debugViewShader;
};

std::expected<std::vector<std::unique_ptr<ue5parse::FileSource>>, Core::Error> openArchiveDirectory(const std::filesystem::path& directory);

class Provider {
public:
    Provider() = default;
    std::expected<void, Core::Error> mount(MountOptions options, std::stop_token stop);
    Provider(const Provider&) = delete;
    Provider& operator=(const Provider&) = delete;

    int getFileCount() const noexcept { return fileCount; }
    const ue5parse::FileProvider& getFiles() const noexcept { return *files; }
    ShaderMaterials& getShaderMaterials() const noexcept { return *shaderMaterials; }

private:
    std::unique_ptr<ue5parse::FileProvider> files;
    std::optional<ue5parse::runtime::FRuntime> runtime;
    std::unique_ptr<ShaderMaterials> shaderMaterials;
    int fileCount = 0;
};

}
