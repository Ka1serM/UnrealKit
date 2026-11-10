#include "UnrealKit/Assets/Reading.h"

#include <cctype>
#include <format>

#include "Engine/Source/Runtime/Engine/Animation/Skeleton.h"
#include "Engine/Source/Runtime/Engine/Engine/Texture2D.h"

namespace UnrealKit::Assets::Reading {

std::unexpected<Core::Error> fail(std::string code, std::string_view path, const ue5parse::Error& error) {
    if (error.code == ue5parse::ErrorCode::Cancelled)
        return Core::cancelled();
    return Core::fail(std::move(code), std::format("'{}': {} at {} (offset {}).", path, ue5parse::getErrorName(error.code), error.where, error.offset));
}

std::string toString(const ue::FString& text) {
    return text.ToUtf8();
}

std::expected<ue::UObject*, Core::Error> readObject(const std::string_view path, const std::stop_token stop) {
    const std::expected<ue::UObject*, Core::Error> loaded = loadObject<ue::UObject>(path, "object", stop);
    if (!loaded)
        return loaded;
    ue::UClass* blueprint = ue::Cast<ue::UClass>(*loaded);
    if (blueprint == nullptr)
        return loaded;
    ue::UObject* defaults = blueprint->GetDefaultObject();
    if (defaults == nullptr)
        return Core::fail("object.default", std::format("The Blueprint '{}' has no class-default object.", path));
    return defaults;
}

namespace {

MeshSocket socketOf(const ue::USkeletalMeshSocket& socket) {
    return {toString(socket.SocketName), toString(socket.BoneName), socket.RelativeLocation, socket.RelativeRotation, socket.RelativeScale};
}

}

std::vector<MeshSocket> readMeshSockets(const ue::TArray<ue::TObjectPtr<ue::UStaticMeshSocket>>& sockets) {
    std::vector<MeshSocket> result;
    for (const ue::UStaticMeshSocket* socket : sockets)
        if (socket != nullptr)
            result.push_back({toString(socket->SocketName), {}, socket->RelativeLocation, socket->RelativeRotation, socket->RelativeScale});
    return result;
}

std::vector<MeshSocket> readSkeletalMeshSockets(const ue::USkeletalMesh& mesh) {
    std::vector<MeshSocket> result;
    for (const ue::USkeletalMeshSocket* socket : mesh.GetMeshOnlySocketList())
        if (socket != nullptr)
            result.push_back(socketOf(*socket));
    if (const ue::USkeleton* skeleton = mesh.GetSkeleton())
        for (const ue::USkeletalMeshSocket* socket : skeleton->Sockets)
            if (socket != nullptr)
                result.push_back(socketOf(*socket));
    return result;
}

std::expected<const ue::FSkeletalMeshLODRenderData*, Core::Error> firstSkeletalLod(const ue::USkeletalMesh& mesh, const std::string_view path) {
    if (const ue::FSkeletalMeshRenderData* data = mesh.GetResourceForRendering())
        for (const ue::FSkeletalMeshLODRenderData& lod : data->LODRenderData)
            if (lod.bHasBuffers && lod.MultiSizeIndexContainer.Num() > 0)
                return &lod;
    return Core::fail("mesh.lod", std::format("'{}' has no LOD with render data.", path));
}

std::expected<std::pair<int, int>, Core::Error> textureSize(const std::string_view path, const std::stop_token stop) {
    const std::expected<ue::UTexture2D*, Core::Error> texture = loadObject<ue::UTexture2D>(path, "texture", stop);
    if (!texture)
        return std::unexpected(texture.error());
    const ue::FTexturePlatformData* data = (*texture)->GetPlatformData();
    if (data == nullptr || data->SizeX <= 0 || data->SizeY <= 0)
        return Core::fail("texture.size", std::format("'{}' has no size.", path));
    return std::pair(data->SizeX, data->SizeY);
}

std::expected<std::string, Core::Error> displayName(const ue::UObject& source, const std::string_view objectPath, const std::stop_token stop) {
    std::string name;
    if (const std::string uiData = reference(source, "UIData"); !uiData.empty()) {
        const std::expected<ue::UObject*, Core::Error> data = readObject(uiData, stop);
        if (!data)
            return std::unexpected(data.error());
        name = text(**data, "DisplayName");
    }
    if (name.empty())
        name = text(source, "ShippingName");
    return name.empty() ? objectName(objectPath) : name;
}

std::string pathOf(const ue::UObject* object) {
    return object == nullptr ? std::string() : toString(object->GetPathName());
}

std::string pathOf(const ue::FProperty* property, const void* value) {
    if (property == nullptr || value == nullptr)
        return {};
    if (const auto* soft = ue::CastField<ue::FSoftObjectProperty>(property)) {
        const ue::FSoftObjectPath& path = static_cast<const ue::FSoftObjectPtr*>(value)->GetUniqueID();
        return path.IsNull() ? std::string() : toString(path.ToString());
    }
    if (ue::CastField<ue::FObjectProperty>(property) != nullptr)
        return toString(static_cast<const ue::FObjectPtr*>(value)->GetPathName());
    if (const auto* objectProperty = ue::CastField<ue::FObjectPropertyBase>(property))
        return pathOf(objectProperty->GetObjectPropertyValue(value));
    if (const auto* structProperty = ue::CastField<ue::FStructProperty>(property))
        if (structProperty->Struct == ue::TBaseStructure<ue::FSoftObjectPath>::Get()) {
            const auto& path = *static_cast<const ue::FSoftObjectPath*>(value);
            return path.IsNull() ? std::string() : toString(path.ToString());
        }
    return {};
}

namespace {

std::string enumText(const ue::UEnum* type, const std::int64_t number) {
    if (type == nullptr)
        return std::to_string(number);
    const ue::FName name = type->GetNameByValue(number);
    if (name.isNone())
        return {};
    std::string text = name.toString();
    if (text.find("::") == std::string::npos)
        text = toString(type->GetName()) + "::" + text;
    return text;
}

const ue::FProperty* findProperty(const ue::UObject& source, const std::string_view name) {
    return source.GetClass()->FindPropertyByName(ue::FName(name));
}

const void* valueOf(const ue::UObject& source, const ue::FProperty* property) {
    return property->ContainerPtrToValuePtr<void>(&source);
}

}

std::string textOf(const ue::FProperty* property, const void* value) {
    if (property == nullptr || value == nullptr)
        return {};
    if (property->IsA<ue::FStrProperty>())
        return toString(*static_cast<const ue::FString*>(value));
    if (property->IsA<ue::FNameProperty>())
        return toString(*static_cast<const ue::FName*>(value));
    if (property->IsA<ue::FTextProperty>())
        return toString(static_cast<const ue::FText*>(value)->ToString());
    if (const auto* enumProperty = ue::CastField<ue::FEnumProperty>(property))
        return enumText(enumProperty->GetEnum(), enumProperty->GetUnderlyingProperty()->GetSignedIntPropertyValue(value));
    if (const auto* byteProperty = ue::CastField<ue::FByteProperty>(property))
        if (byteProperty->Enum != nullptr)
            return enumText(byteProperty->Enum, *static_cast<const std::uint8_t*>(value));
    return {};
}

std::string text(const ue::UObject& source, const std::string_view name) {
    const ue::FProperty* property = findProperty(source, name);
    return property == nullptr ? std::string() : textOf(property, valueOf(source, property));
}

namespace {

template <class Visit>
void forEachArchetype(const ue::UObject& source, Visit&& visit) {
    const ue::UObject* object = &source;
    for (int depth = 0; object != nullptr && depth < 64; ++depth) {
        visit(*object);
        ue::UObject* next = object->GetArchetype();
        if (next == object)
            break;
        object = next;
    }
}

}

std::string chainText(const ue::UObject& source, const std::string_view name) {
    std::string result;
    forEachArchetype(source, [&](const ue::UObject& object) {
        if (result.empty())
            result = text(object, name);
    });
    return result;
}

std::string chainReference(const ue::UObject& source, const std::initializer_list<std::string_view> names) {
    std::string result;
    forEachArchetype(source, [&](const ue::UObject& object) {
        if (result.empty())
            result = reference(object, names);
    });
    return result;
}

std::optional<std::vector<std::string>> chainReferences(const ue::UObject& source, const std::initializer_list<std::string_view> names) {
    std::optional<std::vector<std::string>> result;
    forEachArchetype(source, [&](const ue::UObject& object) {
        if (!result)
            result = references(object, names);
    });
    return result;
}

std::string reference(const ue::UObject& source, const std::initializer_list<std::string_view> names) {
    for (const std::string_view name : names) {
        const ue::FProperty* property = findProperty(source, name);
        if (property == nullptr)
            continue;
        std::string path = pathOf(property, valueOf(source, property));
        if (!path.empty())
            return path;
    }
    return {};
}

ue::UObject* object(const ue::UObject& source, const std::string_view name) {
    const auto* property = ue::CastField<ue::FObjectPropertyBase>(findProperty(source, name));
    return property == nullptr ? nullptr : property->GetObjectPropertyValue(valueOf(source, property));
}

std::string uuid(const ue::UObject& source) {
    ue::FGuid guid;
    if (!ue::GetPropertyValue(&source, ue::FName("Uuid"), guid))
        return {};
    return std::format("{:08x}-{:04x}-{:04x}-{:04x}-{:04x}{:08x}", guid.a, guid.b >> 16, guid.b & 0xffff, guid.c >> 16, guid.c & 0xffff, guid.d);
}

ue::FLinearColor slateColor(const ue::UObject& source, const std::string_view name) {
    const auto* property = ue::CastField<ue::FStructProperty>(findProperty(source, name));
    ue::FLinearColor color{0, 0, 0, 0};
    if (property != nullptr)
        ue::GetPropertyValue_InContainer(property->Struct, valueOf(source, property), ue::FName("SpecifiedColor"), color);
    return color;
}

namespace {

template <class Visit>
bool forEachElement(const ue::FProperty* property, const void* value, Visit&& visit) {
    if (const auto* array = ue::CastField<ue::FArrayProperty>(property)) {
        ue::FScriptArrayHelper helper(array, value);
        for (int i = 0; i < helper.Num(); ++i)
            visit(array->Inner, helper.GetRawPtr(i));
        return true;
    }
    if (const auto* set = ue::CastField<ue::FSetProperty>(property)) {
        ue::FScriptSetHelper helper(set, value);
        for (int i = 0; i < helper.GetMaxIndex(); ++i)
            if (helper.IsValidIndex(i))
                visit(set->ElementProp, helper.GetElementPtr(i));
        return true;
    }
    return false;
}

}

std::optional<std::vector<std::string>> references(const ue::UObject& source, const std::initializer_list<std::string_view> names) {
    for (const std::string_view name : names) {
        const ue::FProperty* property = findProperty(source, name);
        if (property == nullptr)
            continue;
        std::vector<std::string> paths;
        forEachElement(property, valueOf(source, property), [&](const ue::FProperty* element, const void* value) { paths.push_back(pathOf(element, value)); });
        if (!paths.empty())
            return paths;
    }
    return std::nullopt;
}

std::optional<std::vector<std::string>> objectReferences(const ue::UObject& source, const std::string_view name) {
    const ue::FProperty* property = findProperty(source, name);
    if (property == nullptr)
        return std::nullopt;
    std::vector<std::string> paths;
    const bool container = forEachElement(property, valueOf(source, property), [&](const ue::FProperty* element, const void* value) {
        std::string path = pathOf(element, value);
        if (!path.empty())
            paths.push_back(std::move(path));
    });
    if (!container)
        return std::nullopt;
    return paths;
}

std::vector<std::pair<std::string, std::string>> referenceMap(const ue::UObject& source, const std::string_view name) {
    const auto* property = ue::CastField<ue::FMapProperty>(findProperty(source, name));
    if (property == nullptr)
        return {};
    ue::FScriptMapHelper helper(property, valueOf(source, property));
    std::vector<std::pair<std::string, std::string>> result;
    for (int i = 0; i < helper.GetMaxIndex(); ++i)
        if (helper.IsValidIndex(i))
            result.emplace_back(pathOf(property->KeyProp, helper.GetKeyPtr(i)), pathOf(property->ValueProp, helper.GetValuePtr(i)));
    return result;
}

std::string lowercase(const std::string_view text) {
    std::string result(text);
    for (char& c : result)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return result;
}

std::string objectName(const std::string_view objectPath) {
    const std::size_t dot = objectPath.rfind('.');
    std::string_view name = dot == std::string_view::npos ? objectPath : objectPath.substr(dot + 1);
    if (name.ends_with("_C"))
        name.remove_suffix(2);
    return std::string(name);
}

std::string_view enumName(const std::string_view text) {
    const std::size_t separator = text.rfind("::");
    return separator == std::string_view::npos ? text : text.substr(separator + 2);
}

}
