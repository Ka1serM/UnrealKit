#pragma once

#include <expected>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "CppUE5Parse/Provider/FileProvider.h"
#include "Engine/Source/Runtime/RenderCore/ShaderCodeArchive.h"
#include "UnrealKit/Core/Error.h"
#include "UnrealKit/Materials/UnrealMaterial.h"

namespace ue5parse::runtime {
class UMaterialInterface;
}
namespace ue = ue5parse::runtime;

namespace UnrealKit::Assets {

struct MaterialTranslation {
    ue5parse::conversion::MaterialProgram program;
    std::vector<std::string> textures;
    std::vector<std::string> warnings;
};

class ShaderMaterials {
public:
    ShaderMaterials(const ue5parse::FileProvider& files, std::string debugViewShader)
        : library(files), game(files.getGame()), debugViewShader(std::move(debugViewShader)) {}

    std::expected<MaterialTranslation, Core::Error> translate(const ue::UMaterialInterface& material, Materials::VertexFactory use, std::stop_token stop);

private:
    std::expected<void, Core::Error> openLibraries();

    ue5parse::runtime::FShaderCodeLibrary library;
    ue5parse::EGame game;
    std::string debugViewShader;
    std::once_flag opened;
    std::optional<Core::Error> openFailure;
    Materials::CompiledShaderCache shaders;
};

}
