#pragma once

#include <cstddef>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#include "UnrealKit/Scene/Matrix.h"
#include "UnrealKit/Scene/MeshLod.h"
#include "UnrealKit/Scene/Records.h"

namespace UnrealKit::Scene {

Frame mesh(std::string_view key, std::string_view name, const MeshLod& mesh, std::string_view skeleton);
std::expected<Frame, Core::Error> mesh(std::string_view key, std::string_view name, const MeshLod& mesh, std::string_view skeleton,
                                       std::span<const unsigned char> colors);
Frame quad(std::string_view key, std::string_view name);
inline constexpr std::string_view quadKey = "unrealkit:quad";

struct BoneAttachment {
    std::string bone;
    Matrix relative;
};

class Skeleton {
public:
    Skeleton(const MeshLod& mesh, const MeshLod* poseSource);

    Frame record(std::string_view key, std::string_view name) const;
    std::optional<BoneAttachment> attachment(std::string_view name) const;
    std::optional<Matrix> bindMatrix(std::string_view name) const;
    std::optional<Vector3> bindPosition(std::string_view name) const;

private:
    std::optional<std::size_t> findBone(std::string_view name) const;
    std::optional<std::pair<std::size_t, Matrix>> findPoint(std::string_view name) const;
    std::optional<std::pair<std::size_t, Matrix>> findSocket(std::span<const Assets::Reading::MeshSocket> sockets, std::string_view wanted) const;

    const MeshLod& mesh;
    const MeshLod* poseSource;
    const ue::FReferenceSkeleton& referenceSkeleton;
    std::vector<std::string> boneNames;
    std::vector<Matrix> poses;
    std::unordered_map<std::string, std::size_t> boneIndices;
};

}
