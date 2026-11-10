#pragma once

#include <initializer_list>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "CppUE5Parse/Core/Result.h"
#include "Engine/Source/Runtime/CoreUObject/UObject/PropertyAccess.h"
#include "Engine/Source/Runtime/Engine/Engine/SkeletalMesh.h"
#include "Engine/Source/Runtime/Engine/Engine/StaticMeshSocket.h"
#include "UnrealKit/Core/Error.h"

namespace ue = ue5parse::runtime;

namespace UnrealKit::Assets::Reading {

[[nodiscard]] std::unexpected<Core::Error> fail(std::string code, std::string_view path, const ue5parse::Error& error);

template <class T>
std::expected<T*, Core::Error> loadObject(std::string_view path, std::string_view code, std::stop_token stop);

std::expected<ue::UObject*, Core::Error> readObject(std::string_view path, std::stop_token stop);

std::expected<std::pair<int, int>, Core::Error> textureSize(std::string_view path, std::stop_token stop);

struct MeshSocket {
    std::string name;
    std::string bone;
    ue::FVector location;
    ue::FRotator rotation;
    ue::FVector scale;
};

std::vector<MeshSocket> readMeshSockets(const ue::TArray<ue::TObjectPtr<ue::UStaticMeshSocket>>& sockets);
std::vector<MeshSocket> readSkeletalMeshSockets(const ue::USkeletalMesh& mesh);
std::expected<const ue::FSkeletalMeshLODRenderData*, Core::Error> firstSkeletalLod(const ue::USkeletalMesh& mesh, std::string_view path);

std::expected<std::string, Core::Error> displayName(const ue::UObject& source, std::string_view objectPath, std::stop_token stop);

std::string pathOf(const ue::UObject* object);
std::string pathOf(const ue::FProperty* property, const void* value);
std::string textOf(const ue::FProperty* property, const void* value);

std::string text(const ue::UObject& source, std::string_view name);
std::string reference(const ue::UObject& source, std::initializer_list<std::string_view> names);
inline std::string reference(const ue::UObject& source, std::string_view name) {
    return reference(source, {name});
}

template <class T>
T chainValue(const ue::UObject& source, std::string_view name, T fallback) {
    for (const ue::UObject* object = &source; object != nullptr; object = object->GetArchetype()) {
        T result{};
        if (ue::GetPropertyValue(object, ue::FName(name), result))
            return result;
    }
    return fallback;
}
std::string chainText(const ue::UObject& source, std::string_view name);
std::string chainReference(const ue::UObject& source, std::initializer_list<std::string_view> names);
inline std::string chainReference(const ue::UObject& source, std::string_view name) {
    return chainReference(source, {name});
}
std::optional<std::vector<std::string>> chainReferences(const ue::UObject& source, std::initializer_list<std::string_view> names);
inline std::optional<std::vector<std::string>> chainReferences(const ue::UObject& source, std::string_view name) {
    return chainReferences(source, {name});
}
ue::UObject* object(const ue::UObject& source, std::string_view name);
std::string uuid(const ue::UObject& source);
ue::FLinearColor slateColor(const ue::UObject& source, std::string_view name);
std::optional<std::vector<std::string>> references(const ue::UObject& source, std::initializer_list<std::string_view> names);
inline std::optional<std::vector<std::string>> references(const ue::UObject& source, std::string_view name) {
    return references(source, {name});
}
std::optional<std::vector<std::string>> objectReferences(const ue::UObject& source, std::string_view name);
std::vector<std::pair<std::string, std::string>> referenceMap(const ue::UObject& source, std::string_view name);
std::string lowercase(std::string_view text);
std::string objectName(std::string_view objectPath);
std::string_view enumName(std::string_view text);
std::string toString(const ue::FString& text);
inline std::string toString(const ue::FName& name) {
    return name.isNone() ? std::string() : name.toString();
}

template <class T>
std::expected<T*, Core::Error> loadObject(const std::string_view path, const std::string_view code, const std::stop_token stop) {
    if (stop.stop_requested())
        return Core::cancelled();
    ue::UObject* loaded = ue::StaticLoadObject(ue::UObject::StaticClass(), nullptr, ue::FString(path));
    if (loaded == nullptr)
        return Core::fail(std::string(code) + ".load", "'" + std::string(path) + "' does not load.");
    T* typed = ue::Cast<T>(loaded);
    if (typed == nullptr)
        return Core::fail(std::string(code) + ".type", "'" + std::string(path) + "' is a " + toString(loaded->GetClass()->GetName()) + ", not a "
                                                           + toString(T::StaticClass()->GetName()) + ".");
    return typed;
}

template <class T>
T value(const ue::UObject& source, std::string_view name, T fallback) {
    T result{};
    return ue::GetPropertyValue(&source, ue::FName(name), result) ? result : fallback;
}

}
