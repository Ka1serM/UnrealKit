#include "UnrealKit/Scene/MeshRecord.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <format>

#include <tbb/parallel_for.h>

#include "UnrealKit/Assets/Reading.h"

namespace UnrealKit::Scene {

namespace {

using Builder = flatbuffers::FlatBufferBuilder;

std::string lowerCase(const std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), [](const unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return result;
}

template <class T, class Fill>
flatbuffers::Offset<flatbuffers::Vector<T>> filledVector(Builder& builder, const std::size_t count, Fill fill) {
    T* values = nullptr;
    const auto offset = builder.CreateUninitializedVector(count, &values);
    fill(values);
    return offset;
}

void fillTriangleMaterials(const MeshLod& mesh, std::uint16_t* materials) {
    std::fill_n(materials, mesh.getIndices().size() / 3, std::uint16_t{0});
    for (const MeshSection& section : mesh.getSections())
        std::fill_n(materials + section.firstIndex / 3, section.triangleCount, static_cast<std::uint16_t>(section.materialIndex));
}

struct Influences {
    std::size_t count = 0;
    std::vector<std::uint16_t> bones;
    std::vector<std::uint16_t> weights;
};

Influences influencesOf(const MeshLod& mesh) {
    const ue::FSkeletalMeshLODRenderData* lod = mesh.getSkinnedLod();
    if (lod == nullptr)
        return {};
    const ue::FSkinWeightVertexBuffer& skin = lod->SkinWeightVertexBuffer;
    const std::uint32_t scale = skin.Use16BitBoneWeight() ? 1 : 257;
    Influences result{.count = skin.GetMaxBoneInfluences()};
    result.bones.assign(mesh.getVertexCount() * result.count, 0);
    result.weights.assign(mesh.getVertexCount() * result.count, 0);
    for (const ue::FSkelMeshRenderSection& section : lod->RenderSections) {
        const ue::uint32 end = section.BaseVertexIndex + section.NumVertices;
        for (ue::uint32 vertex = section.BaseVertexIndex; vertex < end; ++vertex)
            for (std::size_t slot = 0; slot < result.count; ++slot) {
                const std::size_t at = vertex * result.count + slot;
                const auto index = static_cast<ue::uint32>(slot);
                result.weights[at] = static_cast<std::uint16_t>(skin.GetBoneWeight(vertex, index) * scale);
                result.bones[at] = section.BoneMap[static_cast<ue::int32>(skin.GetBoneIndex(vertex, index))];
            }
    }
    return result;
}

Frame encodeMesh(const std::string_view key, const std::string_view name, const MeshLod& mesh, const std::string_view skeleton,
                 const std::span<const unsigned char> overrideColors) {
    const std::size_t vertexCount = mesh.getVertexCount();
    const std::span<const std::uint32_t> indices = mesh.getIndices();
    const auto channelCount = static_cast<std::size_t>(mesh.getUvChannelCount());
    const Influences influences = influencesOf(mesh);
    const std::size_t vertexBytes = 3 * 4 + 3 * 4 + 4 * 4 + 4 + channelCount * 8 + influences.count * 4;
    Builder builder(vertexCount * vertexBytes + indices.size() * 4 + indices.size() / 3 * 2 + mesh.getMaterials().size() * 64 + 4096);

    const std::span<const ue::FVector3f> sourcePositions = mesh.getPositions();
    const auto positions = filledVector<float>(builder, vertexCount * 3, [&](float* target) {
        tbb::parallel_for(std::size_t{0}, vertexCount, [&](const std::size_t i) {
            target[i * 3] = sourcePositions[i].X;
            target[i * 3 + 1] = sourcePositions[i].Y;
            target[i * 3 + 2] = sourcePositions[i].Z;
        });
    });
    const auto normals = filledVector<float>(builder, vertexCount * 3, [&](float* target) {
        tbb::parallel_for(std::size_t{0}, vertexCount, [&](const std::size_t i) {
            const ue::FVector4f normal = mesh.getNormal(i);
            const float length = std::max(std::sqrt(normal.X * normal.X + normal.Y * normal.Y + normal.Z * normal.Z), 1e-8f);
            target[i * 3] = normal.X / length;
            target[i * 3 + 1] = normal.Y / length;
            target[i * 3 + 2] = normal.Z / length;
        });
    });
    const auto uvs = filledVector<float>(builder, channelCount * vertexCount * 2, [&](float* target) {
        tbb::parallel_for(std::size_t{0}, vertexCount, [&](const std::size_t i) {
            for (std::size_t channel = 0; channel < channelCount; ++channel) {
                const ue::FVector2f& uv = mesh.getUv(i, channel);
                target[(channel * vertexCount + i) * 2] = uv.X;
                target[(channel * vertexCount + i) * 2 + 1] = uv.Y;
            }
        });
    });
    const std::span<const ue::FColor> meshColors = mesh.getColors();
    const auto colors = filledVector<std::uint8_t>(builder, vertexCount * 4, [&](std::uint8_t* target) {
        if (!overrideColors.empty()) {
            std::copy_n(overrideColors.data(), vertexCount * 4, target);
            return;
        }
        if (meshColors.empty()) {
            std::fill_n(target, vertexCount * 4, std::uint8_t{255});
            return;
        }
        for (std::size_t i = 0; i < vertexCount; ++i) {
            const ue::FColor& source = meshColors[i];
            target[i * 4] = source.R;
            target[i * 4 + 1] = source.G;
            target[i * 4 + 2] = source.B;
            target[i * 4 + 3] = source.A;
        }
    });
    const std::span<const ue::FMeshTangents> sourceTangents = mesh.getTangents();
    const auto tangents = filledVector<float>(builder, vertexCount * 4, [&](float* target) {
        tbb::parallel_for(std::size_t{0}, vertexCount, [&](const std::size_t i) {
            const ue::FMeshTangents& tangent = sourceTangents[i];
            target[i * 4] = tangent.TangentX.X;
            target[i * 4 + 1] = tangent.TangentX.Y;
            target[i * 4 + 2] = tangent.TangentX.Z;
            target[i * 4 + 3] = tangent.TangentZ.W;
        });
    });
    const auto corners = builder.CreateVector(indices.data(), indices.size());
    const auto triangleMaterials =
        filledVector<std::uint16_t>(builder, indices.size() / 3, [&](std::uint16_t* target) { fillTriangleMaterials(mesh, target); });
    std::vector<flatbuffers::Offset<flatbuffers::String>> slots;
    slots.reserve(mesh.getMaterials().size());
    for (const MeshMaterial& material : mesh.getMaterials())
        slots.push_back(builder.CreateString(material.slotName));
    const auto slotVector = builder.CreateVector(slots);
    const bool skinned = influences.count > 0;
    const auto skeletonText = skinned ? builder.CreateString(skeleton) : 0;
    const auto boneVector = skinned ? builder.CreateVector(influences.bones) : 0;
    const auto weightVector = skinned ? builder.CreateVector(influences.weights) : 0;
    const auto keyText = builder.CreateString(key);
    const auto nameText = builder.CreateString(name);
    return finish(builder, scene::stream::Record::Mesh,
                  scene::stream::CreateMesh(builder, keyText, nameText, positions, normals, static_cast<std::uint8_t>(channelCount), uvs, colors, corners,
                                            triangleMaterials, slotVector, skeletonText, static_cast<std::uint8_t>(influences.count), boneVector, weightVector,
                                            tangents));
}

}

Frame mesh(const std::string_view key, const std::string_view name, const MeshLod& mesh, const std::string_view skeleton) {
    return encodeMesh(key, name, mesh, skeleton, {});
}

std::expected<Frame, Core::Error> mesh(const std::string_view key, const std::string_view name, const MeshLod& mesh, const std::string_view skeleton,
                                       const std::span<const unsigned char> colors) {
    if (colors.size() != mesh.getVertexCount() * 4)
        return Core::fail("mesh.colors",
                          std::format("'{}' has {} vertices but the component has {} vertex colors.", key, mesh.getVertexCount(), colors.size() / 4));
    return encodeMesh(key, name, mesh, skeleton, colors);
}

Skeleton::Skeleton(const MeshLod& mesh, const MeshLod* poseSource) : mesh(mesh), poseSource(poseSource), referenceSkeleton(*mesh.getReferenceSkeleton()) {
    const std::span<const ue::FMeshBoneInfo> boneInfo = referenceSkeleton.GetRefBoneInfo();
    const std::span<const ue::FTransform> binds = referenceSkeleton.GetRefBonePose();
    boneNames.reserve(boneInfo.size());
    poses.reserve(boneInfo.size());
    boneIndices.reserve(boneInfo.size());
    for (std::size_t i = 0; i < boneInfo.size(); ++i) {
        boneNames.push_back(boneInfo[i].Name.toString());
        boneIndices.try_emplace(lowerCase(boneNames.back()), i);
        const int parent = boneInfo[i].ParentIndex;
        poses.push_back(parent < 0 ? matrixOf(binds[i]) : poses[static_cast<std::size_t>(parent)] * matrixOf(binds[i]));
    }
}

Frame Skeleton::record(const std::string_view key, const std::string_view name) const {
    Builder builder(boneNames.size() * 128 + 512);
    const std::span<const ue::FTransform> binds = referenceSkeleton.GetRefBonePose();
    std::vector<flatbuffers::Offset<scene::stream::Bone>> offsets;
    offsets.reserve(boneNames.size());
    for (std::size_t i = 0; i < boneNames.size(); ++i) {
        const ue::FVector& t = binds[i].GetTranslation();
        const ue::FQuat& r = binds[i].GetRotation();
        const ue::FVector& s = binds[i].GetScale3D();
        const std::array<float, 3> location{static_cast<float>(t.X), static_cast<float>(t.Y), static_cast<float>(t.Z)};
        const std::array<float, 4> rotation{static_cast<float>(r.X), static_cast<float>(r.Y), static_cast<float>(r.Z), static_cast<float>(r.W)};
        const std::array<float, 3> scale{static_cast<float>(s.X), static_cast<float>(s.Y), static_cast<float>(s.Z)};
        const auto boneName = builder.CreateString(boneNames[i]);
        const auto locationVector = builder.CreateVector(location.data(), location.size());
        const auto rotationVector = builder.CreateVector(rotation.data(), rotation.size());
        const auto scaleVector = builder.CreateVector(scale.data(), scale.size());
        offsets.push_back(scene::stream::CreateBone(builder, boneName, referenceSkeleton.GetParentIndex(static_cast<ue::int32>(i)), locationVector,
                                                    rotationVector, scaleVector));
    }
    const auto boneVector = builder.CreateVector(offsets);
    const auto keyText = builder.CreateString(key);
    const auto nameText = builder.CreateString(name);
    return finish(builder, scene::stream::Record::Skeleton, scene::stream::CreateSkeleton(builder, keyText, nameText, boneVector));
}

std::optional<std::size_t> Skeleton::findBone(const std::string_view name) const {
    const auto found = boneIndices.find(lowerCase(name));
    if (found == boneIndices.end())
        return std::nullopt;
    return found->second;
}

std::optional<BoneAttachment> Skeleton::attachment(const std::string_view name) const {
    const std::optional<std::pair<std::size_t, Matrix>> point = findPoint(name);
    if (!point)
        return std::nullopt;
    return BoneAttachment{.bone = boneNames[point->first], .relative = point->second};
}

std::optional<Matrix> Skeleton::bindMatrix(const std::string_view name) const {
    const std::optional<std::pair<std::size_t, Matrix>> point = findPoint(name);
    if (!point)
        return std::nullopt;
    return poses[point->first] * point->second;
}

std::optional<Vector3> Skeleton::bindPosition(const std::string_view name) const {
    return bindMatrix(name).transform([](const Matrix& matrix) { return matrix.translation(); });
}

std::optional<std::pair<std::size_t, Matrix>> Skeleton::findPoint(const std::string_view name) const {
    const std::string wanted = lowerCase(name);
    if (auto point = findSocket(mesh.getSockets(), wanted))
        return point;
    if (poseSource)
        if (auto point = findSocket(poseSource->getSockets(), wanted))
            return point;
    const std::optional<std::size_t> bone = findBone(name);
    if (!bone)
        return std::nullopt;
    return std::pair(*bone, Matrix::identity());
}

std::optional<std::pair<std::size_t, Matrix>> Skeleton::findSocket(const std::span<const Assets::Reading::MeshSocket> sockets,
                                                                   const std::string_view wanted) const {
    const auto socket = std::ranges::find_if(sockets, [&](const Assets::Reading::MeshSocket& candidate) { return lowerCase(candidate.name) == wanted; });
    if (socket == sockets.end())
        return std::nullopt;
    const std::optional<std::size_t> bone = findBone(socket->bone);
    if (!bone)
        return std::nullopt;
    return std::pair(*bone, matrixOf(socket->location, socket->rotation, socket->scale));
}

Frame quad(const std::string_view key, const std::string_view name) {
    Builder builder(1024);
    const std::array<float, 12> positions{0, -0.5f, 0.5f, 0, 0.5f, 0.5f, 0, 0.5f, -0.5f, 0, -0.5f, -0.5f};
    const std::array<float, 12> normals{-1, 0, 0, -1, 0, 0, -1, 0, 0, -1, 0, 0};
    const std::array<float, 8> uvs{0, 0, 1, 0, 1, 1, 0, 1};
    const std::array<std::uint8_t, 16> colors{255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255};
    const std::array<std::uint32_t, 6> indices{0, 2, 1, 0, 3, 2};
    const std::array<std::uint16_t, 2> triangleMaterials{0, 0};
    const std::array<float, 16> tangents{0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1, 0, 1};
    const auto positionVector = builder.CreateVector(positions.data(), positions.size());
    const auto normalVector = builder.CreateVector(normals.data(), normals.size());
    const auto uvVector = builder.CreateVector(uvs.data(), uvs.size());
    const auto colorVector = builder.CreateVector(colors.data(), colors.size());
    const auto indexVector = builder.CreateVector(indices.data(), indices.size());
    const auto materialVector = builder.CreateVector(triangleMaterials.data(), triangleMaterials.size());
    const auto slotVector = builder.CreateVector(std::vector{builder.CreateString("Quad")});
    const auto tangentVector = builder.CreateVector(tangents.data(), tangents.size());
    const auto keyText = builder.CreateString(key);
    const auto nameText = builder.CreateString(name);
    return finish(builder, scene::stream::Record::Mesh,
                  scene::stream::CreateMesh(builder, keyText, nameText, positionVector, normalVector, 1, uvVector, colorVector, indexVector, materialVector,
                                            slotVector, 0, 0, 0, 0, tangentVector));
}

}
