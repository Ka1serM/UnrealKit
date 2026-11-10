#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "UnrealKit/Core/Diagnostic.h"
#include "UnrealKit/Scene/Records.h"
#include "UnrealKit/Transport/Importer.h"
#include "UnrealKit/Transport/RecordSink.h"
#include "UnrealKit/Transport/WebSocket.h"

namespace UnrealKit::Transport {

class SceneSession final : public RecordSink {
public:
    using VersionMismatch = std::function<void(std::string_view importerVersion)>;

    SceneSession(Product product, Importer importer, std::stop_token stop, VersionMismatch versionMismatch);
    ~SceneSession() override;
    SceneSession(const SceneSession&) = delete;
    SceneSession& operator=(const SceneSession&) = delete;

    std::expected<void, Core::Error> open(Progress progress) override;
    std::expected<void, Core::Error> send(Scene::Frame frame) override;
    std::expected<std::vector<Core::Diagnostic>, Core::Error> complete(std::span<const Core::Diagnostic> diagnostics) override;

private:
    enum class State { Open, Finished, Failed };

    std::expected<void, Core::Error> handshake();
    void read();
    std::expected<void, Core::Error> check();
    std::unexpected<Core::Error> fail(std::string_view message) const;
    std::string failureText(const scene::stream::Failed& failed) const;

    Product product;
    Importer importer;
    std::stop_token stop;
    VersionMismatch versionMismatch;
    Progress progress;
    WebSocket socket;

    std::mutex mutex;
    std::condition_variable changed;
    State state = State::Open;
    std::string failure;
    std::vector<Core::Diagnostic> finished;
    std::chrono::steady_clock::time_point lastActivity = std::chrono::steady_clock::now();

    std::jthread reader;
};

}
