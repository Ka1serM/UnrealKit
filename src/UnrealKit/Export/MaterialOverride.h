#pragma once

#include <string>

namespace ue5parse::runtime {
class UMaterialInterface;
}
namespace ue = ue5parse::runtime;

namespace UnrealKit::Export {

struct MaterialOverride {
    std::string path;
    const ue::UMaterialInterface* material = nullptr;

    bool operator==(const MaterialOverride&) const = default;
};

MaterialOverride overrideOf(const ue::UMaterialInterface* material);

}
