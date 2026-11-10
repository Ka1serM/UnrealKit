#include "UnrealKit/Export/ResolvedMaterial.h"

#include <array>
#include <format>

#include "UnrealKit/Assets/Reading.h"
#include "Engine/Source/Runtime/Engine/Materials/Material.h"
#include "Engine/Source/Runtime/Engine/Materials/MaterialInstance.h"
#include "Engine/Source/Runtime/Engine/Materials/MaterialInstanceDynamic.h"

namespace UnrealKit::Export {
namespace {

using Assets::Reading::pathOf;

constexpr std::size_t maxMaterialDepth = 16;
//! ATDM_ObjectTranslucency of the game's TranslucencyDepthMode on UMaterial.
constexpr int objectTranslucencyDepthMode = 1;

std::string blendModeName(const ue::EBlendMode value) {
    static constexpr std::array<std::string_view, 8> names{
        "BLEND_Opaque",   "BLEND_Masked",         "BLEND_Translucent",  "BLEND_Additive",
        "BLEND_Modulate", "BLEND_AlphaComposite", "BLEND_AlphaHoldout", "BLEND_TranslucentColoredTransmittance"};
    const auto index = static_cast<std::size_t>(value);
    return index < names.size() ? std::string(names[index]) : std::string();
}

}

std::expected<ResolvedMaterial, Core::Error> resolveMaterial(const ue::UMaterialInterface& material) {
    ResolvedMaterial result;
    result.chain.push_back(pathOf(&material));
    const ue::UMaterialInterface* root = &material;
    const ue::UMaterialInterface* named = &material;
    for (const auto* instance = ue::Cast<ue::UMaterialInstance>(&material); instance && instance->Parent;
         instance = ue::Cast<ue::UMaterialInstance>(instance->Parent)) {
        root = instance->Parent;
        if (ue::Cast<ue::UMaterialInstanceDynamic>(named) != nullptr)
            named = instance->Parent;
        if (result.chain.size() == maxMaterialDepth)
            return Core::fail("material.chain", std::format("The parent chain of '{}' is longer than {}.", result.chain.front(), maxMaterialDepth));
        result.chain.push_back(pathOf(instance->Parent));
    }

    result.name = Assets::Reading::toString(named->GetName());
    result.blendMode = blendModeName(material.GetBlendMode().GetValue());
    result.twoSided = material.IsTwoSided();
    result.unlit = material.GetShadingModel() == ue::MSM_Unlit;
    result.sortPriority = Assets::Reading::value<int>(*root, "SortPriorityOffset", 0);
    result.disableDepthTest = Assets::Reading::value<bool>(*root, "bDisableDepthTest", false);
    result.objectTranslucency = Assets::Reading::value<int>(*root, "TranslucencyDepthMode", 0) == objectTranslucencyDepthMode;
    return result;
}

}
