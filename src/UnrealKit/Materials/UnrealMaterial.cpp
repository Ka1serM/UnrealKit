#include "UnrealKit/Materials/UnrealMaterial.h"

#include "Conversion/Materials/MaterialTranslator.h"
#include "UnrealKit/Materials/DebugView.h"

namespace UnrealKit::Materials {

std::shared_ptr<const CompiledShaderCache::Shaders> CompiledShaderCache::shaders(const MaterialData& data) {
    Entry* entry = nullptr;
    {
        const std::lock_guard lock(mutex);
        entry = &entries[{std::string(data.owner), data.vertexFactory}];
    }
    std::call_once(entry->compiled, [&] {
        std::vector<CompiledShader> compiled;
        for (const ShaderData& shader : data.shaders) {
            std::expected<CompiledShader, Core::Error> result = compileShader(data, shader, spirv);
            if (!result) {
                entry->shaders = std::make_shared<const Shaders>(std::unexpected(std::move(result.error())));
                return;
            }
            compiled.push_back(std::move(*result));
        }
        entry->shaders = std::make_shared<const Shaders>(std::move(compiled));
    });
    return entry->shaders;
}

std::expected<Translation, Core::Error> translateUnrealMaterial(const MaterialData& data, CompiledShaderCache& shaders) {
    const std::shared_ptr<const CompiledShaderCache::Shaders> compiled = shaders.shaders(data);
    if (!*compiled)
        return std::unexpected((*compiled).error());
    auto translated = ue5parse::conversion::translateMaterial(data, **compiled, debugViewSurface);
    if (!translated)
        return Core::fail("material.translation", std::move(translated.error().message));
    return Translation{std::move(translated->program), std::move(translated->warnings)};
}

}
