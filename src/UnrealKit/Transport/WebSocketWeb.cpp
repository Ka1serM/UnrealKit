#include "UnrealKit/Transport/WebSocket.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <format>
#include <mutex>
#include <thread>
#include <unordered_map>

#include <emscripten/html5.h>
#include <emscripten/websocket.h>

namespace UnrealKit::Transport {

namespace {

using namespace std::chrono_literals;

constexpr std::size_t sendBufferLimit = 16 * 1024 * 1024;

}

struct WebSocket::Connection {
    enum class State { Connecting, Open, Closed };

    EMSCRIPTEN_WEBSOCKET_T socket = 0;
    std::mutex mutex;
    std::condition_variable changed;
    State state = State::Connecting;
    bool receivedText = false;
    std::deque<std::string> messages;
};

namespace {

//! Emscripten deletes a socket without removing its event handlers, so an event can come after the connection is gone.
std::mutex connectionsMutex;
std::unordered_map<std::uintptr_t, WebSocket::Connection*> connections;
std::uintptr_t nextConnection = 1;

template <typename Change>
EM_BOOL update(void* id, Change change) {
    const std::scoped_lock registry(connectionsMutex);
    const auto found = connections.find(reinterpret_cast<std::uintptr_t>(id));
    if (found == connections.end())
        return EM_TRUE;
    WebSocket::Connection& connection = *found->second;
    {
        const std::scoped_lock lock(connection.mutex);
        change(connection);
    }
    connection.changed.notify_all();
    return EM_TRUE;
}

EM_BOOL onOpen(int, const EmscriptenWebSocketOpenEvent*, void* id) {
    return update(id, [](WebSocket::Connection& connection) { connection.state = WebSocket::Connection::State::Open; });
}

EM_BOOL onMessage(int, const EmscriptenWebSocketMessageEvent* event, void* id) {
    return update(id, [event](WebSocket::Connection& connection) {
        if (event->isText)
            connection.receivedText = true;
        else
            connection.messages.emplace_back(reinterpret_cast<const char*>(event->data), event->numBytes);
    });
}

EM_BOOL onEnd(void* id) {
    return update(id, [](WebSocket::Connection& connection) { connection.state = WebSocket::Connection::State::Closed; });
}

EM_BOOL onClose(int, const EmscriptenWebSocketCloseEvent*, void* id) {
    return onEnd(id);
}
EM_BOOL onError(int, const EmscriptenWebSocketErrorEvent*, void* id) {
    return onEnd(id);
}

}

WebSocket::WebSocket(const std::chrono::milliseconds writeTimeout) : writeTimeout(writeTimeout), connection(std::make_unique<Connection>()) {}

std::expected<void, Core::Error> WebSocket::connect(const std::string& url, const std::chrono::milliseconds connectTimeout) {
    std::uintptr_t id;
    {
        const std::scoped_lock registry(connectionsMutex);
        id = nextConnection++;
        connections.emplace(id, connection.get());
    }
    EmscriptenWebSocketCreateAttributes attributes;
    emscripten_websocket_init_create_attributes(&attributes);
    attributes.url = url.c_str();
    attributes.createOnMainThread = EM_TRUE;
    connection->socket = emscripten_websocket_new(&attributes);
    if (connection->socket <= 0) {
        const std::scoped_lock registry(connectionsMutex);
        connections.erase(id);
        return Core::fail("websocket", std::format("The browser cannot open a WebSocket to {}.", url));
    }
    auto* const userData = reinterpret_cast<void*>(id);
    const pthread_t mainThread = EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD;
    emscripten_websocket_set_onopen_callback_on_thread(connection->socket, userData, onOpen, mainThread);
    emscripten_websocket_set_onmessage_callback_on_thread(connection->socket, userData, onMessage, mainThread);
    emscripten_websocket_set_onclose_callback_on_thread(connection->socket, userData, onClose, mainThread);
    emscripten_websocket_set_onerror_callback_on_thread(connection->socket, userData, onError, mainThread);

    std::unique_lock lock(connection->mutex);
    connection->changed.wait_for(lock, connectTimeout, [this] { return connection->state != Connection::State::Connecting; });
    if (connection->state == Connection::State::Open) {
        open = true;
        return {};
    }
    lock.unlock();
    {
        const std::scoped_lock registry(connectionsMutex);
        connections.erase(id);
    }
    emscripten_websocket_delete(connection->socket);
    connection->socket = 0;
    return Core::fail("websocket", std::format("The WebSocket to {} did not open within {}.", url, connectTimeout));
}

WebSocket::~WebSocket() {
    close();
    {
        const std::scoped_lock registry(connectionsMutex);
        std::erase_if(connections, [this](const auto& entry) { return entry.second == connection.get(); });
    }
    if (connection->socket > 0)
        emscripten_websocket_delete(connection->socket);
}

std::expected<void, Core::Error> WebSocket::send(const std::span<const std::byte> message) {
    const auto deadline = std::chrono::steady_clock::now() + writeTimeout;
    for (std::size_t buffered = 0;;) {
        if (!open)
            return Core::fail("websocket", "The WebSocket is closed.");
        if (emscripten_websocket_get_buffered_amount(connection->socket, &buffered) != EMSCRIPTEN_RESULT_SUCCESS)
            return Core::fail("websocket", "The WebSocket broke.");
        if (buffered < sendBufferLimit)
            break;
        if (std::chrono::steady_clock::now() > deadline)
            return Core::fail("websocket", std::format("The WebSocket peer took nothing for {}.", writeTimeout));
        std::this_thread::sleep_for(5ms);
    }
    if (emscripten_websocket_send_binary(connection->socket, const_cast<std::byte*>(message.data()), static_cast<std::uint32_t>(message.size()))
        != EMSCRIPTEN_RESULT_SUCCESS)
        return Core::fail("websocket", "The WebSocket broke.");
    return {};
}

std::expected<WebSocket::Received, Core::Error> WebSocket::receive(std::string& message, const std::chrono::milliseconds timeout) {
    std::unique_lock lock(connection->mutex);
    connection->changed.wait_for(
        lock, timeout, [this] { return !connection->messages.empty() || connection->receivedText || connection->state == Connection::State::Closed; });
    if (connection->receivedText)
        return Core::fail("websocket", "The WebSocket peer sent a text message.");
    if (!connection->messages.empty()) {
        message = std::move(connection->messages.front());
        connection->messages.pop_front();
        return Received::Message;
    }
    return connection->state == Connection::State::Closed ? Received::Closed : Received::Timeout;
}

void WebSocket::close() noexcept {
    if (!open.exchange(false))
        return;
    emscripten_websocket_close(connection->socket, 1000, "");
}

}
