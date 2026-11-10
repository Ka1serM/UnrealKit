#pragma once

#include <string>
#include <vector>

#include "UnrealKit/Core/Error.h"

namespace ue5parse::runtime {
class UMaterialInterface;
}
namespace ue = ue5parse::runtime;

namespace UnrealKit::Export {

struct ResolvedMaterial {
    std::string name;
    std::vector<std::string> chain;
    std::string blendMode;
    bool twoSided = false;
    bool unlit = false;
    int sortPriority = 0;
    bool disableDepthTest = false;
    bool objectTranslucency = false;
};

std::expected<ResolvedMaterial, Core::Error> resolveMaterial(const ue::UMaterialInterface& material);

}
