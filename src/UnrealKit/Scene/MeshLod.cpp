#include "UnrealKit/Scene/MeshLod.h"

#include <algorithm>
#include <format>
#include <limits>

#include "Engine/Source/Runtime/Engine/Materials/MaterialInterface.h"
#include "UnrealKit/Assets/Reading.h"

namespace UnrealKit::Scene {

namespace {

using namespace Assets::Reading;

template <class Material>
std::vector<MeshMaterial> readMaterials(const std::span<const Material> slots) {
    std::vector<MeshMaterial> result;
    result.reserve(slots.size());
    for (const Material& material : slots) {
        const ue::FName slot = material.ImportedMaterialSlotName.isNone() ? material.MaterialSlotName : material.ImportedMaterialSlotName;
        result.push_back({toString(slot), pathOf(material.MaterialInterface)});
    }
    return result;
}

}

std::expected<void, Core::Error> MeshLod::load(const std::string_view objectPath, const std::stop_token stop) {
    const std::expected<ue::UObject*, Core::Error> loaded = loadObject<ue::UObject>(objectPath, "mesh", stop);
    if (!loaded)
        return std::unexpected(loaded.error());
    std::expected<void, Core::Error> read;
    if (const auto* skeletal = ue::Cast<ue::USkeletalMesh>(*loaded))
        read = readSkeletal(*skeletal, objectPath);
    else if (const auto* staticMesh = ue::Cast<ue::UStaticMesh>(*loaded))
        read = readStatic(*staticMesh, objectPath);
    else
        return Core::fail("mesh.type", std::format("'{}' is not a static or skeletal mesh.", objectPath));
    if (!read)
        return read;
    return checkSections(objectPath);
}

std::expected<void, Core::Error> MeshLod::readStatic(const ue::UStaticMesh& mesh, const std::string_view path) {
    const ue::FStaticMeshLODResources* lod = nullptr;
    if (const ue::FStaticMeshRenderData* render = mesh.GetRenderData())
        for (const ue::FStaticMeshLODResources& candidate : render->LODResources)
            if (!candidate.bIsLODCookedOut && candidate.bHasBuffers) {
                lod = &candidate;
                break;
            }
    if (lod == nullptr)
        return Core::fail("mesh.lod", std::format("'{}' has no LOD with render data.", path));
    if (std::expected<void, Core::Error> vertices = readVertices(lod->VertexBuffers, path); !vertices)
        return vertices;
    indices = lod->IndexBuffer.GetIndices();
    for (const ue::FStaticMeshSection& section : lod->Sections)
        sections.push_back({section.MaterialIndex, static_cast<int>(section.FirstIndex), static_cast<int>(section.NumTriangles)});
    materials = readMaterials<ue::FStaticMaterial>(mesh.GetStaticMaterials());
    sockets = readMeshSockets(mesh.Sockets);
    return {};
}

bool drawsAnySection(const ue::USkeletalMesh& mesh) {
    const ue::FSkeletalMeshRenderData* data = mesh.GetResourceForRendering();
    if (data == nullptr)
        return false;
    for (const ue::FSkeletalMeshLODRenderData& lod : data->LODRenderData)
        if (lod.bHasBuffers && lod.MultiSizeIndexContainer.Num() > 0)
            return std::ranges::any_of(lod.RenderSections, [](const ue::FSkelMeshRenderSection& section) { return !section.bDisabled; });
    return false;
}

bool isSkeletonPlaceholder(const ue::USkeletalMesh& mesh) {
    constexpr double flatCentimetres = 0.001;
    constexpr double largestHalfSizeCentimetres = 2;
    const ue::FVector extent = mesh.GetImportedBounds().BoxExtent;
    const double thinnest = std::min({extent.X, extent.Y, extent.Z});
    const double largest = std::max({extent.X, extent.Y, extent.Z});
    return thinnest <= flatCentimetres && largest <= largestHalfSizeCentimetres;
}

std::expected<void, Core::Error> MeshLod::readSkeletal(const ue::USkeletalMesh& mesh, const std::string_view path) {
    const std::expected<const ue::FSkeletalMeshLODRenderData*, Core::Error> lod = firstSkeletalLod(mesh, path);
    if (!lod)
        return std::unexpected(lod.error());
    skinned = *lod;
    if (std::expected<void, Core::Error> vertices = readVertices(skinned->StaticVertexBuffers, path); !vertices)
        return vertices;
    const std::span<const std::uint32_t> allIndices = skinned->MultiSizeIndexContainer.GetIndices();
    for (const ue::FSkelMeshRenderSection& section : skinned->RenderSections) {
        if (section.bDisabled)
            continue;
        const std::size_t first = section.BaseIndex;
        const std::size_t count = static_cast<std::size_t>(section.NumTriangles) * 3;
        if (first + count > allIndices.size())
            return Core::fail("mesh.sections", std::format("A section of '{}' is out of range.", path));
        sections.push_back({section.MaterialIndex, static_cast<int>(drawnIndices.size()), static_cast<int>(section.NumTriangles)});
        drawnIndices.insert(drawnIndices.end(), allIndices.begin() + static_cast<std::ptrdiff_t>(first),
                            allIndices.begin() + static_cast<std::ptrdiff_t>(first + count));
    }
    indices = drawnIndices;
    materials = readMaterials<ue::FSkeletalMaterial>(mesh.GetMaterials());
    referenceSkeleton = &mesh.GetRefSkeleton();
    sockets = readSkeletalMeshSockets(mesh);
    if (std::expected<void, Core::Error> bones = checkBones(path); !bones)
        return bones;
    return checkSkin(path);
}

std::expected<void, Core::Error> MeshLod::readVertices(const ue::FStaticMeshVertexBuffers& buffers, const std::string_view path) {
    vertexBuffer = &buffers.StaticMeshVertexBuffer;
    positions = buffers.PositionVertexBuffer.GetVertexData();
    const std::size_t count = positions.size();
    if (count == 0 || vertexBuffer->GetNumVertices() < count || vertexBuffer->GetNumTexCoords() < 1)
        return Core::fail("mesh.lod", std::format("'{}' has incomplete vertex buffers.", path));
    const std::span<const ue::FColor> meshColors = buffers.ColorVertexBuffer.GetVertexData();
    if (meshColors.empty())
        return {};
    if (meshColors.size() < count)
        return Core::fail("mesh.colors", std::format("'{}' has too few vertex colors.", path));
    colors = meshColors.first(count);
    return {};
}

std::expected<void, Core::Error> MeshLod::checkSections(const std::string_view path) const {
    if (indices.size() % 3 != 0)
        return Core::fail("mesh.indices", std::format("'{}' has {} indices, not triangles.", path, indices.size()));
    const auto outside = std::ranges::find_if(indices, [&](const std::uint32_t index) { return index >= getVertexCount(); });
    if (outside != indices.end())
        return Core::fail("mesh.indices", std::format("'{}' has the index {} for {} vertices.", path, *outside, getVertexCount()));
    const std::size_t triangles = indices.size() / 3;
    const std::size_t slots = std::max<std::size_t>(materials.size(), 1);
    for (const MeshSection& section : sections) {
        const auto first = static_cast<std::size_t>(section.firstIndex) / 3;
        const auto count = static_cast<std::size_t>(section.triangleCount);
        if (section.materialIndex < 0 || static_cast<std::size_t>(section.materialIndex) >= slots || first + count > triangles)
            return Core::fail("mesh.sections", std::format("A section of '{}' is out of range.", path));
    }
    return {};
}

std::expected<void, Core::Error> MeshLod::checkBones(const std::string_view path) const {
    const std::span<const ue::FMeshBoneInfo> bones = referenceSkeleton->GetRefBoneInfo();
    if (bones.empty())
        return Core::fail("mesh.bones", std::format("'{}' has no bones.", path));
    for (std::size_t i = 0; i < bones.size(); ++i)
        if (bones[i].ParentIndex >= static_cast<int>(i))
            return Core::fail("mesh.bones", std::format("Bone {} of '{}' comes before its parent.", i, path));
    return {};
}

std::expected<void, Core::Error> MeshLod::checkSkin(const std::string_view path) const {
    const ue::FSkinWeightVertexBuffer& skin = skinned->SkinWeightVertexBuffer;
    const std::size_t vertexCount = getVertexCount();
    const std::size_t influences = skin.GetMaxBoneInfluences();
    const auto boneCount = static_cast<std::size_t>(referenceSkeleton->GetNum());
    if (skin.GetNumVertices() < vertexCount)
        return Core::fail("mesh.weights", std::format("'{}' has too few skin weights.", path));
    if (influences > std::numeric_limits<std::uint8_t>::max())
        return Core::fail("mesh.weights", std::format("'{}' has too many bone influences.", path));
    std::size_t nextVertex = 0;
    for (const ue::FSkelMeshRenderSection& section : skinned->RenderSections) {
        const std::size_t sectionVertices = section.NumVertices;
        if (section.BaseVertexIndex != nextVertex || nextVertex + sectionVertices > vertexCount)
            return Core::fail("mesh.sections", std::format("A section of '{}' does not follow the previous section.", path));
        const auto mapSize = static_cast<std::size_t>(section.BoneMap.Num());
        for (std::size_t i = nextVertex; i < nextVertex + sectionVertices; ++i)
            for (std::size_t slot = 0; slot < influences; ++slot) {
                const auto vertex = static_cast<ue::uint32>(i);
                const auto influence = static_cast<ue::uint32>(slot);
                if (skin.GetBoneWeight(vertex, influence) == 0)
                    continue;
                const std::size_t boneIndex = skin.GetBoneIndex(vertex, influence);
                if (boneIndex >= mapSize || section.BoneMap[static_cast<ue::int32>(boneIndex)] >= boneCount)
                    return Core::fail("mesh.sections", std::format("'{}' has an invalid bone index.", path));
            }
        nextVertex += sectionVertices;
    }
    if (nextVertex != vertexCount)
        return Core::fail("mesh.sections", std::format("The sections of '{}' do not cover all vertices.", path));
    return {};
}

}
