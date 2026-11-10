#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "UnrealKit/Assets/Provider.h"
#include "UnrealKit/Core/Diagnostic.h"
#include "UnrealKit/Core/WorkQueue.h"
#include "UnrealKit/Materials/MaterialData.h"
#include "UnrealKit/Scene/Matrix.h"
#include "UnrealKit/Scene/MeshLod.h"
#include "UnrealKit/Scene/Records.h"
#include "UnrealKit/Transport/RecordSink.h"
#include "UnrealKit/Export/MaterialOverride.h"

namespace UnrealKit::Export {

struct ExportMesh {
    std::string meshPath;
    std::string meshKey;
    std::string name;
    std::vector<MaterialOverride> overrides;
    std::vector<unsigned char> colors;
    std::optional<Scene::SceneLight> light;
    std::optional<Scene::SceneCamera> camera;
    std::optional<Scene::SceneInstances> instances;
    bool particles = false;
    bool armature = false;
    int parent = -1;
    std::vector<std::string> attachPoints;
    Scene::Matrix matrix = Scene::Matrix::identity();
    std::string poseSource;
    std::string originPoint;
    std::optional<std::uint32_t> collection;
    std::string animation;
    std::string track;
    bool visible = true;
    bool castShadow = true;
    int sortPriority = 0;
    bool reverseCulling = false;
    std::string viewer;
    Scene::ViewerVisibility viewerVisibility = Scene::ViewerVisibility::Everyone;
    bool required = false;
};

struct ExportJob {
    std::string exportId;
    std::string title;
    std::vector<ExportMesh> meshes;
    std::vector<std::string> images;
    std::vector<Core::Diagnostic> diagnostics;
    std::optional<Scene::Animation> animation;
    std::uint32_t framesPerSecond = 30;
    int frameEnd = 1;
};

enum class ExportStage { Recording, Sending };

using ExportProgress = std::function<void(ExportStage stage, float fraction)>;

struct ExportResult {
    std::string title;
    std::vector<Core::Diagnostic> diagnostics;
    int places = 0;
};

struct ExportBatch {
    std::vector<ExportMesh> meshes;
    std::vector<std::string> images;
    std::uint32_t collection = 0;
};

struct SceneOptions {
    bool recording = false;
};

class Diagnostics {
public:
    void add(Core::Diagnostic diagnostic);
    void add(std::vector<Core::Diagnostic> diagnostics);
    std::vector<Core::Diagnostic> take();

private:
    std::mutex mutex;
    std::vector<Core::Diagnostic> items;
};

class ProgressMeter {
public:
    ProgressMeter(ExportProgress report, ExportStage stage);

    void record(float fraction);
    void addRecords(std::size_t records);
    void sent();
    void built(std::uint32_t done);
    void finish();

private:
    void update();

    ExportProgress report;
    std::mutex mutex;
    std::size_t total = 0;
    std::size_t sentCount = 0;
    std::size_t builtCount = 0;
    ExportStage stage;
    float recordedShown = 0;
    float shown = 0;
};

class SceneSender {
public:
    SceneSender(const Assets::Provider& provider, SceneOptions options, Transport::RecordSink& sink, std::stop_token stop, ExportProgress progress);

    std::expected<void, Core::Error> open(std::string_view exportId, std::string_view title);
    std::expected<std::uint32_t, Core::Error> collection(const std::string& name, std::uint32_t parent);
    void recorded(float fraction);
    std::expected<void, Core::Error> sendSceneRange(std::uint32_t framesPerSecond, int frameStart, int frameEnd);
    std::expected<void, Core::Error> sendAnimation(const Scene::Animation& animation);
    std::expected<void, Core::Error> send(const ExportBatch& batch);
    std::expected<void, Core::Error> sendResources(const ExportBatch& batch);
    std::expected<std::vector<Core::Diagnostic>, Core::Error> complete(std::vector<Core::Diagnostic> diagnostics);

private:
    const Scene::MeshLod* findMesh(const std::string& path) const;
    std::expected<void, Core::Error> sendRecord(Scene::Frame frame);
    std::expected<std::vector<std::vector<std::string>>, Core::Error> sendSharedResources(const ExportBatch& batch);

    const Assets::Provider& provider;
    SceneOptions options;
    std::stop_token stop;
    ProgressMeter meter;
    Transport::RecordSink& sink;
    Diagnostics diagnostics;
    std::unordered_set<std::string> requestedMeshes, requestedMaterials, requestedImages;
    std::unordered_set<std::string> sentMeshes, sentMaterials;
    std::unordered_set<std::string> requestedPoseSources;
    std::unordered_set<std::string> missingMaterials;
    std::mutex sentMutex;
    std::unordered_map<std::string, Scene::MeshLod> meshes;
    std::map<std::pair<std::uint32_t, std::string>, std::uint32_t> collections;
    std::uint32_t nextId = 0;
    std::unordered_map<std::string, std::uint32_t> cameraObjects;
};

class ResourceStream {
public:
    explicit ResourceStream(SceneSender& sender);

    void push(ExportBatch batch);
    std::expected<void, Core::Error> finish();

private:
    Core::WorkQueue<ExportBatch> queue;
    std::expected<void, Core::Error> result;
    std::jthread thread;
};

MaterialOverride bindSlot(const ExportMesh& mesh, std::size_t slot, const Scene::MeshMaterial& material);

int keyInstances(std::vector<ExportMesh>& parts, std::string_view exportId, int& count);

std::expected<ExportResult, Core::Error> sendScene(const Assets::Provider& provider, Transport::RecordSink& sink, ExportJob job, std::stop_token stop,
                                                   ExportProgress progress);

}
