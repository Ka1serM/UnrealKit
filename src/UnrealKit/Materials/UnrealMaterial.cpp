#include "UnrealKit/Materials/UnrealMaterial.h"

#include "Conversion/Materials/MaterialTranslator.h"
#include "UnrealKit/Materials/DebugView.h"

namespace UnrealKit::Materials {
namespace {

using scene::stream::MaterialGeometry;
using ue5parse::conversion::GeometryAttribute;

using ue5parse::conversion::ProgramOp;

static_assert(static_cast<uint8_t>(ProgramOp::MulAdd) == static_cast<uint8_t>(scene::stream::MaterialOp::MAX));
static_assert(static_cast<uint8_t>(ProgramOp::Combine) == static_cast<uint8_t>(scene::stream::MaterialOp::Combine));
static_assert(static_cast<uint8_t>(ProgramOp::Select) == static_cast<uint8_t>(scene::stream::MaterialOp::Select));
static_assert(static_cast<uint32_t>(MaterialGeometry::VertexColor) == static_cast<uint32_t>(GeometryAttribute::VertexColor));
static_assert(static_cast<uint32_t>(MaterialGeometry::SceneDepth) == static_cast<uint32_t>(GeometryAttribute::SceneDepth));
static_assert(static_cast<uint32_t>(MaterialGeometry::CustomData8) == static_cast<uint32_t>(GeometryAttribute::CustomData8));
static_assert(static_cast<uint32_t>(MaterialGeometry::LocalPosition) == static_cast<uint32_t>(GeometryAttribute::LocalPosition));

scene::stream::TextureAddress sceneAddress(const ue5parse::conversion::TextureAddress address) {
    return static_cast<scene::stream::TextureAddress>(address);
}

MaterialProgram sceneProgram(ue5parse::conversion::MaterialProgram&& translated) {
    MaterialProgram program;
    program.instructions.reserve(translated.instructions.size());
    for (const ue5parse::conversion::ProgramInstruction& instruction : translated.instructions)
        program.instructions.emplace_back(static_cast<scene::stream::MaterialOp>(instruction.op), instruction.width, instruction.index, instruction.a,
                                          instruction.b, instruction.c);
    program.constants = std::move(translated.constants);
    for (ue5parse::conversion::MaterialProgram::Parameter& parameter : translated.parameters)
        program.parameters.push_back({std::move(parameter.name), std::move(parameter.value)});
    for (ue5parse::conversion::MaterialProgram::Texture& texture : translated.textures)
        program.textures.push_back(
            {std::move(texture.image), std::move(texture.name), sceneAddress(texture.addressU), sceneAddress(texture.addressV), texture.nearest});
    program.baseColor = translated.baseColor;
    program.normal = translated.normal;
    program.emission = translated.emission;
    program.roughness = translated.roughness;
    program.metallic = translated.metallic;
    program.specular = translated.specular;
    program.opacity = translated.opacity;
    program.refraction = translated.refraction;
    program.displacement = translated.displacement;
    return program;
}

}

std::shared_ptr<const CompiledShaderCache::Shaders> CompiledShaderCache::shaders(const MaterialData& data) {
    Entry* entry = nullptr;
    {
        const std::lock_guard lock(mutex);
        entry = &entries[{std::string(data.owner), data.vertexFactory}];
    }
    std::call_once(entry->compiled, [&] {
        std::vector<CompiledShader> compiled;
        for (const ShaderData& shader : data.shaders) {
            std::expected<CompiledShader, Core::Error> result = compileShader(data, shader);
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
    return Translation{sceneProgram(std::move(translated->program)), std::move(translated->warnings)};
}

}
