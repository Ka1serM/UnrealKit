#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <flatbuffers/flatbuffers.h>

#include "Conversion/Materials/MaterialProgram.h"
#include "UnrealKit/Core/Diagnostic.h"
#include "UnrealKit/Scene/DdsImage.h"
#include "UnrealKit/Scene/Matrix.h"
#include "scene_generated.h"

namespace UnrealKit::Scene {

using Frame = flatbuffers::DetachedBuffer;

struct Property {
    std::string name;
    std::string value;
};

struct SceneMaterial {
    std::string key;
    std::string name;
    std::vector<std::string> chain;
    std::string blendMode;
    bool twoSided = false;
    ue5parse::conversion::MaterialProgram program;
    bool unlit = false;
    int sortPriority = 0;
    bool disableDepthTest = false;
    bool objectTranslucency = false;
};

enum class LightType { Point, Spot, Rect, Directional, Sky };

struct SceneLight {
    std::string key;
    std::string name;
    LightType type = LightType::Point;
    std::array<float, 3> color{1, 1, 1};
    float intensity = 0;
    float sourceRadius = 0;
    float innerConeAngle = 0;
    float outerConeAngle = 0;
    float sourceWidth = 0;
    float sourceHeight = 0;
    float sourceAngle = 0;
    float attenuationRadius = 0;
    bool inverseSquaredFalloff = true;
    float lightFalloffExponent = 0;
    std::string image;
    float sourceCubemapAngle = 0;
    bool lowerHemisphereIsBlack = false;
    std::array<float, 4> lowerHemisphereColor{0, 0, 0, 1};
    float indirectLightingIntensity = 1;
};

struct SceneCamera {
    std::string key;
    std::string name;
    float fieldOfView = 90;
    float nearClip = 1;
};

struct InstanceParameter {
    std::string name;
    std::uint8_t components = 1;
    std::vector<float> values;
};

struct SceneInstances {
    std::string key;
    std::string mesh;
    scene::stream::Facing facing = scene::stream::Facing::None;
    std::vector<int> firstFrames;
    std::vector<std::uint32_t> frameCounts;
    std::vector<float> locations;
    std::vector<float> rotations;
    std::vector<float> scales;
    std::vector<InstanceParameter> parameters;
    int loopStart = 0;
    std::uint32_t loopFrames = 0;
};

struct Loop {
    float start = 0;
    float frames = 0;
};

struct Keys {
    std::uint8_t components = 0;
    std::vector<float> frames;
    std::vector<float> values;
    Loop loop;
};

namespace ChannelProperty {
inline constexpr std::string_view location = "location";
inline constexpr std::string_view rotation = "rotation";
inline constexpr std::string_view scale = "scale";
inline constexpr std::string_view visibility = "visibility";
inline constexpr std::string_view fieldOfView = "field_of_view";
std::string bone(std::string_view bone, std::string_view attribute);
std::string materialParameter(std::size_t slot, std::string_view parameter);
}

struct Channel {
    std::string property;
    Keys keys;
};

struct Track {
    std::string name;
    std::vector<Channel> channels;

    void add(std::string property, Keys keys);
    const Keys* find(std::string_view property) const;
};

struct Animation {
    std::string key;
    std::string name;
    std::vector<Track> tracks;
};

struct SceneCollection {
    std::uint32_t id = 0;
    std::string name;
    int parent = -1;
    std::vector<Property> properties;
};

enum class ObjectKind { Empty, Mesh, Light, Camera, Instances };

enum class ViewerVisibility { Everyone, OnlyViewer, NotViewer };

struct SceneObject {
    std::uint32_t id = 0;
    std::string name;
    std::uint32_t collection = 0;
    int parent = -1;
    std::string parentBone;
    Matrix matrix = Matrix::identity();
    ObjectKind kind = ObjectKind::Empty;
    std::string data;
    bool posed = false;
    std::vector<std::string> materials;
    bool visible = true;
    bool castShadow = true;
    int sortPriority = 0;
    bool reverseCulling = false;
    std::vector<Property> properties;
    std::string animation;
    std::string track;
    int viewer = -1;
    ViewerVisibility viewerVisibility = ViewerVisibility::Everyone;
};

Keys visibilityKeys(std::span<const float> keys);

std::string_view datablockName(std::string_view objectPath);

Frame hello(std::string_view productVersion);
Frame begin(std::string_view exportId, std::string_view title);
Frame complete(std::span<const Core::Diagnostic> diagnostics);
Frame failed(const Core::Diagnostic& diagnostic);
std::expected<Frame, Core::Error> image(std::string_view key, std::string_view name, const DdsImage& file, bool srgb);
Frame material(const SceneMaterial& material);
ue5parse::conversion::MaterialProgram readProgram(const scene::stream::MaterialProgram& program);
Frame light(const SceneLight& light);
Frame camera(const SceneCamera& camera);
Frame instances(const SceneInstances& instances);
Frame collection(const SceneCollection& collection);
Frame object(const SceneObject& object);
Frame animation(const Animation& animation);
Frame sceneRange(std::uint32_t framesPerSecond, int frameStart, int frameEnd);

template <class T>
Frame finish(flatbuffers::FlatBufferBuilder& builder, const scene::stream::Record type, const flatbuffers::Offset<T> record) {
    scene::stream::FinishEnvelopeBuffer(builder, scene::stream::CreateEnvelope(builder, type, record.Union()));
    return builder.Release();
}

const scene::stream::Envelope* verified(std::span<const std::uint8_t> bytes);

std::vector<Core::Diagnostic> diagnostics(const flatbuffers::Vector<flatbuffers::Offset<scene::stream::Diagnostic>>* records);
Core::Diagnostic diagnostic(const scene::stream::Diagnostic& record);

}
