#include "UnrealKit/Assets/ShaderMaterials.h"

#include <algorithm>
#include <array>
#include <deque>
#include <format>
#include <ranges>
#include <span>
#include <tuple>
#include <unordered_set>
#include <utility>

#include "Conversion/Materials/MaterialShaderExport.h"
#include "Engine/Source/Runtime/Engine/Engine/Texture.h"
#include "Engine/Source/Runtime/Engine/Materials/MaterialInterface.h"
#include "Engine/Source/Runtime/Engine/Materials/MaterialParameterCollection.h"
#include "UnrealKit/Assets/Reading.h"
#include "UnrealKit/Materials/DebugView.h"

namespace UnrealKit::Assets {
namespace {

using Reading::pathOf;
using Reading::toString;

constexpr std::array<std::string_view, 3> meshVertexFactories{"FLocalVertexFactory", "TGPUSkinVertexFactoryDefault", "FInstancedStaticMeshVertexFactory"};
constexpr std::array<std::string_view, 1> spriteVertexFactories{"FParticleSpriteVertexFactory"};
constexpr std::array<std::string_view, 1> meshParticleVertexFactories{"FMeshParticleVertexFactory"};

std::span<const std::string_view> vertexFactories(const Materials::VertexFactory use) {
    switch (use) {
    case Materials::VertexFactory::Mesh:
        return meshVertexFactories;
    case Materials::VertexFactory::ParticleSprite:
        return spriteVertexFactories;
    case Materials::VertexFactory::ParticleMesh:
        return meshParticleVertexFactories;
    }
    std::unreachable();
}
constexpr std::array<std::string_view, 2> platforms = {"PCD3D_SM6", "PCD3D_SM5"};
constexpr std::array<ue::EMaterialQualityLevel, 5> qualities = {ue::EMaterialQualityLevel::Num, ue::EMaterialQualityLevel::Epic,
                                                                ue::EMaterialQualityLevel::High, ue::EMaterialQualityLevel::Medium,
                                                                ue::EMaterialQualityLevel::Low};

std::unexpected<Core::Error> unsupported(const std::string_view path, const std::string_view reason) {
    return Core::fail("material.shader", std::format("'{}' {}.", path, reason));
}

template <class Range, class T>
std::size_t rank(const Range& range, const T& value) {
    return static_cast<std::size_t>(std::ranges::find(range, value) - std::ranges::begin(range));
}

int shaderPreference(const std::string_view name) {
    if (name == "TBasePassPSFNoLightMapPolicy")
        return 0;
    if (name.starts_with("TBasePassPSF"))
        return 1;
    if (!name.contains("BasePassPSF"))
        return 4;
    return name.contains("FallbackPolicy") || name.contains("NoLightMapPolicy") ? 2 : 3;
}

struct Candidate {
    const ue5parse::conversion::MaterialShaderInfo* info;
    std::tuple<std::size_t, int, std::size_t, std::size_t> order;
};

Materials::ParameterCollection parameterCollection(const ue::UMaterialParameterCollection& collection, std::deque<std::string>& strings) {
    Materials::ParameterCollection layout;
    const std::string name = toString(collection.GetName());
    const auto keep = [&](const ue::FName& parameter) -> std::string_view {
        return strings.emplace_back(Materials::collectionParameterName(name, toString(parameter)));
    };
    for (const ue::FCollectionScalarParameter& parameter : collection.ScalarParameters)
        layout.scalars.push_back({keep(parameter.ParameterName), false, {parameter.DefaultValue}});
    for (const ue::FCollectionVectorParameter& parameter : collection.VectorParameters) {
        const ue::FLinearColor& value = parameter.DefaultValue;
        layout.vectors.push_back({keep(parameter.ParameterName), true, {value.R, value.G, value.B, value.A}});
    }
    return layout;
}

Materials::BlendMode blendMode(const ue::EBlendMode mode) {
    if (mode > ue::BLEND_AlphaHoldout)
        return Materials::BlendMode::Translucent;
    return static_cast<Materials::BlendMode>(mode);
}

}

std::expected<void, Core::Error> ShaderMaterials::openLibraries() {
    std::call_once(opened, [&] {
        if (auto result = library.OpenLibraries(); !result)
            openFailure = Reading::fail("material.shader_library", "shader libraries", result.error()).error();
    });
    if (openFailure)
        return std::unexpected(*openFailure);
    return {};
}

std::expected<MaterialTranslation, Core::Error> ShaderMaterials::translate(const ue::UMaterialInterface& material, const Materials::VertexFactory use,
                                                                           const std::stop_token stop) {
    if (auto opened = openLibraries(); !opened)
        return std::unexpected(opened.error());
    const std::string materialPath = Reading::pathOf(&material);
    const auto maps = ue5parse::conversion::loadMaterialShaderMaps(material);
    if (!maps)
        return Reading::fail("material.shader", materialPath, maps.error());
    if (!*maps)
        return unsupported(materialPath, "has no cooked shader map");
    const std::vector<ue5parse::conversion::MaterialShaderInfo> compiled = ue5parse::conversion::getMaterialShaders(**maps, library);

    std::vector<Candidate> candidates;
    const std::span<const std::string_view> factories = vertexFactories(use);
    for (const ue5parse::conversion::MaterialShaderInfo& info : compiled) {
        const std::string platform = toString(info.shaderMap->ShaderPlatformName);
        if (!std::ranges::contains(platforms, platform))
            continue;
        const std::size_t factory = rank(factories | std::views::transform([](const std::string_view name) { return ue::FHashedName(name); }),
                                         info.meshShaderMap->VertexFactoryTypeName);
        if (use != Materials::VertexFactory::Mesh && factory == factories.size())
            continue;
        const bool pixel = info.shader->GetFrequency() == ue::SF_Pixel;
        if (!pixel && !info.typeName.contains("PSF"))
            continue;
        const int preference = shaderPreference(info.typeName) + (shaderPreference(info.typeName) == 4 && !pixel);
        candidates.push_back({&info, {factory, preference, rank(platforms, platform), rank(qualities, info.shaderMap->ShaderMapId.QualityLevel)}});
    }
    if (candidates.empty())
        return unsupported(materialPath, use == Materials::VertexFactory::Mesh
                                             ? "has no pixel shader in a shared PC D3D shader map"
                                             : std::format("has no {} shaders in a shared PC D3D shader map", factories.front()));
    const ue5parse::conversion::MaterialShaderInfo& selected = *std::ranges::min(candidates, {}, &Candidate::order).info;
    const ue::FMaterialShaderMap& map = *selected.shaderMap;
    const ue::FHashedName debugViewType(debugViewShader);
    const auto debugView = std::ranges::find_if(compiled, [&](const ue5parse::conversion::MaterialShaderInfo& info) {
        return info.meshShaderMap == selected.meshShaderMap && map.PointerTable.GetShaderType(*info.shader) == debugViewType;
    });
    if (debugView == compiled.end())
        return unsupported(materialPath, std::format("has no {} shader", debugViewShader));
    std::string vertexName = selected.typeName;
    if (const std::size_t at = vertexName.find("PSF"); at != std::string::npos)
        vertexName.replace(at, 3, "VSF");
    const auto vertex = std::ranges::find_if(compiled, [&](const ue5parse::conversion::MaterialShaderInfo& info) {
        return info.meshShaderMap == selected.meshShaderMap && info.shader->GetFrequency() == ue::SF_Vertex && !info.typeName.empty()
               && vertexName.starts_with(info.typeName);
    });
    if (vertex == compiled.end())
        return unsupported(materialPath, std::format("has no vertex shader for {}", selected.typeName));
    if (stop.stop_requested())
        return Core::cancelled();

    const std::array<const ue5parse::conversion::MaterialShaderInfo*, 3> selectedShaders{&selected, &*vertex, &*debugView};
    static constexpr std::array shaderStages{Materials::ShaderStage::Pixel, Materials::ShaderStage::Vertex, Materials::ShaderStage::Pixel};
    const auto exported = ue5parse::conversion::exportMaterialShaders(material, **maps, selectedShaders, library);
    if (!exported)
        return Reading::fail("material.shader", materialPath, exported.error());

    std::deque<std::string> strings;
    const auto keep = [&](std::string text) -> std::string_view { return strings.emplace_back(std::move(text)); };
    std::vector<Materials::ShaderData> shaderData;
    shaderData.reserve(exported->shaders.size());
    for (std::size_t i = 0; i < exported->shaders.size(); ++i) {
        const ue5parse::conversion::MaterialShaderCode& code = exported->shaders[i];
        shaderData.push_back({code.typeName, shaderStages[i], {reinterpret_cast<const uint8_t*>(code.code.data()), code.code.size()}});
    }

    const ue::FUniformExpressionSet& expressions = map.Content.MaterialCompilationOutput.UniformExpressionSet;
    std::vector<Materials::NumericParameter> parameters;
    parameters.reserve(exported->numericParameters.size());
    for (const ue5parse::conversion::MaterialNumericParameterValue& parameter : exported->numericParameters)
        parameters.push_back({parameter.name,
                              parameter.type == ue::EMaterialParameterType::Vector,
                              {parameter.value.R, parameter.value.G, parameter.value.B, parameter.value.A}});

    std::vector<Materials::PreshaderProgram> preshaders;
    preshaders.reserve(static_cast<std::size_t>(expressions.UniformPreshaders.Num()));
    for (const ue::FMaterialUniformPreshaderHeader& header : expressions.UniformPreshaders)
        preshaders.push_back({header.OpcodeOffset, header.OpcodeSize, header.FieldIndex, header.NumFields});
    std::vector<Materials::PreshaderField> fields;
    fields.reserve(static_cast<std::size_t>(expressions.UniformPreshaderFields.Num()));
    for (const ue::FMaterialUniformPreshaderField& field : expressions.UniformPreshaderFields)
        fields.push_back({field.BufferOffset, field.ComponentIndex, field.Type});
    std::vector<std::string_view> preshaderNames;
    preshaderNames.reserve(static_cast<std::size_t>(expressions.UniformPreshaderData.Names.Num()));
    for (const ue::FName& name : expressions.UniformPreshaderData.Names)
        preshaderNames.push_back(keep(name.toString()));

    MaterialTranslation translation;
    std::unordered_set<std::string_view> sampled;
    std::vector<Materials::TextureSlot> textures;
    textures.reserve(exported->textureSlots.size());
    for (const ue5parse::conversion::MaterialTextureSlot& slot : exported->textureSlots) {
        const std::string_view path = slot.texture != nullptr ? keep(pathOf(slot.texture)) : std::string_view();
        if (!path.empty() && Materials::programSamples(slot.type) && sampled.insert(path).second)
            translation.textures.emplace_back(path);
        textures.push_back({slot.parameterName, path, slot.type, slot.index, slot.addressX, slot.addressY, slot.nearest, slot.size});
    }

    std::vector<Materials::TextureExtent> textureSizes;
    textureSizes.reserve(exported->textureExtents.size());
    for (const ue5parse::conversion::MaterialTextureExtent& extent : exported->textureExtents)
        textureSizes.push_back({extent.parameterName, extent.referencedTextureIndex, extent.sizeX, extent.sizeY, extent.numSlices});

    std::vector<Materials::ParameterCollection> collections;
    collections.reserve(exported->parameterCollections.size());
    for (const ue::UMaterialParameterCollection* collection : exported->parameterCollections)
        collections.push_back(collection != nullptr ? parameterCollection(*collection, strings) : Materials::ParameterCollection{});
    const std::string ownerPath = pathOf((*maps)->owner);
    const Materials::MaterialData data{
        .owner = ownerPath,
        .blendMode = blendMode(material.GetBlendMode().GetValue()),
        .game = game,
        .shaders = shaderData,
        .parameters = parameters,
        .preshaders = preshaders,
        .fields = fields,
        .preshaderCode = {expressions.UniformPreshaderData.Data.GetData(), static_cast<std::size_t>(expressions.UniformPreshaderData.Data.Num())},
        .preshaderNames = preshaderNames,
        .uniformExpressions = &expressions,
        .textures = textures,
        .textureSizes = textureSizes,
        .parameterCollections = collections,
        .vertexFactory = use,
    };
    auto translated = Materials::translateUnrealMaterial(data, shaders);
    if (!translated)
        return std::unexpected(Core::Error{translated.error().code, std::format("'{}': {}", materialPath, translated.error().message)});
    translation.program = std::move(translated->program);
    translation.warnings = std::move(translated->warnings);
    return translation;
}

}
