#pragma once

#include <expected>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "UnrealKit/Core/Error.h"
#include "UnrealKit/Materials/MaterialData.h"
#include "UnrealKit/Materials/MaterialProgram.h"
#include "UnrealKit/Materials/ShaderCompiler.h"

namespace UnrealKit::Materials {

class CompiledShaderCache {
public:
    using Shaders = std::expected<std::vector<CompiledShader>, Core::Error>;

    std::shared_ptr<const Shaders> shaders(const MaterialData& data);

private:
    struct Entry {
        std::once_flag compiled;
        std::shared_ptr<const Shaders> shaders;
    };

    std::mutex mutex;
    std::map<std::pair<std::string, VertexFactory>, Entry> entries;
};

struct Translation {
    MaterialProgram program;
    std::vector<std::string> warnings;
};

std::expected<Translation, Core::Error> translateUnrealMaterial(const MaterialData& data, CompiledShaderCache& shaders);

}
