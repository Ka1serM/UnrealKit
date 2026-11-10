#pragma once

#include <mutex>
#include <vector>

#include "UnrealKit/Transport/RecordSink.h"

namespace UnrealKit::Transport {

class RecordedScene final : public RecordSink {
public:
    std::expected<void, Core::Error> open(Progress progress) override;
    std::expected<void, Core::Error> send(Scene::Frame frame) override;
    std::expected<std::vector<Core::Diagnostic>, Core::Error> complete(std::span<const Core::Diagnostic> diagnostics) override;

    std::vector<Scene::Frame> take();

private:
    std::mutex mutex;
    std::vector<Scene::Frame> frames;
};

}
