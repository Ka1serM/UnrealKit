#include "UnrealKit/Scene/DdsImage.h"

#include <algorithm>
#include <cstring>
#include <optional>
#include <utility>

namespace UnrealKit::Scene {

namespace {

constexpr std::size_t headerSize = 128;
constexpr std::size_t dx10HeaderSize = 20;

constexpr std::uint32_t capsFlag = 0x1, heightFlag = 0x2, widthFlag = 0x4, pitchFlag = 0x8, pixelFormatFlag = 0x1000, mipMapCountFlag = 0x20000,
                        linearSizeFlag = 0x80000;
constexpr std::uint32_t alphaPixels = 0x1, fourCc = 0x4, rgb = 0x40, luminance = 0x20000;
constexpr std::uint32_t complexCaps = 0x8, textureCaps = 0x1000, mipMapCaps = 0x400000;
constexpr std::uint32_t dxgiBc1 = 71, dxgiBc1Srgb = 72, dxgiBc3 = 77, dxgiBc3Srgb = 78, dxgiBc4 = 80, dxgiBc5 = 83, dxgiBc6hUf16 = 95, dxgiBc7 = 98,
                        dxgiBc7Srgb = 99;
constexpr std::uint32_t textureDimension2d = 3;

void put(std::span<std::uint8_t> file, const std::size_t offset, const std::uint32_t value) {
    std::memcpy(file.data() + offset, &value, sizeof(value));
}

std::uint32_t get(const std::span<const std::uint8_t> file, const std::size_t offset) {
    std::uint32_t value = 0;
    std::memcpy(&value, file.data() + offset, sizeof(value));
    return value;
}

std::optional<Scene::PixelFormat> formatOfDxgi(const std::uint32_t dxgi) {
    switch (dxgi) {
    case dxgiBc1:
    case dxgiBc1Srgb:
        return Scene::PixelFormat::BC1;
    case dxgiBc3:
    case dxgiBc3Srgb:
        return Scene::PixelFormat::BC3;
    case dxgiBc4:
        return Scene::PixelFormat::BC4;
    case dxgiBc5:
        return Scene::PixelFormat::BC5;
    case dxgiBc6hUf16:
        return Scene::PixelFormat::BC6H;
    case dxgiBc7:
    case dxgiBc7Srgb:
        return Scene::PixelFormat::BC7;
    default:
        return std::nullopt;
    }
}

std::size_t dxgiBlockBytes(const Scene::PixelFormat format) {
    return format == Scene::PixelFormat::BC1 || format == Scene::PixelFormat::BC4 ? 8 : 16;
}

std::uint32_t dxgiFormat(const Scene::PixelFormat format, const bool srgb) {
    switch (format) {
    case Scene::PixelFormat::BC1:
        return srgb ? dxgiBc1Srgb : dxgiBc1;
    case Scene::PixelFormat::BC3:
        return srgb ? dxgiBc3Srgb : dxgiBc3;
    case Scene::PixelFormat::BC4:
        return dxgiBc4;
    case Scene::PixelFormat::BC5:
        return dxgiBc5;
    case Scene::PixelFormat::BC6H:
        return dxgiBc6hUf16;
    case Scene::PixelFormat::BC7:
        return srgb ? dxgiBc7Srgb : dxgiBc7;
    case Scene::PixelFormat::B8G8R8A8:
    case Scene::PixelFormat::G8:
        return 0;
    case Scene::PixelFormat::R8G8B8A8:
    case Scene::PixelFormat::FloatRGBA:
        break;
    }
    std::unreachable();
}

}

DdsImage::DdsImage(const Scene::TextureMips& mips, const std::string_view gamePath, std::stop_token stop)
    : format(mips.format), srgb(mips.srgb), width(mips.width), height(mips.height), levels(static_cast<int>(mips.levels.size())), payloadSize(mips.size),
      mips(&mips), gamePath(gamePath), stop(std::move(stop)) {}

DdsImage::DdsImage(const Scene::Texture& texture)
    : format(texture.format), srgb(texture.srgb), width(texture.width), height(texture.height), levels(texture.levels), payloadSize(texture.data.size()),
      texture(&texture) {}

std::size_t DdsImage::getSize() const {
    return headerSize + (dxgiFormat(format, false) ? dx10HeaderSize : 0) + payloadSize;
}

std::expected<void, Core::Error> DdsImage::write(std::span<std::uint8_t> file) const {
    const std::uint32_t dxgi = dxgiFormat(format, srgb);
    const std::size_t dataOffset = headerSize + (dxgi ? dx10HeaderSize : 0);
    std::fill(file.begin(), file.begin() + dataOffset, std::uint8_t{0});
    std::memcpy(file.data(), "DDS ", 4);
    put(file, 4, 124);
    const bool mipmapped = levels > 1;
    put(file, 8, capsFlag | heightFlag | widthFlag | pixelFormatFlag | (dxgi ? linearSizeFlag : pitchFlag) | (mipmapped ? mipMapCountFlag : 0));
    put(file, 12, static_cast<std::uint32_t>(height));
    put(file, 16, static_cast<std::uint32_t>(width));
    const std::uint32_t pixelBytes = format == Scene::PixelFormat::G8 ? 1 : 4;
    const std::size_t topBytes = dxgi ? static_cast<std::size_t>((width + 3) / 4) * ((height + 3) / 4) * dxgiBlockBytes(format) : 0;
    put(file, 20, static_cast<std::uint32_t>(dxgi ? topBytes : static_cast<std::size_t>(width) * pixelBytes));
    put(file, 28, static_cast<std::uint32_t>(levels));
    put(file, 76, 32);
    put(file, 108, textureCaps | (mipmapped ? complexCaps | mipMapCaps : 0));
    if (dxgi) {
        put(file, 80, fourCc);
        std::memcpy(file.data() + 84, "DX10", 4);
        put(file, headerSize, dxgi);
        put(file, headerSize + 4, textureDimension2d);
        put(file, headerSize + 12, 1);
    } else if (format == Scene::PixelFormat::G8) {
        put(file, 80, luminance);
        put(file, 88, 8);
        put(file, 92, 0xff);
    } else {
        put(file, 80, rgb | alphaPixels);
        put(file, 88, 32);
        put(file, 92, 0x00ff0000);
        put(file, 96, 0x0000ff00);
        put(file, 100, 0x000000ff);
        put(file, 104, 0xff000000);
    }
    if (texture != nullptr) {
        std::memcpy(file.data() + dataOffset, texture->data.data(), payloadSize);
        return {};
    }
    return Scene::readTextureMips(*mips, std::as_writable_bytes(file.subspan(dataOffset)), gamePath, stop);
}

std::expected<TextureView, Core::Error> readDds(const std::span<const std::uint8_t> file, const bool srgb) {
    if (file.size() < headerSize || std::memcmp(file.data(), "DDS ", 4) != 0)
        return Core::fail("dds.format", "The image is not a DDS file.");
    TextureView texture{.srgb = srgb,
                        .width = static_cast<int>(get(file, 16)),
                        .height = static_cast<int>(get(file, 12)),
                        .levels = (get(file, 8) & mipMapCountFlag) != 0 ? std::max(static_cast<int>(get(file, 28)), 1) : 1};
    std::size_t dataOffset = headerSize;
    const std::uint32_t flags = get(file, 80);
    if ((flags & fourCc) != 0 && std::memcmp(file.data() + 84, "DX10", 4) == 0) {
        dataOffset += dx10HeaderSize;
        const std::optional<PixelFormat> format = file.size() >= dataOffset ? formatOfDxgi(get(file, headerSize)) : std::nullopt;
        if (!format)
            return Core::fail("dds.format", "The DDS file has a format that the app does not write.");
        texture.format = *format;
    } else if ((flags & luminance) != 0) {
        texture.format = PixelFormat::G8;
    } else if ((flags & rgb) != 0) {
        texture.format = PixelFormat::B8G8R8A8;
    } else {
        return Core::fail("dds.format", "The DDS file has a format that the app does not write.");
    }
    texture.data = std::as_bytes(file.subspan(dataOffset));
    return texture;
}

}
