#pragma once

#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include "UnrealKit/Core/Error.h"

namespace ue5parse::runtime {
class UObject;
class UBlueprintGeneratedClass;
class USCS_Node;
}

namespace ue = ue5parse::runtime;

namespace UnrealKit::Assets {

struct ClassComponentTemplate {
    std::string name;
    const ue::UObject* componentTemplate = nullptr;
    std::string parentName;
    std::string socket;
};

class ActorComponents {
public:
    ActorComponents() = default;
    ActorComponents(const ActorComponents&) = delete;
    ActorComponents& operator=(const ActorComponents&) = delete;

    std::expected<void, Core::Error> read(std::string_view classPath, std::stop_token stop);

    const std::vector<ClassComponentTemplate>& getAll() const noexcept { return components; }
    const ClassComponentTemplate* find(std::string_view name) const;
    std::optional<float> getDefaultNumber(std::string_view name) const;

    static std::string getMesh(const ClassComponentTemplate& component);
    static std::vector<std::string> getMaterials(const ClassComponentTemplate& component);

private:
    void addNativeComponents();
    void addConstructionNodes(ue::UBlueprintGeneratedClass& declaring);
    void addConstructionNode(ue::USCS_Node& node, const std::string& parent, ue::UBlueprintGeneratedClass& actual);
    void add(ClassComponentTemplate component);
    ue::UObject* load(std::string_view path) const;

    const ue::UObject* defaults = nullptr;
    std::vector<ClassComponentTemplate> components;
    std::unordered_map<std::string, std::size_t> byName;
    std::unordered_map<std::string, const ue::UObject*> overrides;
};

}
