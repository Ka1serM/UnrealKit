#include "UnrealKit/Export/SceneExport.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <utility>

#include <tbb/parallel_for.h>

#include "UnrealKit/Assets/Reading.h"
#include "UnrealKit/Core/Error.h"
#include "UnrealKit/Core/Log.h"
#include "Engine/Source/Runtime/Engine/Materials/MaterialInterface.h"
#include "UnrealKit/Scene/DdsImage.h"
#include "UnrealKit/Scene/MeshRecord.h"
#include "UnrealKit/Scene/Records.h"
#include "UnrealKit/Scene/Texture.h"
#include "UnrealKit/Export/ResolvedMaterial.h"

namespace UnrealKit::Export {

namespace {

void warn(Diagnostics& diagnostics, const char* code, const std::string& subject, const Core::Error& error) {
    if (error.isCancelled())
        return;
    std::string message = std::format("{} ({})", error.message, error.code);
    Core::logWarning("{}: {} ({})", code, message, subject);
    diagnostics.add({.severity = Core::Severity::Warning, .code = code, .message = std::move(message), .subject = subject});
}

struct MaterialRequest {
    std::string key;
    MaterialOverride material;
    Materials::VertexFactory use;
};

std::expected<const ue::UMaterialInterface*, Core::Error> materialOf(const MaterialOverride& material, const std::stop_token stop) {
    if (material.material != nullptr)
        return material.material;
    return Assets::Reading::loadObject<ue::UMaterialInterface>(material.path, "material", stop).transform([](ue::UMaterialInterface* loaded) {
        return static_cast<const ue::UMaterialInterface*>(loaded);
    });
}

std::string materialKey(const std::string& path, const Materials::VertexFactory use) {
    switch (use) {
    case Materials::VertexFactory::Mesh:
        return path;
    case Materials::VertexFactory::ParticleSprite:
        return path + ":particle sprite";
    case Materials::VertexFactory::ParticleMesh:
        return path + ":particle mesh";
    }
    std::unreachable();
}

class FirstFailure {
public:
    void keep(const Core::Error& error) {
        std::lock_guard lock(mutex);
        if (!failure)
            failure = error;
    }

    std::expected<void, Core::Error> get() {
        std::lock_guard lock(mutex);
        if (failure)
            return std::unexpected(*failure);
        return {};
    }

private:
    std::mutex mutex;
    std::optional<Core::Error> failure;
};

std::string joined(const std::vector<std::string>& values) {
    std::string text;
    for (const std::string& value : values)
        text += (text.empty() ? "" : ", ") + value;
    return text;
}

}

void Diagnostics::add(Core::Diagnostic diagnostic) {
    std::lock_guard lock(mutex);
    items.push_back(std::move(diagnostic));
}

void Diagnostics::add(std::vector<Core::Diagnostic> diagnostics) {
    std::lock_guard lock(mutex);
    for (Core::Diagnostic& diagnostic : diagnostics)
        items.push_back(std::move(diagnostic));
}

std::vector<Core::Diagnostic> Diagnostics::take() {
    std::lock_guard lock(mutex);
    return std::move(items);
}

ProgressMeter::ProgressMeter(ExportProgress report, const ExportStage stage) : report(std::move(report)), stage(stage) {
    if (this->report)
        this->report(stage, 0);
}

void ProgressMeter::record(const float fraction) {
    std::lock_guard lock(mutex);
    if (fraction < 1 && fraction - recordedShown < 0.005f)
        return;
    recordedShown = fraction;
    if (report)
        report(ExportStage::Recording, fraction);
    if (fraction < 1)
        return;
    stage = ExportStage::Sending;
    update();
}

void ProgressMeter::addRecords(const std::size_t records) {
    std::lock_guard lock(mutex);
    total += records;
    update();
}

void ProgressMeter::sent() {
    std::lock_guard lock(mutex);
    ++sentCount;
    update();
}

void ProgressMeter::built(const std::uint32_t done) {
    std::lock_guard lock(mutex);
    builtCount = std::max<std::size_t>(builtCount, done);
    update();
}

void ProgressMeter::finish() {
    std::lock_guard lock(mutex);
    shown = 1;
    if (report)
        report(ExportStage::Sending, 1);
}

void ProgressMeter::update() {
    if (!report || stage != ExportStage::Sending || total == 0)
        return;
    const float records = static_cast<float>(total + 1);
    const float sentPart = static_cast<float>(std::min(sentCount, total)) / records;
    const float builtPart = static_cast<float>(std::min(builtCount, total)) / records;
    const float fraction = std::max(shown, 0.1f + 0.5f * sentPart + 0.4f * builtPart);
    if (fraction - shown < 0.005f && fraction < 1)
        return;
    shown = fraction;
    report(ExportStage::Sending, fraction);
}

SceneSender::SceneSender(const Assets::Provider& provider, const SceneOptions options, Transport::RecordSink& sink, std::stop_token stop,
                         ExportProgress progress)
    : provider(provider), options(options), stop(std::move(stop)),
      meter(std::move(progress), options.recording ? ExportStage::Recording : ExportStage::Sending), sink(sink) {}

std::expected<void, Core::Error> SceneSender::open(const std::string_view exportId, const std::string_view title) {
    if (auto opened = sink.open([this](const std::uint32_t done, std::uint32_t) { meter.built(done); }); !opened)
        return opened;
    if (auto begun = sink.send(Scene::begin(exportId, title)); !begun)
        return begun;
    meter.addRecords(1);
    return sendRecord(Scene::collection({.id = 0, .name = std::string(title), .parent = -1, .properties = {{"verylia.export_id", std::string(exportId)}}}));
}

std::expected<std::uint32_t, Core::Error> SceneSender::collection(const std::string& name, const std::uint32_t parent) {
    const auto [found, added] = collections.try_emplace({parent, name}, static_cast<std::uint32_t>(collections.size() + 1));
    if (!added)
        return found->second;
    meter.addRecords(1);
    if (auto sent = sendRecord(Scene::collection({.id = found->second, .name = name, .parent = static_cast<int>(parent)})); !sent)
        return std::unexpected(sent.error());
    return found->second;
}

void SceneSender::recorded(const float fraction) {
    meter.record(fraction);
}

std::expected<void, Core::Error> SceneSender::sendSceneRange(const std::uint32_t framesPerSecond, const int frameStart, const int frameEnd) {
    meter.addRecords(1);
    return sendRecord(Scene::sceneRange(framesPerSecond, frameStart, frameEnd));
}

std::expected<void, Core::Error> SceneSender::sendAnimation(const Scene::Animation& animation) {
    meter.addRecords(1);
    return sendRecord(Scene::animation(animation));
}

const Scene::MeshLod* SceneSender::findMesh(const std::string& path) const {
    const auto found = meshes.find(path);
    return found == meshes.end() ? nullptr : &found->second;
}

std::expected<void, Core::Error> SceneSender::sendRecord(Scene::Frame frame) {
    if (auto sent = sink.send(std::move(frame)); !sent)
        return sent;
    meter.sent();
    return {};
}

std::expected<void, Core::Error> SceneSender::sendResources(const ExportBatch& batch) {
    return sendSharedResources(batch).transform([](const auto&) {});
}

std::expected<std::vector<std::vector<std::string>>, Core::Error> SceneSender::sendSharedResources(const ExportBatch& batch) {
    const std::vector<ExportMesh>& parts = batch.meshes;

    std::vector<std::string> newMeshes;
    std::vector<char> required;
    std::unordered_map<std::string, std::size_t> newIndices;
    for (const ExportMesh& part : parts) {
        if (part.meshPath.empty())
            continue;
        if (const auto found = newIndices.find(part.meshPath); found != newIndices.end()) {
            if (part.required)
                required[found->second] = true;
            continue;
        }
        if (!requestedMeshes.insert(part.meshPath).second)
            continue;
        newIndices.emplace(part.meshPath, newMeshes.size());
        newMeshes.push_back(part.meshPath);
        required.push_back(part.required);
    }
    std::vector<std::optional<Scene::MeshLod>> loaded(newMeshes.size());
    FirstFailure requiredFailure;
    tbb::parallel_for(std::size_t{0}, newMeshes.size(), [&](const std::size_t i) {
        const auto read = loaded[i].emplace().load(newMeshes[i], stop);
        if (read)
            return;
        loaded[i].reset();
        if (required[i])
            requiredFailure.keep(read.error());
        else
            warn(diagnostics, "export.mesh_unreadable", newMeshes[i], read.error());
    });
    if (stop.stop_requested())
        return Core::cancelled();
    if (auto failure = requiredFailure.get(); !failure)
        return std::unexpected(failure.error());
    for (std::size_t i = 0; i < newMeshes.size(); ++i)
        if (loaded[i])
            meshes.emplace(newMeshes[i], std::move(*loaded[i]));
    for (const ExportMesh& part : parts) {
        if (part.poseSource.empty() || meshes.contains(part.poseSource) || !requestedPoseSources.insert(part.poseSource).second)
            continue;
        Scene::MeshLod source;
        const auto read = source.load(part.poseSource, stop);
        if (!read) {
            if (read.error().isCancelled())
                return std::unexpected(read.error());
            warn(diagnostics, "export.pose_source_unreadable", part.poseSource, read.error());
            continue;
        }
        meshes.emplace(part.poseSource, std::move(source));
    }

    std::vector<std::vector<std::string>> slotMaterials(parts.size());
    std::vector<MaterialRequest> newMaterials;
    const auto bind = [&](const std::size_t part, const std::size_t slot, MaterialOverride material, const Materials::VertexFactory use) {
        if (material.path.empty()) {
            const std::string subject = std::format("{} slot {}", parts[part].meshPath.empty() ? parts[part].name : parts[part].meshPath, slot);
            if (missingMaterials.insert(subject).second) {
                const std::string message = "The slot has no material.";
                Core::logWarning("export.material_missing: {} ({})", message, subject);
                diagnostics.add({.severity = Core::Severity::Warning, .code = "export.material_missing", .message = message, .subject = subject});
            }
            slotMaterials[part].emplace_back();
            return;
        }
        std::string key = materialKey(material.path, use);
        if (requestedMaterials.insert(key).second)
            newMaterials.push_back({key, std::move(material), use});
        slotMaterials[part].push_back(std::move(key));
    };
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const ExportMesh& part = parts[i];
        if (part.instances && part.meshPath.empty() && !part.overrides.empty()) {
            bind(i, 0, part.overrides.front(), part.particles ? Materials::VertexFactory::ParticleSprite : Materials::VertexFactory::Mesh);
            continue;
        }
        const Scene::MeshLod* mesh = findMesh(part.meshPath);
        if (!mesh)
            continue;
        const std::span<const Scene::MeshMaterial> meshMaterials = mesh->getMaterials();
        for (std::size_t slot = 0; slot < meshMaterials.size(); ++slot)
            bind(i, slot, bindSlot(part, slot, meshMaterials[slot]), part.particles ? Materials::VertexFactory::ParticleMesh : Materials::VertexFactory::Mesh);
    }

    std::vector<std::optional<Scene::SceneMaterial>> sceneMaterials(newMaterials.size());
    std::vector<std::vector<std::string>> programTextures(newMaterials.size());
    tbb::parallel_for(std::size_t{0}, newMaterials.size(), [&](const std::size_t i) {
        const auto& [key, requested, use] = newMaterials[i];
        const std::string& path = requested.path;
        const auto material = materialOf(requested, stop);
        if (!material)
            return warn(diagnostics, "export.material_unreadable", path, material.error());
        auto resolved = resolveMaterial(**material);
        if (!resolved)
            return warn(diagnostics, "export.material_unreadable", path, resolved.error());
        auto translation = provider.getShaderMaterials().translate(**material, use, stop);
        if (!translation)
            return warn(diagnostics, "export.program_unavailable", path, translation.error());
        if (!translation->warnings.empty()) {
            const std::vector<std::string>& warnings = translation->warnings;
            std::string message = std::format("The material program differs from the game in {} places, such as: {}", warnings.size(), warnings.front());
            diagnostics.add({.severity = Core::Severity::Info, .code = "export.program_incomplete", .message = std::move(message), .subject = path});
        }
        programTextures[i] = std::move(translation->textures);
        sceneMaterials[i] = Scene::SceneMaterial{.key = key,
                                                 .name = std::move(resolved->name),
                                                 .chain = std::move(resolved->chain),
                                                 .blendMode = std::move(resolved->blendMode),
                                                 .twoSided = resolved->twoSided,
                                                 .program = std::move(translation->program),
                                                 .unlit = resolved->unlit,
                                                 .sortPriority = resolved->sortPriority,
                                                 .disableDepthTest = resolved->disableDepthTest,
                                                 .objectTranslucency = resolved->objectTranslucency};
    });
    if (stop.stop_requested())
        return Core::cancelled();

    std::vector<std::string> textures;
    const auto request = [&](const std::string& path) {
        if (requestedImages.insert(path).second)
            textures.push_back(path);
    };
    for (const std::vector<std::string>& sampled : programTextures)
        for (const std::string& texture : sampled)
            request(texture);
    for (const std::string& image : batch.images)
        request(image);
    for (const ExportMesh& part : batch.meshes)
        if (part.light && !part.light->image.empty())
            request(part.light->image);

    std::size_t records = textures.size();
    records += std::ranges::count_if(sceneMaterials, [](const auto& material) { return material.has_value(); });
    for (const auto& mesh : loaded)
        records += mesh ? 1 + mesh->isSkeletal() : 0;
    meter.addRecords(records);

    FirstFailure sendFailure;
    tbb::parallel_for(std::size_t{0}, textures.size(), [&](const std::size_t i) {
        const std::string& path = textures[i];
        const auto cube = Scene::isCubeTexture(path, stop);
        if (!cube)
            return warn(diagnostics, "export.texture_unreadable", path, cube.error());
        std::expected<Scene::Frame, Core::Error> frame;
        if (*cube) {
            auto faces = Scene::readCube(path, stop);
            if (!faces)
                return warn(diagnostics, "export.texture_unreadable", path, faces.error());
            Scene::Texture strip{.format = faces->format, .srgb = faces->srgb, .width = faces->size, .height = 6 * faces->size, .data = std::move(faces->data)};
            if (strip.format == Scene::PixelFormat::FloatRGBA)
                strip = Scene::encodeToBc6h(strip);
            frame = Scene::image(path, Scene::datablockName(path), Scene::DdsImage(strip), strip.srgb);
        } else {
            const auto mip = Scene::findTextureMip(provider, path, stop);
            if (!mip)
                return warn(diagnostics, "export.texture_unreadable", path, mip.error());
            if (mip->format == Scene::PixelFormat::FloatRGBA) {
                const auto texture = Scene::readTexture(provider, path, stop);
                if (!texture)
                    return warn(diagnostics, "export.texture_unreadable", path, texture.error());
                const Scene::Texture encoded = Scene::encodeToBc6h(*texture);
                frame = Scene::image(path, Scene::datablockName(path), Scene::DdsImage(encoded), encoded.srgb);
            } else {
                frame = Scene::image(path, Scene::datablockName(path), Scene::DdsImage(*mip, path, stop), mip->srgb);
            }
        }
        if (!frame)
            return warn(diagnostics, "export.texture_unreadable", path, frame.error());
        if (auto sent = sendRecord(std::move(*frame)); !sent)
            return sendFailure.keep(sent.error());
    });
    if (stop.stop_requested())
        return Core::cancelled();
    if (auto failure = sendFailure.get(); !failure)
        return std::unexpected(failure.error());

    for (std::optional<Scene::SceneMaterial>& material : sceneMaterials) {
        if (!material)
            continue;
        if (const auto sent = sendRecord(Scene::material(*material)); !sent)
            return std::unexpected(sent.error());
        sentMaterials.insert(material->key);
    }

    tbb::parallel_for(std::size_t{0}, newMeshes.size(), [&](const std::size_t i) {
        if (!loaded[i])
            return;
        const std::string& path = newMeshes[i];
        const Scene::MeshLod& mesh = *findMesh(path);
        const std::string_view name = Scene::datablockName(path);
        if (mesh.isSkeletal())
            if (auto sent = sendRecord(Scene::Skeleton(mesh, nullptr).record(path, name)); !sent)
                return sendFailure.keep(sent.error());
        if (auto sent = sendRecord(Scene::mesh(path, name, mesh, mesh.isSkeletal() ? std::string_view(path) : std::string_view())); !sent)
            return sendFailure.keep(sent.error());
        std::lock_guard lock(sentMutex);
        sentMeshes.insert(path);
    });
    if (auto failure = sendFailure.get(); !failure)
        return std::unexpected(failure.error());

    return slotMaterials;
}

std::expected<void, Core::Error> SceneSender::send(const ExportBatch& batch) {
    const std::vector<ExportMesh>& parts = batch.meshes;
    const auto shared = sendSharedResources(batch);
    if (!shared)
        return std::unexpected(shared.error());
    const std::vector<std::vector<std::string>>& slotMaterials = *shared;
    std::size_t records = parts.size();
    for (const ExportMesh& part : parts)
        records += !part.colors.empty() + part.light.has_value() + part.camera.has_value() + part.instances.has_value();
    meter.addRecords(records);

    std::unordered_set<std::string_view> failedVariants;
    for (const ExportMesh& part : parts) {
        if (part.colors.empty() || !sentMeshes.contains(part.meshPath))
            continue;
        const Scene::MeshLod& mesh = *findMesh(part.meshPath);
        const std::string_view skeleton = mesh.isSkeletal() ? std::string_view(part.meshPath) : std::string_view();
        auto frame = Scene::mesh(part.meshKey, Scene::datablockName(part.meshPath), mesh, skeleton, part.colors);
        if (!frame) {
            warn(diagnostics, "export.mesh_colors_invalid", part.meshKey, frame.error());
            failedVariants.insert(part.meshKey);
            continue;
        }
        if (auto sent = sendRecord(std::move(*frame)); !sent)
            return sent;
    }

    std::vector<std::optional<Scene::Skeleton>> skeletons(parts.size());
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const ExportMesh& part = parts[i];
        const Scene::MeshLod* mesh = findMesh(part.meshPath);
        if (part.armature && mesh && sentMeshes.contains(part.meshPath) && mesh->isSkeletal())
            skeletons[i].emplace(*mesh, findMesh(part.poseSource));
    }

    for (const ExportMesh& part : parts) {
        if (!part.light)
            continue;
        if (auto sent = sendRecord(Scene::light(*part.light)); !sent)
            return sent;
    }
    for (const ExportMesh& part : parts) {
        if (!part.camera)
            continue;
        if (auto sent = sendRecord(Scene::camera(*part.camera)); !sent)
            return sent;
    }
    for (const ExportMesh& part : parts) {
        if (!part.instances || (!part.meshPath.empty() && (!sentMeshes.contains(part.meshPath) || failedVariants.contains(part.meshKey))))
            continue;
        if (part.meshPath.empty() && sentMeshes.emplace(Scene::quadKey).second) {
            meter.addRecords(1);
            if (auto sent = sendRecord(Scene::quad(Scene::quadKey, "Quad")); !sent)
                return sent;
        }
        if (auto sent = sendRecord(Scene::instances(*part.instances)); !sent)
            return sent;
    }

    std::vector<std::optional<std::uint32_t>> carriers(parts.size());
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const ExportMesh& part = parts[i];
        const bool empty = part.meshPath.empty();
        if (!empty && (!sentMeshes.contains(part.meshPath) || failedVariants.contains(part.meshKey)))
            continue;
        Scene::SceneObject object{.id = nextId++,
                                  .collection = part.collection.value_or(batch.collection),
                                  .matrix = part.matrix,
                                  .posed = skeletons[i].has_value(),
                                  .visible = part.visible,
                                  .castShadow = part.castShadow,
                                  .sortPriority = part.sortPriority,
                                  .reverseCulling = part.reverseCulling,
                                  .animation = part.animation,
                                  .track = part.track};
        object.name = !part.name.empty() ? part.name : empty ? std::string() : std::string(Scene::datablockName(part.meshPath));
        if (!part.viewer.empty()) {
            const auto viewer = cameraObjects.find(part.viewer);
            if (viewer == cameraObjects.end())
                return Core::fail("export.viewer", std::format("The viewer {} of {} was not sent before it.", part.viewer, object.name));
            object.viewer = static_cast<int>(viewer->second);
            object.viewerVisibility = part.viewerVisibility;
        }
        if (part.parent >= 0 && carriers[part.parent]) {
            object.parent = static_cast<int>(*carriers[part.parent]);
            std::optional<Scene::BoneAttachment> attachment;
            for (const std::string& point : part.attachPoints) {
                if (skeletons[part.parent])
                    attachment = skeletons[part.parent]->attachment(point);
                if (attachment)
                    break;
            }
            if (attachment) {
                object.parentBone = attachment->bone;
                object.matrix = attachment->relative * part.matrix;
            } else if (!part.attachPoints.empty() && !skeletons[part.parent]) {
                object.matrix = Scene::Matrix::identity();
                diagnostics.add({.severity = Core::Severity::Warning,
                                 .code = "export.attach_point",
                                 .message = std::format("The parent mesh has no skeleton for the socket {}, so the mesh stays at the "
                                                        "parent's origin.",
                                                        joined(part.attachPoints)),
                                 .subject = part.meshPath});
            }
        }
        if (!part.originPoint.empty()) {
            const std::optional<Scene::Vector3> origin = skeletons[i] ? skeletons[i]->bindPosition(part.originPoint) : std::nullopt;
            if (origin) {
                object.matrix = object.matrix * Scene::Matrix::translation({-origin->x, -origin->y, -origin->z});
            } else {
                diagnostics.add({.severity = Core::Severity::Warning,
                                 .code = "export.origin_point",
                                 .message = std::format("The mesh has no socket or bone {}, so it keeps its origin.", part.originPoint),
                                 .subject = part.meshPath});
            }
        }
        if (part.light) {
            object.kind = Scene::ObjectKind::Light;
            object.data = part.light->key;
        } else if (part.camera) {
            object.kind = Scene::ObjectKind::Camera;
            object.data = part.camera->key;
        } else if (part.instances) {
            object.kind = Scene::ObjectKind::Instances;
            object.data = part.instances->key;
            if (!empty)
                object.properties.push_back({"verylia.objectPath", part.meshPath});
        } else if (!empty) {
            object.kind = Scene::ObjectKind::Mesh;
            object.data = part.meshKey.empty() ? part.meshPath : part.meshKey;
            object.properties.push_back({"verylia.objectPath", part.meshPath});
        }
        for (const std::string& material : slotMaterials[i])
            object.materials.push_back(sentMaterials.contains(material) ? material : "");
        if (auto sent = sendRecord(Scene::object(object)); !sent)
            return sent;
        carriers[i] = object.id;
        if (part.camera)
            cameraObjects.emplace(part.camera->key, object.id);
    }
    return {};
}

std::expected<std::vector<Core::Diagnostic>, Core::Error> SceneSender::complete(std::vector<Core::Diagnostic> diagnostics) {
    for (Core::Diagnostic& diagnostic : this->diagnostics.take())
        diagnostics.push_back(std::move(diagnostic));
    auto result = sink.complete(diagnostics);
    if (result)
        meter.finish();
    return result;
}

ResourceStream::ResourceStream(SceneSender& sender)
    : thread([this, &sender](std::stop_token stop) {
          while (true) {
              auto batch = queue.pop(stop);
              if (!batch) {
                  result = std::unexpected(batch.error());
                  return;
              }
              if (!*batch)
                  return;
              result = sender.sendResources(**batch);
              if (!result)
                  return;
          }
      }) {}

void ResourceStream::push(ExportBatch batch) {
    queue.push(std::move(batch));
}

std::expected<void, Core::Error> ResourceStream::finish() {
    queue.finish();
    thread.join();
    return result;
}

MaterialOverride bindSlot(const ExportMesh& mesh, const std::size_t slot, const Scene::MeshMaterial& material) {
    if (slot < mesh.overrides.size() && !mesh.overrides[slot].path.empty())
        return mesh.overrides[slot];
    return {.path = material.materialPath};
}

int keyInstances(std::vector<ExportMesh>& parts, const std::string_view exportId, int& count) {
    int end = 0;
    for (ExportMesh& part : parts) {
        if (!part.instances)
            continue;
        part.instances->key = std::format("{}:instances {}", exportId, count++);
        for (std::size_t i = 0; i < part.instances->firstFrames.size(); ++i)
            end = std::max(end, part.instances->firstFrames[i] + static_cast<int>(part.instances->frameCounts[i]) - 1);
        if (part.instances->loopFrames > 0)
            end = std::max(end, part.instances->loopStart + static_cast<int>(part.instances->loopFrames) - 1);
    }
    return end;
}

std::expected<ExportResult, Core::Error> sendScene(const Assets::Provider& provider, Transport::RecordSink& sink, ExportJob job, std::stop_token stop,
                                                   ExportProgress progress) {
    SceneSender sender(provider, {}, sink, std::move(stop), std::move(progress));
    if (auto opened = sender.open(job.exportId, job.title); !opened)
        return std::unexpected(opened.error());
    int instancesCount = 0;
    const int instancesEnd = keyInstances(job.meshes, job.exportId, instancesCount);
    if (job.animation || instancesCount > 0)
        if (auto sent = sender.sendSceneRange(job.framesPerSecond, 1, std::max(job.frameEnd, instancesEnd)); !sent)
            return std::unexpected(sent.error());
    if (job.animation)
        if (auto sent = sender.sendAnimation(*job.animation); !sent)
            return std::unexpected(sent.error());
    if (auto sent = sender.send({.meshes = std::move(job.meshes), .images = std::move(job.images), .collection = 0}); !sent)
        return std::unexpected(sent.error());
    auto diagnostics = sender.complete(std::move(job.diagnostics));
    if (!diagnostics)
        return std::unexpected(diagnostics.error());
    return ExportResult{.title = job.title, .diagnostics = std::move(*diagnostics)};
}

}
