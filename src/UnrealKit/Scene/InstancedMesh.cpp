#include "UnrealKit/Scene/InstancedMesh.h"

#include <utility>

#include "Engine/Source/Runtime/Engine/Components/InstancedStaticMeshComponent.h"

namespace UnrealKit::Scene {

std::optional<SceneInstances> instancesOf(const ue::USceneComponent& component, std::string key, std::string mesh) {
    const auto* instanced = ue::Cast<ue::UInstancedStaticMeshComponent>(&component);
    if (instanced == nullptr)
        return std::nullopt;
    const auto count = static_cast<std::size_t>(instanced->GetInstanceCount());
    SceneInstances result{.key = std::move(key), .mesh = std::move(mesh)};
    result.locations.reserve(count * 3);
    result.rotations.reserve(count * 4);
    result.scales.reserve(count * 3);
    ue::FTransform transform;
    for (std::size_t i = 0; i < count; ++i) {
        instanced->GetInstanceTransform(static_cast<ue::int32>(i), transform);
        const ue::FVector location = transform.GetLocation();
        const ue::FQuat rotation = transform.GetRotation();
        const ue::FVector scale = transform.GetScale3D();
        result.locations.insert(result.locations.end(), {static_cast<float>(location.X), static_cast<float>(location.Y), static_cast<float>(location.Z)});
        result.rotations.insert(result.rotations.end(), {static_cast<float>(rotation.X), static_cast<float>(rotation.Y), static_cast<float>(rotation.Z),
                                                         static_cast<float>(rotation.W)});
        result.scales.insert(result.scales.end(), {static_cast<float>(scale.X), static_cast<float>(scale.Y), static_cast<float>(scale.Z)});
    }
    return result;
}

}
