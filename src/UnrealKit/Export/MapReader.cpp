#include "UnrealKit/Export/MapReader.h"

#include <algorithm>
#include <cmath>
#include <deque>
#include <format>
#include <numbers>
#include <optional>
#include <unordered_map>
#include <unordered_set>

#include "UnrealKit/Assets/Reading.h"
#include "UnrealKit/Core/Log.h"
#include "Engine/Source/Runtime/CoreUObject/UObject/PropertyAccess.h"
#include "Engine/Source/Runtime/CoreUObject/UObject/UObjectHash.h"
#include "Engine/Source/Runtime/CoreUObject/UObject/UnrealType.h"
#include "Engine/Source/Runtime/Engine/Components/DirectionalLightComponent.h"
#include "Engine/Source/Runtime/Engine/Components/RectLightComponent.h"
#include "Engine/Source/Runtime/Engine/Components/SkinnedMeshComponent.h"
#include "Engine/Source/Runtime/Engine/Components/SkyLightComponent.h"
#include "Engine/Source/Runtime/Engine/Components/SpotLightComponent.h"
#include "Engine/Source/Runtime/Engine/Components/StaticMeshComponent.h"
#include "Engine/Source/Runtime/Engine/Engine/Level.h"
#include "Engine/Source/Runtime/Engine/Engine/LevelStreaming.h"
#include "Engine/Source/Runtime/Engine/Engine/TextureCube.h"
#include "Engine/Source/Runtime/Engine/Engine/World.h"
#include "Engine/Source/Runtime/Engine/GameFramework/Actor.h"
#include "Engine/Source/Runtime/Engine/Materials/MaterialInterface.h"
#include "UnrealKit/Scene/InstancedMesh.h"
#include "UnrealKit/Scene/Matrix.h"

namespace UnrealKit::Export {
namespace {

using namespace Assets::Reading;
using ue::FName;
using ue::FTransform;
using ue::UObject;
using ue::USceneComponent;

struct Node {
    std::string path;
    std::string name;
    std::string mesh;
    std::vector<MaterialOverride> materials;
    int parent = -1;
    FTransform transform;
    std::optional<Scene::SceneInstances> instances;
    std::vector<std::uint8_t> colors;
    const ue::ULightComponentBase* light = nullptr;
    bool castShadow = true;
    int sortPriority = 0;
    bool reverseCulling = false;
};

bool isA(const UObject& value, const std::string_view className) {
    for (const ue::UStruct* type = value.GetClass(); type != nullptr; type = type->GetSuperStruct())
        if (type->GetFName() == FName(className))
            return true;
    return false;
}

bool isVisible(const UObject& value, const std::vector<std::string>& hiddenClasses) {
    for (const std::string& hidden : hiddenClasses)
        if (isA(value, hidden))
            return false;
    if (const auto* primitive = ue::Cast<ue::UPrimitiveComponent>(&value);
        primitive != nullptr && (!primitive->bRenderInMainPass || Assets::Reading::value<bool>(*primitive, "bEarlyZPassAggregatedMesh", false)))
        return false;
    if (const auto* actor = ue::Cast<ue::AActor>(&value))
        return !actor->IsHidden() && !actor->IsEditorOnly();
    if (const auto* component = ue::Cast<USceneComponent>(&value))
        return component->IsVisible() && !component->bHiddenInGame && !component->bIsEditorOnly && !component->bRenderInTargetViewMode;
    return true;
}

std::string meshPath(const USceneComponent& component) {
    if (const auto* staticMesh = ue::Cast<ue::UStaticMeshComponent>(&component))
        return pathOf(staticMesh->GetStaticMesh());
    if (const auto* skinned = ue::Cast<ue::USkinnedMeshComponent>(&component))
        return pathOf(skinned->GetSkinnedAsset());
    return {};
}

std::vector<MaterialOverride> materialsOf(const USceneComponent& component) {
    const auto* mesh = ue::Cast<ue::UMeshComponent>(&component);
    if (mesh == nullptr)
        return {};
    std::vector<MaterialOverride> result;
    result.reserve(static_cast<std::size_t>(mesh->OverrideMaterials.Num()));
    for (const ue::UMaterialInterface* material : mesh->OverrideMaterials)
        result.push_back(overrideOf(material));
    return result;
}

std::vector<std::uint8_t> colorsOf(const USceneComponent& component) {
    const auto* staticMesh = ue::Cast<ue::UStaticMeshComponent>(&component);
    if (staticMesh == nullptr || staticMesh->LODData.IsEmpty() || !staticMesh->LODData[0].OverrideVertexColors)
        return {};
    const ue::TArray<ue::FColor>& overrideColors = staticMesh->LODData[0].OverrideVertexColors->GetVertexData();
    std::vector<std::uint8_t> result;
    result.reserve(static_cast<std::size_t>(overrideColors.Num()) * 4);
    for (const ue::FColor& color : overrideColors)
        result.insert(result.end(), {color.R, color.G, color.B, color.A});
    return result;
}

float candelas(const ue::ULightComponent& light) {
    return light.ComputeLightBrightness() / (100.0f * 100.0f);
}

float unitlessCandelas(const ue::UPointLightComponent& light) {
    const float metres = light.AttenuationRadius / 2 / 100.0f;
    return light.ComputeLightBrightness() * std::pow(0.75f, light.LightFalloffExponent) * metres * metres;
}

std::optional<Scene::SceneLight> lightOf(const ue::ULightComponentBase& light, std::string key, std::string name) {
    if (!light.bAffectsWorld)
        return std::nullopt;
    ue::FLinearColor color(light.LightColor);
    if (const auto* colored = ue::Cast<ue::ULightComponent>(&light); colored != nullptr && colored->bUseTemperature)
        color *= ue::FLinearColor::MakeFromColorTemperature(colored->Temperature);
    Scene::SceneLight result{.key = std::move(key), .name = std::move(name), .color = {color.R, color.G, color.B}};
    if (const auto* sky = ue::Cast<ue::USkyLightComponent>(&light)) {
        const ue::FLinearColor lower = sky->LowerHemisphereColor;
        result.type = Scene::LightType::Sky;
        result.intensity = sky->Intensity;
        if (sky->SourceType == ue::SLS_SpecifiedCubemap)
            result.image = pathOf(sky->Cubemap);
        result.sourceCubemapAngle = sky->SourceCubemapAngle;
        result.lowerHemisphereIsBlack = sky->bLowerHemisphereIsBlack;
        result.lowerHemisphereColor = {lower.R, lower.G, lower.B, lower.A};
        result.indirectLightingIntensity = sky->IndirectLightingIntensity;
        return result;
    }
    result.indirectLightingIntensity = light.IndirectLightingIntensity;
    if (const auto* local = ue::Cast<ue::ULocalLightComponent>(&light))
        result.attenuationRadius = local->AttenuationRadius;
    if (const auto* point = ue::Cast<ue::UPointLightComponent>(&light)) {
        result.inverseSquaredFalloff = point->bUseInverseSquaredFalloff;
        result.lightFalloffExponent = point->LightFalloffExponent;
    }
    if (const auto* spot = ue::Cast<ue::USpotLightComponent>(&light)) {
        constexpr float degrees = 180.0f / std::numbers::pi_v<float>;
        result.type = Scene::LightType::Spot;
        result.intensity = spot->bUseInverseSquaredFalloff ? candelas(*spot) : unitlessCandelas(*spot);
        result.sourceRadius = spot->SourceRadius;
        result.innerConeAngle = std::clamp(spot->InnerConeAngle, 0.0f, 89.0f);
        result.outerConeAngle = spot->GetHalfConeAngle() * degrees;
    } else if (const auto* rect = ue::Cast<ue::URectLightComponent>(&light)) {
        result.type = Scene::LightType::Rect;
        result.intensity = candelas(*rect);
        result.sourceWidth = rect->SourceWidth;
        result.sourceHeight = rect->SourceHeight;
    } else if (const auto* directional = ue::Cast<ue::UDirectionalLightComponent>(&light)) {
        result.type = Scene::LightType::Directional;
        result.intensity = directional->ComputeLightBrightness();
        result.sourceAngle = directional->LightSourceAngle;
    } else if (const auto* point = ue::Cast<ue::UPointLightComponent>(&light)) {
        result.type = Scene::LightType::Point;
        result.intensity = point->bUseInverseSquaredFalloff ? candelas(*point) : unitlessCandelas(*point);
        result.sourceRadius = point->SourceRadius;
    } else {
        return std::nullopt;
    }
    return result;
}

std::vector<ExportMesh> placements(std::vector<Node>& nodes) {
    std::vector<ExportMesh> parts;
    parts.reserve(nodes.size());
    for (Node& node : nodes) {
        ExportMesh& placement = parts.emplace_back();
        placement.name = node.name;
        placement.parent = node.parent;
        placement.matrix = Scene::matrixOf(node.transform);
        placement.castShadow = node.castShadow;
        placement.sortPriority = node.sortPriority;
        placement.reverseCulling = node.reverseCulling;
        if (node.light != nullptr)
            placement.light = lightOf(*node.light, node.path, node.name);
        if (node.instances && node.instances->locations.empty())
            continue;
        placement.meshPath = std::move(node.mesh);
        placement.overrides = std::move(node.materials);
        if (!node.colors.empty()) {
            placement.meshKey = node.path;
            placement.colors = std::move(node.colors);
        }
        placement.instances = std::move(node.instances);
    }
    return parts;
}


}

bool loadsWithWorld(const ue::ULevelStreaming& streaming) {
    if (!isA(streaming, "LevelStreamingDynamic"))
        return true;
    bool initiallyLoaded = false;
    ue::GetPropertyValue(&streaming, FName("bInitiallyLoaded"), initiallyLoaded);
    return initiallyLoaded;
}

struct MapReader::State {
    MapRules rules;
    std::deque<std::string> worlds;
    std::unordered_set<std::string> seenWorlds;
    std::unordered_set<std::string> seenNodes;
    std::vector<ue::AActor*> actors;
    std::size_t actorIndex = 0;
    std::string levelName;
    int worldsLoaded = 0;

    void enqueue(const std::string& path) {
        if (seenWorlds.insert(lowercase(path)).second)
            worlds.push_back(path);
    }

    float getProgress() const {
        const float pending = actors.empty() ? 0.0f : static_cast<float>(actors.size() - actorIndex) / static_cast<float>(actors.size());
        return (static_cast<float>(worldsLoaded) - pending) / static_cast<float>(worldsLoaded + static_cast<int>(worlds.size()));
    }

    std::expected<void, Core::Error> enqueueStreamedLevels(const ue::UWorld& world, const std::stop_token stop) {
        for (const ue::ULevelStreaming* streaming : world.GetStreamingLevels())
            if (streaming != nullptr && !streaming->GetWorldAsset().IsNull() && loadsWithWorld(*streaming))
                enqueue(worldPath(toString(streaming->GetWorldAsset().ToString())));
        if (!rules.sublevels)
            return {};
        const auto levels = rules.sublevels(world, stop);
        if (!levels)
            return std::unexpected(levels.error());
        for (const std::string& level : *levels)
            enqueue(level);
        return {};
    }

    std::expected<void, Core::Error> loadWorld(const std::string& path, const std::stop_token stop) {
        const auto loaded = loadObject<ue::UWorld>(path, "map.world", stop);
        if (!loaded)
            return std::unexpected(loaded.error());
        const ue::UWorld& world = **loaded;
        if (world.PersistentLevel == nullptr)
            return Core::fail("map.level", std::format("'{}' has no loadable persistent level.", path));
        if (worldsLoaded == 0)
            if (auto streamed = enqueueStreamedLevels(world, stop); !streamed)
                return streamed;
        levelName = toString(world.GetFName());
        ++worldsLoaded;
        actors.assign(world.PersistentLevel->Actors.begin(), world.PersistentLevel->Actors.end());
        actorIndex = 0;
        return {};
    }

    bool addComponent(const USceneComponent& component, const int parent, const bool ancestorsVisible,
                      const std::unordered_map<const USceneComponent*, std::vector<const USceneComponent*>>& children, std::vector<Node>& nodes,
                      const std::stop_token stop) {
        if (stop.stop_requested())
            return false;
        const std::size_t index = nodes.size();
        std::string path = pathOf(&component);
        if (!seenNodes.insert(lowercase(path)).second)
            return false;
        const bool visible = ancestorsVisible && isVisible(component, rules.hiddenClasses);
        Node node{.path = path, .name = toString(component.GetFName()), .parent = parent, .transform = component.GetRelativeTransform()};
        if (visible) {
            node.mesh = meshPath(component);
            node.materials = materialsOf(component);
            node.colors = colorsOf(component);
            node.instances = Scene::instancesOf(component, path + ":instances", node.colors.empty() ? node.mesh : path);
            node.light = ue::Cast<ue::ULightComponentBase>(&component);
            if (const auto* primitive = ue::Cast<ue::UPrimitiveComponent>(&component)) {
                node.castShadow = primitive->CastShadow;
                node.sortPriority = Assets::Reading::value<int>(*primitive, "TranslucencySortPriority", 0);
                node.reverseCulling = Assets::Reading::value<bool>(*primitive, "bReverseCulling", false);
            }
        }
        nodes.push_back(std::move(node));
        int kept = 0;
        if (const auto found = children.find(&component); found != children.end())
            for (const USceneComponent* child : found->second)
                if (addComponent(*child, static_cast<int>(index), visible, children, nodes, stop))
                    ++kept;
        if (!nodes[index].mesh.empty() || nodes[index].light != nullptr || kept > 1)
            return true;
        if (kept == 1) {
            Node& only = nodes[index + 1];
            only.parent = parent;
            only.transform = only.transform * nodes[index].transform;
            for (std::size_t i = index + 2; i < nodes.size(); ++i)
                if (nodes[i].parent > static_cast<int>(index))
                    --nodes[i].parent;
        }
        nodes.erase(nodes.begin() + static_cast<std::ptrdiff_t>(index));
        return kept == 1;
    }
};

MapReader::MapReader(MapRules rules) : state(std::make_unique<State>()) {
    state->rules = std::move(rules);
}

void MapReader::open(const std::string_view path) {
    state->enqueue(worldPath(path));
}

MapReader::~MapReader() = default;
MapReader::MapReader(MapReader&&) noexcept = default;
MapReader& MapReader::operator=(MapReader&&) noexcept = default;

std::expected<std::optional<MapActor>, Core::Error> MapReader::next(const std::stop_token stop) {
    State& map = *state;
    while (true) {
        if (stop.stop_requested())
            return Core::cancelled();
        if (map.actorIndex == map.actors.size()) {
            if (map.worlds.empty())
                return std::nullopt;
            const std::string world = std::move(map.worlds.front());
            map.worlds.pop_front();
            const auto loaded = map.loadWorld(world, stop);
            if (!loaded)
                return std::unexpected(loaded.error());
            continue;
        }
        const ue::AActor* actor = map.actors[map.actorIndex++];
        if (actor == nullptr || !isVisible(*actor, map.rules.hiddenClasses))
            continue;
        const USceneComponent* root = actor->GetRootComponent();
        if (root == nullptr)
            continue;
        ue::TArray<UObject*> subobjects;
        ue::GetObjectsWithOuter(actor, subobjects, false);
        std::unordered_map<const USceneComponent*, std::vector<const USceneComponent*>> children;
        bool usable = false;
        for (const UObject* subobject : subobjects) {
            const auto* component = ue::Cast<USceneComponent>(subobject);
            if (component == nullptr)
                continue;
            usable |=
                ue::Cast<ue::UStaticMeshComponent>(component) || ue::Cast<ue::USkinnedMeshComponent>(component) || ue::Cast<ue::ULightComponentBase>(component);
            if (const USceneComponent* parent = component->GetAttachParent())
                children[parent].push_back(component);
        }
        if (!usable || !map.seenNodes.insert(lowercase(pathOf(actor))).second)
            continue;
        std::vector<Node> nodes;
        map.addComponent(*root, -1, true, children, nodes, stop);
        if (nodes.empty() || stop.stop_requested())
            continue;
        nodes[0].name = toString(actor->GetFName());
        return MapActor{.category = map.levelName, .parts = placements(nodes), .progress = map.getProgress()};
    }
}

std::string worldPath(const std::string_view path) {
    const std::string_view clean = path.substr(0, path.find(':'));
    if (clean.find('.') != std::string_view::npos)
        return std::string(clean);
    const std::size_t slash = clean.rfind('/');
    return slash == std::string_view::npos ? std::string(clean) : std::format("{}.{}", clean, clean.substr(slash + 1));
}

}
