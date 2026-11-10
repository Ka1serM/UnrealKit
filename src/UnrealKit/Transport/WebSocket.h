#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <span>
#include <string>

#include "UnrealKit/Core/Error.h"

namespace UnrealKit::Transport {

class WebSocket {
public:
    enum class Received { Message, Timeout, Closed };

    explicit WebSocket(std::chrono::milliseconds writeTimeout);
    ~WebSocket();
    WebSocket(const WebSocket&) = delete;
    WebSocket& operator=(const WebSocket&) = delete;

    std::expected<void, Core::Error> connect(const std::string& url, std::chrono::milliseconds connectTimeout);
    std::expected<void, Core::Error> send(std::span<const std::byte> message);
    std::expected<Received, Core::Error> receive(std::string& message, std::chrono::milliseconds timeout);
    void close() noexcept;
    bool isOpen() const noexcept { return open; }

    struct Connection;

private:
    std::chrono::milliseconds writeTimeout;
    std::unique_ptr<Connection> connection;
    std::atomic<bool> open = false;
};

}
