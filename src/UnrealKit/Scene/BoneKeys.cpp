#include "UnrealKit/Scene/BoneKeys.h"

#include <algorithm>
#include <cmath>

#include "UnrealKit/Scene/Matrix.h"

namespace UnrealKit::Scene {

namespace {

constexpr float ownVertexReach = 5.0f;

}

KeyReducer::KeyReducer(const std::size_t components, const float tolerance) : components(components), tolerance(tolerance) {
    kept.components = static_cast<std::uint8_t>(components);
    low.fill(-std::numeric_limits<float>::infinity());
    high.fill(std::numeric_limits<float>::infinity());
    minimum.fill(std::numeric_limits<float>::infinity());
    maximum.fill(-std::numeric_limits<float>::infinity());
}

void KeyReducer::keep(const float frame, const Values& values) {
    kept.frames.push_back(frame);
    kept.values.insert(kept.values.end(), values.begin(), values.begin() + static_cast<std::ptrdiff_t>(components));
}

void KeyReducer::add(const float frame, const std::span<const float> values) {
    Values value{};
    std::copy_n(values.begin(), components, value.begin());
    for (std::size_t i = 0; i < components; ++i) {
        minimum[i] = std::min(minimum[i], value[i]);
        maximum[i] = std::max(maximum[i], value[i]);
    }
    lastValues = value;
    if (count++ == 0) {
        keep(frame, value);
        anchorFrame = frame;
        anchorValues = value;
        return;
    }
    float span = frame - anchorFrame;
    bool outside = false;
    for (std::size_t i = 0; i < components && !outside; ++i) {
        const float slope = (value[i] - anchorValues[i]) / span;
        outside = slope < low[i] || slope > high[i];
    }
    if (outside) {
        keep(endFrame, endValues);
        anchorFrame = endFrame;
        anchorValues = endValues;
        low.fill(-std::numeric_limits<float>::infinity());
        high.fill(std::numeric_limits<float>::infinity());
        span = frame - anchorFrame;
    }
    endFrame = frame;
    endValues = value;
    for (std::size_t i = 0; i < components; ++i) {
        low[i] = std::max(low[i], (value[i] - tolerance - anchorValues[i]) / span);
        high[i] = std::min(high[i], (value[i] + tolerance - anchorValues[i]) / span);
    }
}

bool KeyReducer::within(const std::span<const float> values) const {
    for (std::size_t i = 0; i < components; ++i)
        if (maximum[i] - values[i] > tolerance || values[i] - minimum[i] > tolerance)
            return false;
    return true;
}

Keys KeyReducer::takeKeys() {
    if (count > 1 && within(std::span(kept.values).first(components)))
        kept.frames.resize(1);
    else if (count > 1)
        keep(endFrame, endValues);
    kept.values.resize(kept.frames.size() * components);
    count = 0;
    Keys result = std::move(kept);
    kept = {.components = result.components};
    return result;
}

BoneKeys::BoneKeys(const ue::USkeletalMesh& mesh, const KeyReduction reduction) {
    const float budget = positionBudget(reduction);
    const ue::FReferenceSkeleton& skeleton = mesh.GetRefSkeleton();
    const ue::TArray<ue::FMeshBoneInfo>& bones = skeleton.GetRefBoneInfo();
    const ue::TArray<ue::FTransform>& binds = skeleton.GetRefBonePose();
    const std::size_t boneCount = static_cast<std::size_t>(bones.Num());
    std::vector<Matrix> poses;
    poses.reserve(boneCount);
    for (std::size_t i = 0; i < boneCount; ++i) {
        const int parent = bones[static_cast<int>(i)].ParentIndex;
        const Matrix local = matrixOf(binds[static_cast<int>(i)]);
        poses.push_back(parent < 0 ? local : poses[static_cast<std::size_t>(parent)] * local);
    }
    std::vector<unsigned char> shows(boneCount, 0);
    if (const ue::FSkeletalMeshRenderData* render = mesh.GetResourceForRendering())
        if (render->LODRenderData.Num() > 0)
            for (const ue::FSkelMeshRenderSection& section : render->LODRenderData[0].RenderSections)
                for (const ue::FBoneIndexType bone : section.BoneMap)
                    if (bone < boneCount)
                        shows[bone] = 1;
    std::vector<float> levers(boneCount, 0.0f);
    for (std::size_t i = boneCount; i-- > 0;) {
        const int parent = bones[static_cast<int>(i)].ParentIndex;
        if (parent < 0 || !shows[i])
            continue;
        const auto parentIndex = static_cast<std::size_t>(parent);
        const Vector3 head = poses[i].translation();
        const Vector3 parentHead = poses[parentIndex].translation();
        const float length = std::hypot(head.x - parentHead.x, head.y - parentHead.y, head.z - parentHead.z);
        shows[parentIndex] = 1;
        levers[parentIndex] = std::max(levers[parentIndex], levers[i] + length);
    }
    for (std::size_t i = 0; i < boneCount; ++i) {
        if (!shows[i])
            continue;
        const ue::FTransform& bind = binds[static_cast<int>(i)];
        const ue::FVector& t = bind.GetTranslation();
        const ue::FQuat& r = bind.GetRotation();
        const ue::FVector& s = bind.GetScale3D();
        const float lever = std::max(levers[i], ownVertexReach);
        recorded.push_back({.bone = i,
                            .name = bones[static_cast<int>(i)].Name.toString(),
                            .location = {static_cast<float>(t.X), static_cast<float>(t.Y), static_cast<float>(t.Z)},
                            .rotation = {static_cast<float>(r.X), static_cast<float>(r.Y), static_cast<float>(r.Z), static_cast<float>(r.W)},
                            .scale = {static_cast<float>(s.X), static_cast<float>(s.Y), static_cast<float>(s.Z)},
                            .locations = KeyReducer(3, budget),
                            .rotations = KeyReducer(4, budget / (2 * lever)),
                            .scales = KeyReducer(3, budget / lever)});
    }
}

void BoneKeys::add(const float frame, const std::span<const ue::FTransform> pose, const std::span<const unsigned char> hiddenBones) {
    for (Recorded& bone : recorded) {
        const ue::FTransform& transform = pose[bone.bone];
        const ue::FQuat& r = transform.GetRotation();
        const ue::FVector& t = transform.GetTranslation();
        const ue::FVector& s = !hiddenBones.empty() && hiddenBones[bone.bone] ? ue::FVector::ZeroVector : transform.GetScale3D();
        std::array<float, 4> rotation{static_cast<float>(r.X), static_cast<float>(r.Y), static_cast<float>(r.Z), static_cast<float>(r.W)};
        const std::span<const float> previous = bone.rotations.empty() ? std::span<const float>(bone.rotation) : bone.rotations.last();
        if (rotation[0] * previous[0] + rotation[1] * previous[1] + rotation[2] * previous[2] + rotation[3] * previous[3] < 0)
            for (float& component : rotation)
                component = -component;
        const std::array<float, 3> location{static_cast<float>(t.X), static_cast<float>(t.Y), static_cast<float>(t.Z)};
        const std::array<float, 3> scale{static_cast<float>(s.X), static_cast<float>(s.Y), static_cast<float>(s.Z)};
        bone.locations.add(frame, location);
        bone.rotations.add(frame, rotation);
        bone.scales.add(frame, scale);
    }
}

void BoneKeys::takeChannels(Track& track) {
    for (Recorded& bone : recorded) {
        if (bone.locations.empty())
            continue;
        const std::array<float, 4> flipped{-bone.rotation[0], -bone.rotation[1], -bone.rotation[2], -bone.rotation[3]};
        if (!bone.locations.within(bone.location))
            track.add(ChannelProperty::bone(bone.name, ChannelProperty::location), bone.locations.takeKeys());
        if (!bone.rotations.within(bone.rotation) && !bone.rotations.within(flipped))
            track.add(ChannelProperty::bone(bone.name, ChannelProperty::rotation), bone.rotations.takeKeys());
        if (!bone.scales.within(bone.scale))
            track.add(ChannelProperty::bone(bone.name, ChannelProperty::scale), bone.scales.takeKeys());
    }
}

}
