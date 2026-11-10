#include "UnrealKit/Transport/Download.h"

#include <format>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

#include <httplib.h>

#include "UnrealKit/Core/Error.h"

namespace UnrealKit::Transport {

namespace {

std::expected<httplib::Client*, Core::Error> clientOf(const std::string& origin) {
    thread_local std::unordered_map<std::string, std::unique_ptr<httplib::Client>> clients;
    if (const auto found = clients.find(origin); found != clients.end())
        return found->second.get();
    auto client = std::make_unique<httplib::Client>(origin);
    if (!client->is_valid())
        return Core::fail("download", std::format("The URL origin {} is not valid.", origin));
    client->set_keep_alive(true);
    client->set_follow_location(true);
    client->set_connection_timeout(10);
    client->set_read_timeout(30);
    return clients.emplace(origin, std::move(client)).first->second.get();
}

std::expected<std::vector<std::byte>, Core::Error> request(const std::string& url, const std::optional<ByteRange> range, const std::stop_token stop) {
    const std::size_t host = url.find("://");
    const std::size_t path = host == std::string::npos ? std::string::npos : url.find('/', host + 3);
    if (path == std::string::npos)
        return Core::fail("download", std::format("The URL {} has no path.", url));
    httplib::Headers headers;
    const std::string rangeText = range ? std::format("bytes={}-{}", range->offset, range->offset + range->size - 1) : std::string();
    if (range)
        headers.emplace("Range", rangeText);

    const std::expected<httplib::Client*, Core::Error> client = clientOf(url.substr(0, path));
    if (!client)
        return std::unexpected(client.error());
    std::vector<std::byte> body;
    if (range)
        body.reserve(range->size);
    const httplib::Result result = (*client)->Get(
        url.substr(path), headers,
        [&](const char* data, const std::size_t size) {
            const auto* bytes = reinterpret_cast<const std::byte*>(data);
            body.insert(body.end(), bytes, bytes + size);
            return true;
        },
        [&](std::size_t, std::size_t) { return !stop.stop_requested(); });
    if (stop.stop_requested())
        return Core::cancelled();
    if (!result)
        return Core::fail("download", std::format("The download of {} failed: {}", url, httplib::to_string(result.error())));
    if (result->status < 200 || result->status >= 300)
        return Core::fail("download", std::format("The download of {} failed: HTTP status {}", url, result->status));
    if (range && (result->status != 206 || body.size() != range->size))
        return Core::fail(
            "download", std::format("The download of {} of {} gave HTTP status {} with {} bytes, not the range.", rangeText, url, result->status, body.size()));
    return body;
}

}

std::expected<std::vector<std::byte>, Core::Error> download(const std::string& url, const std::stop_token stop) {
    return request(url, std::nullopt, stop);
}

std::expected<std::vector<std::byte>, Core::Error> download(const std::string& url, const ByteRange range, const std::stop_token stop) {
    return request(url, range, stop);
}

}
