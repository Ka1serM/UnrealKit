#include "UnrealKit/Scene/Texture.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <utility>

#define BCDEC_IMPLEMENTATION
#include <bcdec.h>

#include "Engine/Source/Runtime/Engine/Engine/Texture2D.h"
#include "Engine/Source/Runtime/Engine/Engine/TextureCube.h"
#include "UnrealKit/Assets/Reading.h"
#include "UnrealKit/Scene/Bc6hEncoder.h"

namespace UnrealKit::Scene {

namespace {

using Assets::Reading::fail;

std::expected<PixelFormat, Core::Error> formatOf(const ue::EPixelFormat format, const std::string_view gamePath) {
    using ue::EPixelFormat;
    switch (format) {
    case EPixelFormat::PF_B8G8R8A8:
        return PixelFormat::B8G8R8A8;
    case EPixelFormat::PF_DXT1:
        return PixelFormat::BC1;
    case EPixelFormat::PF_DXT5:
        return PixelFormat::BC3;
    case EPixelFormat::PF_BC7:
        return PixelFormat::BC7;
    case EPixelFormat::PF_BC4:
        return PixelFormat::BC4;
    case EPixelFormat::PF_BC5:
        return PixelFormat::BC5;
    case EPixelFormat::PF_BC6H:
        return PixelFormat::BC6H;
    case EPixelFormat::PF_G8:
        return PixelFormat::G8;
    case EPixelFormat::PF_FloatRGBA:
        return PixelFormat::FloatRGBA;
    default:
        return Core::fail("texture.format", std::format("'{}' has the unsupported pixel format {}.", gamePath, static_cast<int>(format)));
    }
}

size_t storedSize(const PixelFormat format, const size_t width, const size_t height) {
    const size_t blocks = (width + 3) / 4 * ((height + 3) / 4);
    switch (format) {
    case PixelFormat::B8G8R8A8:
    case PixelFormat::R8G8B8A8:
        return width * height * 4;
    case PixelFormat::G8:
        return width * height;
    case PixelFormat::FloatRGBA:
        return width * height * 8;
    case PixelFormat::BC1:
    case PixelFormat::BC4:
        return blocks * 8;
    case PixelFormat::BC3:
    case PixelFormat::BC5:
    case PixelFormat::BC6H:
    case PixelFormat::BC7:
        return blocks * 16;
    }
    return 0;
}

}

std::expected<TextureMip, Core::Error> findTextureMip(const Assets::Provider& provider, const std::string_view gamePath, const std::stop_token stop) {
    (void)provider;
    const std::expected<ue::UTexture2D*, Core::Error> source = Assets::Reading::loadObject<ue::UTexture2D>(gamePath, "texture", stop);
    if (!source)
        return std::unexpected(source.error());
    const ue::FTexturePlatformData* data = (*source)->GetPlatformData();
    if (data != nullptr && data->bIsVirtual)
        return Core::fail("texture.virtual", std::format("'{}' is a virtual texture.", gamePath));
    const int first = (*source)->GetFirstMipIndex();
    if (data == nullptr || first < 0)
        return Core::fail("texture.mip", std::format("'{}' has no mip with data.", gamePath));
    const std::expected<PixelFormat, Core::Error> format = formatOf(data->PixelFormat, gamePath);
    if (!format)
        return std::unexpected(format.error());
    const ue::FTexture2DMipMap& mip = data->Mips[first];
    const TextureMip result{.format = *format, .srgb = (*source)->SRGB, .width = mip.SizeX, .height = mip.SizeY, .data = &mip.BulkData};
    if (result.width <= 0 || result.height <= 0)
        return Core::fail("texture.mip", std::format("The mip of '{}' is {}x{}.", gamePath, result.width, result.height));
    const size_t expected = storedSize(result.format, result.width, result.height);
    if (result.getSize() != expected)
        return Core::fail("texture.mip", std::format("The mip of '{}' has {} bytes, not {}.", gamePath, result.getSize(), expected));
    return result;
}

std::expected<void, Core::Error> readTextureMip(const TextureMip& mip, const std::span<std::byte> output, const std::string_view gamePath,
                                                const std::stop_token stop) {
    if (stop.stop_requested())
        return Core::cancelled();
    const auto read = mip.data->ReadInto(output);
    if (!read)
        return fail("texture.mip", gamePath, read.error());
    return {};
}

std::expected<Texture, Core::Error> readTexture(const Assets::Provider& provider, const std::string_view gamePath, const std::stop_token stop) {
    const std::expected<TextureMip, Core::Error> mip = findTextureMip(provider, gamePath, stop);
    if (!mip)
        return std::unexpected(mip.error());
    Texture texture{.format = mip->format, .srgb = mip->srgb, .width = mip->width, .height = mip->height};
    texture.data.resize(mip->getSize());
    if (std::expected<void, Core::Error> read = readTextureMip(*mip, texture.data, gamePath, stop); !read)
        return std::unexpected(read.error());
    return texture;
}

namespace {

using Rgba = std::array<std::uint8_t, 4>;
using RgbaFloat = std::array<float, 4>;

template <size_t BlockBytes, typename Pixel = Rgba, typename DecodeBlock>
std::vector<Pixel> decodeBlocks(const TextureView& texture, DecodeBlock decodeBlock) {
    const int blocksWide = (texture.width + 3) / 4;
    const int blocksHigh = (texture.height + 3) / 4;
    std::vector<Pixel> pixels(static_cast<size_t>(texture.width) * texture.height);
    Pixel block[16];
    const std::byte* source = texture.data.data();
    for (int blockY = 0; blockY < blocksHigh; ++blockY) {
        for (int blockX = 0; blockX < blocksWide; ++blockX, source += BlockBytes) {
            decodeBlock(source, block);
            const int rows = std::min(4, texture.height - blockY * 4);
            const int columns = std::min(4, texture.width - blockX * 4);
            for (int row = 0; row < rows; ++row)
                std::copy_n(block + row * 4, columns, pixels.begin() + static_cast<ptrdiff_t>((blockY * 4 + row) * texture.width + blockX * 4));
        }
    }
    return pixels;
}

float halfToFloat(const std::uint16_t half) {
    const std::uint32_t sign = static_cast<std::uint32_t>(half & 0x8000) << 16;
    const int exponent = (half >> 10) & 0x1f;
    const std::uint32_t mantissa = half & 0x3ff;
    if (exponent == 0)
        return std::copysign(std::ldexp(static_cast<float>(mantissa), -24), sign ? -1.0f : 1.0f);
    const std::uint32_t bits = sign | (exponent == 31 ? 0x7f800000u | (mantissa << 13) : static_cast<std::uint32_t>(exponent + 112) << 23 | mantissa << 13);
    return std::bit_cast<float>(bits);
}

std::vector<RgbaFloat> decodeFloatPixels(const TextureView& texture) {
    const size_t count = static_cast<size_t>(texture.width) * texture.height;
    switch (texture.format) {
    case PixelFormat::BC6H:
        return decodeBlocks<BCDEC_BC6H_BLOCK_SIZE, RgbaFloat>(texture, [](const std::byte* source, RgbaFloat* block) {
            float rgb[16 * 3];
            bcdec_bc6h_float(source, rgb, 4 * 3, 0);
            for (int i = 0; i < 16; ++i)
                block[i] = {rgb[i * 3], rgb[i * 3 + 1], rgb[i * 3 + 2], 1.0f};
        });
    case PixelFormat::FloatRGBA: {
        std::vector<RgbaFloat> pixels(count);
        std::vector<std::uint16_t> halves(count * 4);
        std::memcpy(halves.data(), texture.data.data(), halves.size() * 2);
        for (size_t i = 0; i < halves.size(); ++i)
            pixels[i / 4][i % 4] = halfToFloat(halves[i]);
        return pixels;
    }
    default:
        std::unreachable();
    }
}

std::vector<Rgba> decodePixels(const TextureView& texture) {
    constexpr int rgbaPitch = 4 * 4;
    switch (texture.format) {
    case PixelFormat::BC6H:
    case PixelFormat::FloatRGBA: {
        const std::vector<RgbaFloat> values = decodeFloatPixels(texture);
        std::vector<Rgba> pixels(values.size());
        for (size_t i = 0; i < values.size(); ++i)
            for (size_t c = 0; c < 4; ++c)
                pixels[i][c] = static_cast<std::uint8_t>(std::lround(std::clamp(values[i][c], 0.0f, 1.0f) * 255));
        return pixels;
    }
    case PixelFormat::BC1:
        return decodeBlocks<BCDEC_BC1_BLOCK_SIZE>(texture, [](const std::byte* source, Rgba* block) { bcdec_bc1(source, block, rgbaPitch); });
    case PixelFormat::BC3:
        return decodeBlocks<BCDEC_BC3_BLOCK_SIZE>(texture, [](const std::byte* source, Rgba* block) { bcdec_bc3(source, block, rgbaPitch); });
    case PixelFormat::BC7:
        return decodeBlocks<BCDEC_BC7_BLOCK_SIZE>(texture, [](const std::byte* source, Rgba* block) { bcdec_bc7(source, block, rgbaPitch); });
    case PixelFormat::BC4:
        return decodeBlocks<BCDEC_BC4_BLOCK_SIZE>(texture, [](const std::byte* source, Rgba* block) {
            std::uint8_t gray[16];
            bcdec_bc4(source, gray, 4);
            for (int i = 0; i < 16; ++i)
                block[i] = {gray[i], gray[i], gray[i], 255};
        });
    case PixelFormat::BC5:
        return decodeBlocks<BCDEC_BC5_BLOCK_SIZE>(texture, [](const std::byte* source, Rgba* block) {
            std::uint8_t redGreen[32];
            bcdec_bc5(source, redGreen, 8);
            for (int i = 0; i < 16; ++i)
                block[i] = {redGreen[i * 2], redGreen[i * 2 + 1], 0, 255};
        });
    case PixelFormat::B8G8R8A8: {
        std::vector<Rgba> pixels(static_cast<size_t>(texture.width) * texture.height);
        const auto* source = reinterpret_cast<const std::uint8_t*>(texture.data.data());
        for (Rgba& pixel : pixels) {
            pixel = {source[2], source[1], source[0], source[3]};
            source += 4;
        }
        return pixels;
    }
    case PixelFormat::R8G8B8A8: {
        std::vector<Rgba> pixels(static_cast<size_t>(texture.width) * texture.height);
        std::memcpy(pixels.data(), texture.data.data(), pixels.size() * 4);
        return pixels;
    }
    case PixelFormat::G8: {
        std::vector<Rgba> pixels(static_cast<size_t>(texture.width) * texture.height);
        const auto* source = reinterpret_cast<const std::uint8_t*>(texture.data.data());
        for (Rgba& pixel : pixels) {
            pixel = {*source, *source, *source, 255};
            ++source;
        }
        return pixels;
    }
    }
    std::unreachable();
}

std::vector<std::byte> bytesOf(const std::vector<Rgba>& pixels) {
    std::vector<std::byte> bytes(pixels.size() * 4);
    std::memcpy(bytes.data(), pixels.data(), bytes.size());
    return bytes;
}

}

Texture decodeToRgba(const TextureView& texture) {
    return {.format = PixelFormat::R8G8B8A8, .srgb = texture.srgb, .width = texture.width, .height = texture.height, .data = bytesOf(decodePixels(texture))};
}

Texture encodeToBc6h(const Texture& texture) {
    return {.format = PixelFormat::BC6H,
            .srgb = texture.srgb,
            .width = texture.width,
            .height = texture.height,
            .data = encodeBc6h(decodeFloatPixels(texture.view()), texture.width, texture.height)};
}

std::expected<bool, Core::Error> isCubeTexture(const std::string_view gamePath, const std::stop_token stop) {
    const std::expected<ue::UTexture*, Core::Error> texture = Assets::Reading::loadObject<ue::UTexture>(gamePath, "texture", stop);
    if (!texture)
        return std::unexpected(texture.error());
    return ue::Cast<ue::UTextureCube>(*texture) != nullptr;
}

std::expected<CubeTexture, Core::Error> readCube(const std::string_view gamePath, const std::stop_token stop) {
    const std::expected<ue::UTextureCube*, Core::Error> source = Assets::Reading::loadObject<ue::UTextureCube>(gamePath, "texture", stop);
    if (!source)
        return std::unexpected(source.error());
    const ue::FTexturePlatformData* data = (*source)->GetPlatformData();
    const int first = (*source)->GetFirstMipIndex();
    if (data == nullptr || first < 0)
        return Core::fail("texture.mip", std::format("'{}' has no mip with data.", gamePath));
    const std::expected<PixelFormat, Core::Error> format = formatOf(data->PixelFormat, gamePath);
    if (!format)
        return std::unexpected(format.error());
    const ue::FTexture2DMipMap& mip = data->Mips[first];
    const int size = mip.SizeX;
    const size_t faceBytes = storedSize(*format, size, size);
    if (size <= 0 || mip.SizeY != size || mip.BulkData.GetPayloadSize() != 6 * faceBytes)
        return Core::fail("texture.mip", std::format("The mip of '{}' does not hold six {}x{} faces.", gamePath, size, size));
    CubeTexture cube{.format = *format, .srgb = (*source)->SRGB, .size = size, .data = std::vector<std::byte>(6 * faceBytes)};
    if (stop.stop_requested())
        return Core::cancelled();
    if (const auto read = mip.BulkData.ReadInto(cube.data); !read)
        return fail("texture.mip", gamePath, read.error());
    return cube;
}

}
