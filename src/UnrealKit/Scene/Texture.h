#pragma once

#include <cstddef>
#include <span>
#include <stop_token>
#include <string_view>
#include <vector>

#include "Engine/Source/Runtime/CoreUObject/Serialization/BulkData.h"
#include "UnrealKit/Assets/Provider.h"

namespace UnrealKit::Scene {

enum class PixelFormat { B8G8R8A8, BC1, BC3, BC7, BC4, BC5, BC6H, G8, FloatRGBA, R8G8B8A8 };

struct TextureView {
    PixelFormat format = PixelFormat::BC7;
    bool srgb = false;
    int width = 0;
    int height = 0;
    std::span<const std::byte> data;
};

struct Texture {
    PixelFormat format = PixelFormat::BC7;
    bool srgb = false;
    int width = 0;
    int height = 0;
    std::vector<std::byte> data;

    TextureView view() const { return {.format = format, .srgb = srgb, .width = width, .height = height, .data = data}; }
};

struct CubeTexture {
    PixelFormat format = PixelFormat::BC6H;
    bool srgb = false;
    int size = 0;
    //! The faces follow each other in the order +X, -X, +Y, -Y, +Z, -Z, which is also OpenGL's.
    std::vector<std::byte> data;

    TextureView face(const std::size_t index) const {
        const std::size_t bytes = data.size() / 6;
        return {.format = format, .srgb = srgb, .width = size, .height = size, .data = std::span(data).subspan(index * bytes, bytes)};
    }
};

struct TextureMip {
    PixelFormat format = PixelFormat::BC7;
    bool srgb = false;
    int width = 0;
    int height = 0;
    const ue5parse::runtime::FByteBulkData* data = nullptr;

    std::size_t getSize() const { return data->GetPayloadSize(); }
};

std::expected<TextureMip, Core::Error> findTextureMip(const Assets::Provider& provider, std::string_view gamePath, std::stop_token stop);
std::expected<void, Core::Error> readTextureMip(const TextureMip& mip, std::span<std::byte> output, std::string_view gamePath, std::stop_token stop);

std::expected<Texture, Core::Error> readTexture(const Assets::Provider& provider, std::string_view gamePath, std::stop_token stop);

std::expected<bool, Core::Error> isCubeTexture(std::string_view gamePath, std::stop_token stop);
std::expected<CubeTexture, Core::Error> readCube(std::string_view gamePath, std::stop_token stop);

Texture decodeToRgba(const TextureView& texture);
Texture encodeToBc6h(const Texture& texture);

}
