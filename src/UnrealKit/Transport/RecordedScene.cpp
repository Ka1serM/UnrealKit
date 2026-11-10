#include "UnrealKit/Transport/RecordedScene.h"

#include <utility>

namespace UnrealKit::Transport {

std::expected<void, Core::Error> RecordedScene::open(Progress) {
    return {};
}

std::expected<void, Core::Error> RecordedScene::send(Scene::Frame frame) {
    const std::lock_guard lock(mutex);
    frames.push_back(std::move(frame));
    return {};
}

std::expected<std::vector<Core::Diagnostic>, Core::Error> RecordedScene::complete(const std::span<const Core::Diagnostic> diagnostics) {
    return std::vector<Core::Diagnostic>(diagnostics.begin(), diagnostics.end());
}

std::vector<Scene::Frame> RecordedScene::take() {
    const std::lock_guard lock(mutex);
    return std::move(frames);
}

}
