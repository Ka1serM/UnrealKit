#include "UnrealKit/Transport/WebSocket.h"

#include <format>

#include <httplib.h>

namespace UnrealKit::Transport {

struct WebSocket::Connection {
    explicit Connection(const std::string& url) : client(url) {}

    httplib::ws::WebSocketClient client;
};

WebSocket::WebSocket(const std::chrono::milliseconds writeTimeout) : writeTimeout(writeTimeout) {}

WebSocket::~WebSocket() {
    close();
}

std::expected<void, Core::Error> WebSocket::connect(const std::string& url, const std::chrono::milliseconds connectTimeout) {
    connection = std::make_unique<Connection>(url);
    httplib::ws::WebSocketClient& client = connection->client;
    if (!client.is_valid())
        return Core::fail("websocket", std::format("The WebSocket URL {} is not valid.", url));
    client.set_connection_timeout(connectTimeout);
    client.set_write_timeout(writeTimeout);
    if (const httplib::ws::Result result = client.connect(); !result)
        return Core::fail("websocket", std::format("The WebSocket connection to {} failed: {}", url, httplib::to_string(result.error())));
    open = true;
    return {};
}

std::expected<void, Core::Error> WebSocket::send(const std::span<const std::byte> message) {
    if (connection->client.send(reinterpret_cast<const char*>(message.data()), message.size()))
        return {};
    open = false;
    return Core::fail("websocket", "The WebSocket send failed: the connection broke, or the peer took nothing for the write timeout.");
}

std::expected<WebSocket::Received, Core::Error> WebSocket::receive(std::string& message, const std::chrono::milliseconds timeout) {
    connection->client.set_read_timeout(timeout);
    switch (connection->client.read(message)) {
    case httplib::ws::Binary:
        return Received::Message;
    case httplib::ws::Timeout:
        return Received::Timeout;
    case httplib::ws::Text:
        return Core::fail("websocket", "The WebSocket peer sent a text message.");
    case httplib::ws::Fail:
        break;
    }
    open = false;
    return Received::Closed;
}

void WebSocket::close() noexcept {
    if (!open.exchange(false))
        return;
    connection->client.close();
}

}
