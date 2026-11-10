#pragma once

#include <cstddef>
#include <cstdint>
#include <stop_token>
#include <string>
#include <vector>

#include "UnrealKit/Core/Error.h"

namespace UnrealKit::Transport {

std::expected<std::vector<std::byte>, Core::Error> download(const std::string& url, std::stop_token stop);

struct ByteRange {
    std::uint64_t offset;
    std::uint64_t size;
};

std::expected<std::vector<std::byte>, Core::Error> download(const std::string& url, ByteRange range, std::stop_token stop);

}
