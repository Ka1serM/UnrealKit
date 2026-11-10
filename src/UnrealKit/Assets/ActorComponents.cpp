#include "UnrealKit/Assets/ActorComponents.h"

#include <algorithm>
#include <format>
#include <string>
#include <unordered_map>

#include "Engine/Source/Runtime/CoreUObject/UObject/UObjectGlobals.h"
#include "Engine/Source/Runtime/CoreUObject/UObject/UnrealType.h"
#include "Engine/Source/Runtime/Engine/Components/SceneComponent.h"
#include "Engine/Source/Runtime/Engine/Engine/BlueprintGeneratedClass.h"
#include "Engine/Source/Runtime/Engine/Engine/InheritableComponentHandler.h"
#include "Engine/Source/Runtime/Engine/Engine/SCS_Node.h"
#include "Engine/Source/Runtime/Engine/Engine/SimpleConstructionScript.h"
#include "Engine/Source/Runtime/Engine/GameFramework/Actor.h"
#include "UnrealKit/Assets/Reading.h"

namespace UnrealKit::Assets {

namespace {

using namespace Reading;

std::string nameOf(const ue::UObject* object) {
    if (object == nullptr)
        return {};
    std::string name = toString(object->GetName());
    const std::string suffix = "_GEN_VARIABLE";
    if (name.ends_with(suffix))
        name.resize(name.size() - suffix.size());
    return name == "None" ? std::string() : name;
}

std::string socketOf(const ue::UObject& component) {
    const auto* scene = ue::Cast<ue::USceneComponent>(&component);
    if (scene == nullptr)
        return {};
    const std::string socket = toString(scene->AttachSocketName);
    return socket == "None" ? std::string() : socket;
}

std::string parentOf(const ue::UObject& component) {
    const auto* scene = ue::Cast<ue::USceneComponent>(&component);
    return scene == nullptr ? std::string() : nameOf(scene->AttachParent);
}

}

std::expected<void, Core::Error> ActorComponents::read(const std::string_view classPath, const std::stop_token stop) {
    ue::UObject* loaded = load(classPath);
    if (loaded == nullptr)
        return Core::fail("object.load", std::format("'{}' does not load.", classPath));
    ue::UClass* actorClass = ue::Cast<ue::UClass>(loaded);
    if (actorClass == nullptr || !actorClass->IsChildOf(ue::AActor::StaticClass()))
        return Core::fail("actor.class", std::format("'{}' is not an actor class.", classPath));
    defaults = actorClass->GetDefaultObject();
    if (defaults == nullptr)
        return Core::fail("actor.default", std::format("The actor class '{}' has no class-default object.", classPath));

    ue::TArray<const ue::UBlueprintGeneratedClass*> hierarchy;
    ue::UBlueprintGeneratedClass::GetGeneratedClassesHierarchy(actorClass, hierarchy);
    ue::UBlueprintGeneratedClass* actual = ue::Cast<ue::UBlueprintGeneratedClass>(actorClass);
    if (actual != nullptr)
        for (const ue::UBlueprintGeneratedClass* declaring : hierarchy) {
            if (stop.stop_requested())
                return Core::cancelled();
            if (const ue::UInheritableComponentHandler* handler = declaring->GetInheritableComponentHandler())
                for (const ue::FComponentOverrideRecord& record : handler->GetRecords())
                    if (record.ComponentTemplate != nullptr) {
                        const std::string variable = toString(record.ComponentKey.SCSVariableName);
                        if (!variable.empty() && variable != "None")
                            overrides.try_emplace(lowercase(variable), record.ComponentTemplate);
                    }
        }
    addNativeComponents();
    for (int index = hierarchy.Num() - 1; index >= 0; --index) {
        if (stop.stop_requested())
            return Core::cancelled();
        if (hierarchy[index] != nullptr)
            addConstructionNodes(*const_cast<ue::UBlueprintGeneratedClass*>(hierarchy[index]));
    }
    return {};
}

ue::UObject* ActorComponents::load(const std::string_view path) const {
    if (path.empty())
        return nullptr;
    return ue::StaticLoadObject(ue::UObject::StaticClass(), nullptr, ue::FString(path));
}

const ClassComponentTemplate* ActorComponents::find(const std::string_view name) const {
    if (name.empty())
        return nullptr;
    const auto found = byName.find(lowercase(name));
    return found == byName.end() ? nullptr : &components[found->second];
}

std::optional<float> ActorComponents::getDefaultNumber(const std::string_view name) const {
    if (defaults == nullptr)
        return std::nullopt;
    float value = 0;
    if (!ue::GetPropertyValue(defaults, ue::FName(name), value))
        return std::nullopt;
    return value;
}

void ActorComponents::addNativeComponents() {
    const auto* actor = ue::Cast<ue::AActor>(defaults);
    if (actor == nullptr)
        return;
    std::unordered_map<const ue::UObject*, std::string> propertyNames;
    for (const ue::FProperty* property = defaults->GetClass()->PropertyLink; property != nullptr; property = property->PropertyLinkNext) {
        const auto* objectProperty = ue::CastField<ue::FObjectPropertyBase>(property);
        if (objectProperty == nullptr)
            continue;
        if (const ue::UObject* value = objectProperty->GetObjectPropertyValue_InContainer(defaults))
            propertyNames.try_emplace(value, toString(property->GetFName()));
    }
    for (const ue::UActorComponent* component : actor->GetComponents()) {
        if (component == nullptr)
            continue;
        const auto property = propertyNames.find(component);
        std::string name = property == propertyNames.end() ? nameOf(component) : property->second;
        if (name.empty())
            continue;
        const ue::UObject* templateObject = component;
        if (const auto found = overrides.find(lowercase(name)); found != overrides.end())
            templateObject = found->second;
        const auto existing = std::ranges::find(components, templateObject, &ClassComponentTemplate::componentTemplate);
        if (existing != components.end()) {
            byName.try_emplace(lowercase(name), static_cast<std::size_t>(existing - components.begin()));
            continue;
        }
        add({.name = std::move(name), .componentTemplate = templateObject, .parentName = parentOf(*templateObject), .socket = socketOf(*templateObject)});
    }
}

void ActorComponents::addConstructionNodes(ue::UBlueprintGeneratedClass& declaring) {
    const ue::USimpleConstructionScript* script = declaring.GetSimpleConstructionScript();
    if (script == nullptr)
        return;
    ue::UBlueprintGeneratedClass* actual = ue::Cast<ue::UBlueprintGeneratedClass>(const_cast<ue::UClass*>(defaults->GetClass()));
    const ue::TArray<ue::TObjectPtr<ue::USCS_Node>>& roots = script->GetRootNodes();
    for (int index = 0; index < roots.Num(); ++index)
        if (roots[index] != nullptr)
            addConstructionNode(*roots[index], toString(roots[index]->ParentComponentOrVariableName), *actual);
}

void ActorComponents::addConstructionNode(ue::USCS_Node& node, const std::string& parent, ue::UBlueprintGeneratedClass& actual) {
    const std::string variable = toString(node.InternalVariableName);
    const ue::UObject* componentTemplate = node.GetActualComponentTemplate(&actual);
    if (componentTemplate == nullptr)
        componentTemplate = node.ComponentTemplate;
    if (!variable.empty() && variable != "None" && componentTemplate != nullptr && !byName.contains(lowercase(variable))) {
        std::string socket = toString(node.AttachToName);
        if (socket.empty() || socket == "None")
            socket = socketOf(*componentTemplate);
        add({.name = variable, .componentTemplate = componentTemplate, .parentName = parent, .socket = std::move(socket)});
    }
    const ue::TArray<ue::TObjectPtr<ue::USCS_Node>>& children = node.GetChildNodes();
    for (int index = 0; index < children.Num(); ++index)
        if (children[index] != nullptr)
            addConstructionNode(*children[index], variable, actual);
}

void ActorComponents::add(ClassComponentTemplate component) {
    if (component.componentTemplate == nullptr || component.name.empty())
        return;
    const std::string objectName = lowercase(nameOf(component.componentTemplate));
    byName.try_emplace(lowercase(component.name), components.size());
    if (!objectName.empty())
        byName.try_emplace(objectName, components.size());
    components.push_back(std::move(component));
}

std::string ActorComponents::getMesh(const ClassComponentTemplate& component) {
    if (component.componentTemplate == nullptr)
        return {};
    return reference(*component.componentTemplate, {"SkeletalMesh", "SkinnedAsset", "StaticMesh"});
}

std::vector<std::string> ActorComponents::getMaterials(const ClassComponentTemplate& component) {
    if (component.componentTemplate == nullptr)
        return {};
    return references(*component.componentTemplate, {"OverrideMaterials"}).value_or(std::vector<std::string>{});
}

}
