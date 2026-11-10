#include "UnrealKit/Transport/Download.h"

#include <cstring>
#include <format>
#include <memory>
#include <optional>
#include <string>

#include <emscripten/fetch.h>

#include "UnrealKit/Core/Error.h"

namespace UnrealKit::Transport {

namespace {

struct FetchClose {
    void operator()(emscripten_fetch_t* fetch) const noexcept { emscripten_fetch_close(fetch); }
};

std::expected<std::vector<std::byte>, Core::Error> request(const std::string& url, const std::optional<ByteRange> range, const std::stop_token stop) {
    if (stop.stop_requested())
        return Core::cancelled();
    emscripten_fetch_attr_t attributes;
    emscripten_fetch_attr_init(&attributes);
    std::strcpy(attributes.requestMethod, "GET");
    //! REPLACE skips the IndexedDB cache of Emscripten's fetch.
    attributes.attributes = EMSCRIPTEN_FETCH_LOAD_TO_MEMORY | EMSCRIPTEN_FETCH_SYNCHRONOUS | EMSCRIPTEN_FETCH_REPLACE;
    //! One range is a CORS-safelisted header, so the request needs no preflight.
    const std::string rangeText = range ? std::format("bytes={}-{}", range->offset, range->offset + range->size - 1) : std::string();
    const char* const headers[] = {"Range", rangeText.c_str(), nullptr};
    if (range)
        attributes.requestHeaders = headers;
    const std::unique_ptr<emscripten_fetch_t, FetchClose> fetch(emscripten_fetch(&attributes, url.c_str()));
    if (!fetch)
        return Core::fail("download", std::format("The download of {} cannot start on the browser's main thread.", url));
    if (stop.stop_requested())
        return Core::cancelled();
    //! Status 0: the browser refused the request, for example without a CORS header, or the network failed.
    if (fetch->status < 200 || fetch->status >= 300)
        return Core::fail("download", std::format("The download of {} failed: HTTP status {} {}", url, fetch->status, fetch->statusText));
    if (range && (fetch->status != 206 || fetch->numBytes != range->size))
        return Core::fail("download", std::format("The download of {} of {} gave HTTP status {} with {} bytes, not the range.", rangeText, url, fetch->status,
                                                  fetch->numBytes));
    const auto* data = reinterpret_cast<const std::byte*>(fetch->data);
    return std::vector<std::byte>(data, data + fetch->numBytes);
}

}

std::expected<std::vector<std::byte>, Core::Error> download(const std::string& url, const std::stop_token stop) {
    return request(url, std::nullopt, stop);
}

std::expected<std::vector<std::byte>, Core::Error> download(const std::string& url, const ByteRange range, const std::stop_token stop) {
    return request(url, range, stop);
}

}
