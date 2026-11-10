#include "UnrealKit/Scene/Records.h"

#include <algorithm>
#include <array>
#include <format>
#include <string_view>

namespace UnrealKit::Scene {

namespace {

using Builder = flatbuffers::FlatBufferBuilder;
using PropertyVector = flatbuffers::Offset<flatbuffers::Vector<flatbuffers::Offset<scene::stream::Property>>>;

scene::stream::Severity severityOf(const Core::Severity severity) {
    switch (severity) {
    case Core::Severity::Info:
        return scene::stream::Severity::Info;
    case Core::Severity::Warning:
        return scene::stream::Severity::Warning;
    case Core::Severity::Error:
        return scene::stream::Severity::Error;
    }
    std::unreachable();
}

Core::Severity severityOf(const scene::stream::Severity severity) {
    switch (severity) {
    case scene::stream::Severity::Info:
        return Core::Severity::Info;
    case scene::stream::Severity::Warning:
        return Core::Severity::Warning;
    case scene::stream::Severity::Error:
        return Core::Severity::Error;
    }
    std::unreachable();
}

flatbuffers::Offset<scene::stream::Diagnostic> diagnosticRecord(Builder& builder, const Core::Diagnostic& diagnostic) {
    return scene::stream::CreateDiagnosticDirect(builder, severityOf(diagnostic.severity), diagnostic.code.c_str(), diagnostic.message.c_str(),
                                                 diagnostic.subject.c_str());
}

PropertyVector properties(Builder& builder, const std::vector<Property>& values) {
    if (values.empty())
        return 0;
    std::vector<flatbuffers::Offset<scene::stream::Property>> offsets;
    offsets.reserve(values.size());
    for (const Property& property : values)
        offsets.push_back(scene::stream::CreatePropertyDirect(builder, property.name.c_str(), property.value.c_str()));
    return builder.CreateVector(offsets);
}

flatbuffers::Offset<scene::stream::Keys> keys(Builder& builder, const Keys& keys) {
    if (keys.frames.empty())
        return 0;
    return scene::stream::CreateKeysDirect(builder, keys.components, &keys.frames, &keys.values, keys.loop.start, keys.loop.frames);
}

scene::stream::ObjectKind kindOf(const ObjectKind kind) {
    switch (kind) {
    case ObjectKind::Empty:
        return scene::stream::ObjectKind::Empty;
    case ObjectKind::Mesh:
        return scene::stream::ObjectKind::Mesh;
    case ObjectKind::Light:
        return scene::stream::ObjectKind::Light;
    case ObjectKind::Camera:
        return scene::stream::ObjectKind::Camera;
    case ObjectKind::Instances:
        return scene::stream::ObjectKind::Instances;
    }
    std::unreachable();
}

scene::stream::ViewerVisibility visibilityOf(const ViewerVisibility visibility) {
    switch (visibility) {
    case ViewerVisibility::Everyone:
        return scene::stream::ViewerVisibility::Everyone;
    case ViewerVisibility::OnlyViewer:
        return scene::stream::ViewerVisibility::OnlyViewer;
    case ViewerVisibility::NotViewer:
        return scene::stream::ViewerVisibility::NotViewer;
    }
    std::unreachable();
}

scene::stream::LightType lightTypeOf(const LightType type) {
    switch (type) {
    case LightType::Point:
        return scene::stream::LightType::Point;
    case LightType::Spot:
        return scene::stream::LightType::Spot;
    case LightType::Rect:
        return scene::stream::LightType::Rect;
    case LightType::Directional:
        return scene::stream::LightType::Directional;
    case LightType::Sky:
        return scene::stream::LightType::Sky;
    }
    std::unreachable();
}

}

std::string_view datablockName(const std::string_view objectPath) {
    return objectPath.substr(objectPath.rfind('.') + 1);
}

Frame hello(const std::string_view productVersion) {
    Builder builder(64);
    const auto version = builder.CreateString(productVersion.data(), productVersion.size());
    return finish(builder, scene::stream::Record::Hello, scene::stream::CreateHello(builder, scene::stream::Version::Current, version));
}

Frame begin(const std::string_view exportId, const std::string_view title) {
    Builder builder(256);
    const auto id = builder.CreateString(exportId);
    const auto text = builder.CreateString(title);
    return finish(builder, scene::stream::Record::Begin, scene::stream::CreateBegin(builder, id, text));
}

Frame complete(const std::span<const Core::Diagnostic> diagnostics) {
    Builder builder(1024);
    std::vector<flatbuffers::Offset<scene::stream::Diagnostic>> offsets;
    offsets.reserve(diagnostics.size());
    for (const Core::Diagnostic& diagnostic : diagnostics)
        offsets.push_back(diagnosticRecord(builder, diagnostic));
    return finish(builder, scene::stream::Record::Complete, scene::stream::CreateCompleteDirect(builder, &offsets));
}

Frame failed(const Core::Diagnostic& diagnostic) {
    Builder builder(256);
    return finish(builder, scene::stream::Record::Failed, scene::stream::CreateFailed(builder, diagnosticRecord(builder, diagnostic)));
}

std::expected<Frame, Core::Error> image(const std::string_view key, const std::string_view name, const DdsImage& file, const bool srgb) {
    Builder builder(file.getSize() + 1024);
    std::uint8_t* bytes = nullptr;
    const auto data = builder.CreateUninitializedVector(file.getSize(), &bytes);
    if (std::expected<void, Core::Error> written = file.write({bytes, file.getSize()}); !written)
        return std::unexpected(written.error());
    const auto keyText = builder.CreateString(key);
    const auto nameText = builder.CreateString(name);
    return finish(builder, scene::stream::Record::Image, scene::stream::CreateImage(builder, keyText, nameText, data, srgb));
}

namespace {

flatbuffers::Offset<scene::stream::MaterialProgram> materialProgram(Builder& builder, const Materials::MaterialProgram& program) {
    std::vector<flatbuffers::Offset<scene::stream::MaterialParameter>> parameters;
    parameters.reserve(program.parameters.size());
    for (const auto& [name, value] : program.parameters)
        parameters.push_back(scene::stream::CreateMaterialParameter(builder, builder.CreateString(name), builder.CreateVector(value)));
    std::vector<flatbuffers::Offset<scene::stream::MaterialTexture>> textures;
    textures.reserve(program.textures.size());
    for (const auto& [image, name, addressU, addressV, nearest] : program.textures)
        textures.push_back(scene::stream::CreateMaterialTexture(builder, builder.CreateString(image), name.empty() ? 0 : builder.CreateString(name), addressU,
                                                                addressV, nearest));
    return scene::stream::CreateMaterialProgram(builder, builder.CreateVectorOfStructs(program.instructions), builder.CreateVector(program.constants),
                                                builder.CreateVector(parameters), builder.CreateVector(textures), program.baseColor, program.normal,
                                                program.emission, program.roughness, program.metallic, program.specular, program.opacity, program.displacement,
                                                program.refraction);
}

}

Materials::MaterialProgram readProgram(const scene::stream::MaterialProgram& program) {
    const auto* instructions = reinterpret_cast<const scene::stream::MaterialInstruction*>(program.instructions()->Data());
    Materials::MaterialProgram result{.instructions = {instructions, instructions + program.instructions()->size()},
                                      .constants = {},
                                      .parameters = {},
                                      .textures = {},
                                      .baseColor = program.base_color(),
                                      .normal = program.normal(),
                                      .emission = program.emission(),
                                      .roughness = program.roughness(),
                                      .metallic = program.metallic(),
                                      .specular = program.specular(),
                                      .opacity = program.opacity(),
                                      .refraction = program.refraction(),
                                      .displacement = program.displacement()};
    if (program.constants())
        result.constants.assign(program.constants()->begin(), program.constants()->end());
    if (program.parameters())
        for (const scene::stream::MaterialParameter* parameter : *program.parameters())
            result.parameters.push_back({parameter->name()->str(), {parameter->value()->begin(), parameter->value()->end()}});
    if (program.textures())
        for (const scene::stream::MaterialTexture* texture : *program.textures())
            result.textures.push_back({texture->image()->str(), texture->name() ? texture->name()->str() : std::string(), texture->address_u(),
                                       texture->address_v(), texture->nearest()});
    return result;
}

Frame material(const SceneMaterial& material) {
    const Materials::MaterialProgram& program = material.program;
    Builder builder(program.instructions.size() * sizeof(scene::stream::MaterialInstruction) + program.constants.size() * sizeof(float) + 4096);
    const auto key = builder.CreateString(material.key);
    const auto name = builder.CreateString(material.name);
    const auto chain = builder.CreateVectorOfStrings(material.chain);
    const auto blendMode = material.blendMode.empty() ? 0 : builder.CreateString(material.blendMode);
    const auto encoded = materialProgram(builder, program);
    return finish(builder, scene::stream::Record::Material,
                  scene::stream::CreateMaterial(builder, key, name, chain, blendMode, material.twoSided, encoded, material.unlit, material.sortPriority,
                                                material.disableDepthTest, material.objectTranslucency));
}

Frame light(const SceneLight& light) {
    Builder builder(256);
    const auto key = builder.CreateString(light.key);
    const auto name = builder.CreateString(light.name);
    const auto color = builder.CreateVector(light.color.data(), light.color.size());
    const bool sky = light.type == LightType::Sky;
    const auto image = sky ? builder.CreateString(light.image) : flatbuffers::Offset<flatbuffers::String>();
    const auto lowerHemisphereColor =
        sky ? builder.CreateVector(light.lowerHemisphereColor.data(), light.lowerHemisphereColor.size()) : flatbuffers::Offset<flatbuffers::Vector<float>>();
    return finish(builder, scene::stream::Record::Light,
                  scene::stream::CreateLight(builder, key, name, lightTypeOf(light.type), color, light.intensity, light.sourceRadius, light.innerConeAngle,
                                             light.outerConeAngle, light.sourceWidth, light.sourceHeight, image, light.sourceCubemapAngle,
                                             light.lowerHemisphereIsBlack, lowerHemisphereColor, light.indirectLightingIntensity, light.sourceAngle,
                                             light.attenuationRadius, light.inverseSquaredFalloff, light.lightFalloffExponent));
}

Frame camera(const SceneCamera& camera) {
    Builder builder(256);
    const auto key = builder.CreateString(camera.key);
    const auto name = builder.CreateString(camera.name);
    return finish(builder, scene::stream::Record::Camera, scene::stream::CreateCamera(builder, key, name, camera.fieldOfView, camera.nearClip));
}

Frame instances(const SceneInstances& instances) {
    std::size_t floats = instances.locations.size() + instances.rotations.size() + instances.scales.size();
    for (const InstanceParameter& parameter : instances.parameters)
        floats += parameter.values.size();
    Builder builder(1024 + 4 * (floats + 2 * instances.firstFrames.size()));
    const auto key = builder.CreateString(instances.key);
    const auto mesh = builder.CreateString(instances.mesh);
    const auto firstFrames = builder.CreateVector(instances.firstFrames);
    const auto frameCounts = builder.CreateVector(instances.frameCounts);
    const auto locations = builder.CreateVector(instances.locations);
    const auto rotations = builder.CreateVector(instances.rotations);
    const auto scales = builder.CreateVector(instances.scales);
    std::vector<flatbuffers::Offset<scene::stream::InstanceParameter>> parameters;
    parameters.reserve(instances.parameters.size());
    for (const InstanceParameter& parameter : instances.parameters)
        parameters.push_back(scene::stream::CreateInstanceParameter(builder, builder.CreateString(parameter.name), parameter.components,
                                                                    builder.CreateVector(parameter.values)));
    const auto parameterVector = builder.CreateVector(parameters);
    return finish(builder, scene::stream::Record::Instances,
                  scene::stream::CreateInstances(builder, key, mesh, instances.facing, firstFrames, frameCounts, locations, rotations, scales, parameterVector,
                                                 instances.loopStart, instances.loopFrames));
}

Frame collection(const SceneCollection& collection) {
    Builder builder(512);
    const auto name = builder.CreateString(collection.name);
    const auto values = properties(builder, collection.properties);
    return finish(builder, scene::stream::Record::Collection, scene::stream::CreateCollection(builder, collection.id, name, collection.parent, values));
}

Frame object(const SceneObject& object) {
    Builder builder(1024);
    const auto name = builder.CreateString(object.name);
    const auto parentBone = object.parentBone.empty() ? 0 : builder.CreateString(object.parentBone);
    const auto matrix = builder.CreateVector(object.matrix.rows().data(), object.matrix.rows().size());
    const auto data = object.data.empty() ? 0 : builder.CreateString(object.data);
    const auto materials = object.materials.empty() ? 0 : builder.CreateVectorOfStrings(object.materials);
    const auto values = properties(builder, object.properties);
    const auto animation = object.animation.empty() ? 0 : builder.CreateString(object.animation);
    const auto track = object.track.empty() ? 0 : builder.CreateString(object.track);
    return finish(builder, scene::stream::Record::Object,
                  scene::stream::CreateObject(builder, object.id, name, object.collection, object.parent, parentBone, matrix, kindOf(object.kind), data,
                                              object.posed, materials, object.visible, values, animation, track, object.viewer,
                                              visibilityOf(object.viewerVisibility), object.castShadow, object.sortPriority, object.reverseCulling));
}

Keys visibilityKeys(const std::span<const float> keys) {
    Keys result{.components = 1};
    for (std::size_t i = 0; i + 1 < keys.size(); i += 2) {
        result.frames.push_back(keys[i]);
        result.values.push_back(keys[i + 1]);
    }
    return result;
}

std::string ChannelProperty::bone(const std::string_view bone, const std::string_view attribute) {
    return std::format("bones/{}/{}", bone, attribute);
}

std::string ChannelProperty::materialParameter(const std::size_t slot, const std::string_view parameter) {
    return std::format("materials/{}/{}", slot, parameter);
}

void Track::add(std::string property, Keys keys) {
    if (!keys.frames.empty())
        channels.push_back({std::move(property), std::move(keys)});
}

const Keys* Track::find(const std::string_view property) const {
    const auto found = std::ranges::find(channels, property, &Channel::property);
    return found == channels.end() ? nullptr : &found->keys;
}

Frame animation(const Animation& animation) {
    std::size_t size = 1024;
    for (const Track& track : animation.tracks) {
        size += track.name.size() + 64;
        for (const Channel& channel : track.channels)
            size += (channel.keys.frames.size() + channel.keys.values.size()) * sizeof(float) + channel.property.size() + 64;
    }
    Builder builder(size);
    std::vector<flatbuffers::Offset<scene::stream::Track>> tracks;
    tracks.reserve(animation.tracks.size());
    std::vector<flatbuffers::Offset<scene::stream::Channel>> channels;
    for (const Track& track : animation.tracks) {
        channels.clear();
        for (const Channel& channel : track.channels) {
            const auto property = builder.CreateString(channel.property);
            channels.push_back(scene::stream::CreateChannel(builder, property, keys(builder, channel.keys)));
        }
        const auto trackName = builder.CreateString(track.name);
        tracks.push_back(scene::stream::CreateTrack(builder, trackName, builder.CreateVector(channels)));
    }
    const auto key = builder.CreateString(animation.key);
    const auto name = builder.CreateString(animation.name);
    return finish(builder, scene::stream::Record::Animation, scene::stream::CreateAnimation(builder, key, name, builder.CreateVector(tracks)));
}

Frame sceneRange(const std::uint32_t framesPerSecond, const int frameStart, const int frameEnd) {
    Builder builder(64);
    return finish(builder, scene::stream::Record::Scene, scene::stream::CreateScene(builder, framesPerSecond, frameStart, frameEnd));
}

namespace {

bool hasKnownSeverity(const scene::stream::Diagnostic* record) {
    return record == nullptr || (record->severity() >= scene::stream::Severity::MIN && record->severity() <= scene::stream::Severity::MAX);
}

}

const scene::stream::Envelope* verified(const std::span<const std::uint8_t> bytes) {
    flatbuffers::Verifier verifier(bytes.data(), bytes.size());
    if (bytes.size() < 8 || !scene::stream::EnvelopeBufferHasIdentifier(bytes.data()) || !scene::stream::VerifyEnvelopeBuffer(verifier))
        return nullptr;
    const scene::stream::Envelope* envelope = scene::stream::GetEnvelope(bytes.data());
    if (const auto* failed = envelope->record_as_Failed(); failed != nullptr && !hasKnownSeverity(failed->diagnostic()))
        return nullptr;
    if (const auto* finished = envelope->record_as_Finished(); finished != nullptr && finished->diagnostics() != nullptr)
        for (const scene::stream::Diagnostic* record : *finished->diagnostics())
            if (!hasKnownSeverity(record))
                return nullptr;
    return envelope;
}

std::vector<Core::Diagnostic> diagnostics(const flatbuffers::Vector<flatbuffers::Offset<scene::stream::Diagnostic>>* records) {
    std::vector<Core::Diagnostic> result;
    if (!records)
        return result;
    result.reserve(records->size());
    for (const scene::stream::Diagnostic* record : *records)
        result.push_back(diagnostic(*record));
    return result;
}

Core::Diagnostic diagnostic(const scene::stream::Diagnostic& record) {
    return {.severity = severityOf(record.severity()),
            .code = record.code() ? record.code()->str() : "",
            .message = record.message() ? record.message()->str() : "",
            .subject = record.subject() ? record.subject()->str() : ""};
}

}
