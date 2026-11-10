#pragma once

#include <array>
#include <cstddef>
#include <limits>
#include <span>
#include <string>
#include <vector>

#include "Engine/Source/Runtime/Engine/Engine/SkeletalMesh.h"
#include "UnrealKit/Scene/KeyReduction.h"
#include "UnrealKit/Scene/Records.h"

namespace ue = ue5parse::runtime;

namespace UnrealKit::Scene {

class KeyReducer {
public:
    static constexpr std::size_t maxComponents = 4;

    KeyReducer(std::size_t components, float tolerance);

    void add(float frame, std::span<const float> values);
    bool empty() const { return count == 0; }
    std::span<const float> last() const { return {lastValues.data(), components}; }
    bool within(std::span<const float> values) const;
    Keys takeKeys();

private:
    using Values = std::array<float, maxComponents>;

    void keep(float frame, const Values& values);

    std::size_t components;
    float tolerance;
    Keys kept;
    std::size_t count = 0;
    float anchorFrame = 0, endFrame = 0;
    Values anchorValues{}, endValues{}, lastValues{};
    Values low{}, high{}, minimum{}, maximum{};
};

class BoneKeys {
public:
    BoneKeys(const ue::USkeletalMesh& mesh, KeyReduction reduction);

    void add(float frame, std::span<const ue::FTransform> pose, std::span<const unsigned char> hiddenBones);
    void takeChannels(Track& track);

private:
    struct Recorded {
        std::size_t bone;
        std::string name;
        std::array<float, 3> location;
        std::array<float, 4> rotation;
        std::array<float, 3> scale;
        KeyReducer locations, rotations, scales;
    };

    std::vector<Recorded> recorded;
};

}
