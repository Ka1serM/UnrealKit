#pragma once

#include <cstdint>
#include <expected>
#include <functional>
#include <span>
#include <vector>

#include "UnrealKit/Core/Diagnostic.h"
#include "UnrealKit/Core/Error.h"
#include "UnrealKit/Scene/Records.h"

namespace UnrealKit::Transport {

class RecordSink {
public:
    using Progress = std::function<void(std::uint32_t done, std::uint32_t total)>;

    virtual ~RecordSink() = default;

    virtual std::expected<void, Core::Error> open(Progress progress) = 0;
    virtual std::expected<void, Core::Error> send(Scene::Frame frame) = 0;
    virtual std::expected<std::vector<Core::Diagnostic>, Core::Error> complete(std::span<const Core::Diagnostic> diagnostics) = 0;
};

}
