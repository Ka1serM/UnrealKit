#pragma once

#include <optional>
#include <string>

#include "Engine/Source/Runtime/Engine/Components/SceneComponent.h"
#include "UnrealKit/Scene/Records.h"

namespace ue = ue5parse::runtime;

namespace UnrealKit::Scene {

std::optional<SceneInstances> instancesOf(const ue::USceneComponent& component, std::string key, std::string mesh);

}
