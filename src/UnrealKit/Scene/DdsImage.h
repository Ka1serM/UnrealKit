#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <stop_token>
#include <string_view>

#include "UnrealKit/Scene/Texture.h"

namespace UnrealKit::Scene {

class DdsImage {
public:
    DdsImage(const Scene::TextureMip& mip, std::string_view gamePath, std::stop_token stop);
    explicit DdsImage(const Scene::Texture& texture);

    std::size_t getSize() const;
    std::expected<void, Core::Error> write(std::span<std::uint8_t> file) const;

private:
    Scene::PixelFormat format;
    bool srgb;
    int width;
    int height;
    std::size_t payloadSize;
    const Scene::TextureMip* mip = nullptr;
    const Scene::Texture* texture = nullptr;
    std::string_view gamePath;
    std::stop_token stop;
};

std::expected<TextureView, Core::Error> readDds(std::span<const std::uint8_t> file, bool srgb);

}
