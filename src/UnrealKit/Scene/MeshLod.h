#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <vector>

#include "Engine/Source/Runtime/Engine/Engine/SkeletalMesh.h"
#include "Engine/Source/Runtime/Engine/Engine/StaticMesh.h"
#include "Engine/Source/Runtime/Engine/Rendering/SkeletalMeshLODRenderData.h"
#include "UnrealKit/Assets/Reading.h"

namespace UnrealKit::Scene {

struct MeshMaterial {
    std::string slotName;
    std::string materialPath;
};

struct MeshSection {
    int materialIndex = 0;
    int firstIndex = 0;
    int triangleCount = 0;
};

bool drawsAnySection(const ue::USkeletalMesh& mesh);

bool isSkeletonPlaceholder(const ue::USkeletalMesh& mesh);

class MeshLod {
public:
    MeshLod() = default;
    std::expected<void, Core::Error> load(std::string_view objectPath, std::stop_token stop);

    bool isSkeletal() const { return skinned != nullptr; }
    std::size_t getVertexCount() const { return positions.size(); }
    std::span<const ue::FVector3f> getPositions() const { return positions; }
    ue::FVector4f getNormal(std::size_t vertex) const { return vertexBuffer->VertexTangentZ(static_cast<ue::uint32>(vertex)); }
    std::span<const ue::FMeshTangents> getTangents() const {
        return std::span<const ue::FMeshTangents>(vertexBuffer->GetTangentData()).first(positions.size());
    }
    int getUvChannelCount() const { return static_cast<int>(vertexBuffer->GetNumTexCoords()); }
    std::span<const ue::FVector2f> getUvs() const {
        return std::span<const ue::FVector2f>(vertexBuffer->GetTexCoordData()).first(positions.size() * vertexBuffer->GetNumTexCoords());
    }
    const ue::FVector2f& getUv(std::size_t vertex, std::size_t channel) const {
        return vertexBuffer->GetVertexUV(static_cast<ue::uint32>(vertex), static_cast<ue::uint32>(channel));
    }
    std::span<const ue::FColor> getColors() const { return colors; }
    std::span<const std::uint32_t> getIndices() const { return indices; }
    std::span<const MeshSection> getSections() const { return sections; }
    std::span<const MeshMaterial> getMaterials() const { return materials; }
    std::span<const Assets::Reading::MeshSocket> getSockets() const { return sockets; }
    const ue::FReferenceSkeleton* getReferenceSkeleton() const { return referenceSkeleton; }
    const ue::FSkeletalMeshLODRenderData* getSkinnedLod() const { return skinned; }

private:
    std::expected<void, Core::Error> readStatic(const ue::UStaticMesh& mesh, std::string_view path);
    std::expected<void, Core::Error> readSkeletal(const ue::USkeletalMesh& mesh, std::string_view path);
    std::expected<void, Core::Error> readVertices(const ue::FStaticMeshVertexBuffers& buffers, std::string_view path);
    std::expected<void, Core::Error> checkSections(std::string_view path) const;
    std::expected<void, Core::Error> checkBones(std::string_view path) const;
    std::expected<void, Core::Error> checkSkin(std::string_view path) const;

    std::span<const ue::FVector3f> positions;
    const ue::FStaticMeshVertexBuffer* vertexBuffer = nullptr;
    std::span<const ue::FColor> colors;
    std::span<const std::uint32_t> indices;
    std::vector<std::uint32_t> drawnIndices;
    std::vector<MeshSection> sections;
    std::vector<MeshMaterial> materials;
    std::vector<Assets::Reading::MeshSocket> sockets;
    const ue::FReferenceSkeleton* referenceSkeleton = nullptr;
    const ue::FSkeletalMeshLODRenderData* skinned = nullptr;
};

}
