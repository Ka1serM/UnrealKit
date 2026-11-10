#include "UnrealKit/Scene/Bc6hEncoder.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <ranges>

namespace UnrealKit::Scene {

namespace {

constexpr std::array<int, 16> weights{0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
constexpr int endpointBits = 10;
constexpr int largestHalf = 0x7bff;

int halfBits(const float value) {
    const float clamped = std::clamp(std::isnan(value) ? 0.0f : value, 0.0f, 65504.0f);
    if (clamped < 6.103515625e-5f)
        return static_cast<int>(std::lround(clamped / 5.9604644775390625e-8f));
    const std::uint32_t bits = std::bit_cast<std::uint32_t>(clamped);
    const int exponent = static_cast<int>(bits >> 23) - 112;
    const std::uint32_t mantissa = bits & 0x7fffff;
    const int half = (exponent << 10) | static_cast<int>(mantissa >> 13);
    return std::min(largestHalf, half + static_cast<int>((mantissa >> 12) & 1));
}

int unquantize(const int value) {
    if (value == 0)
        return 0;
    if (value == (1 << endpointBits) - 1)
        return 0xffff;
    return ((value << 16) + 0x8000) >> endpointBits;
}

int finish(const int value) {
    return (value * 31) >> 6;
}

int quantize(const int half) {
    const int guess = static_cast<int>(std::lround(half * 1023.0 / largestHalf));
    int best = 0;
    for (int candidate = std::max(0, guess - 1); candidate <= std::min(1023, guess + 1); ++candidate)
        if (std::abs(finish(unquantize(candidate)) - half) < std::abs(finish(unquantize(best)) - half))
            best = candidate;
    return best;
}

struct Bits {
    std::array<std::uint64_t, 2> words{};
    int position = 0;

    void put(const std::uint64_t value, const int count) {
        for (int i = 0; i < count; ++i, ++position)
            words[static_cast<std::size_t>(position / 64)] |= ((value >> i) & 1) << (position % 64);
    }
};

std::array<std::byte, 16> encodeBlock(const std::array<std::array<int, 3>, 16>& halves) {
    std::array<std::array<int, 3>, 2> endpoints{};
    for (std::size_t c = 0; c < 3; ++c) {
        const auto [low, high] = std::ranges::minmax(halves | std::views::transform([c](const auto& pixel) { return pixel[c]; }));
        endpoints[0][c] = quantize(low);
        endpoints[1][c] = quantize(high);
    }
    std::array<int, 16> indices{};
    for (std::size_t p = 0; p < 16; ++p) {
        long best = std::numeric_limits<long>::max();
        for (int index = 0; index < 16; ++index) {
            long error = 0;
            for (std::size_t c = 0; c < 3; ++c) {
                const int a = unquantize(endpoints[0][c]), b = unquantize(endpoints[1][c]);
                const long difference = finish((a * (64 - weights[index]) + b * weights[index] + 32) >> 6) - halves[p][c];
                error += difference * difference;
            }
            if (error < best)
                best = error, indices[p] = index;
        }
    }
    if (indices[0] >= 8) {
        std::swap(endpoints[0], endpoints[1]);
        for (int& index : indices)
            index = 15 - index;
    }
    Bits bits;
    bits.put(0b00011, 5);
    for (const auto& endpoint : endpoints)
        for (const int value : endpoint)
            bits.put(static_cast<std::uint64_t>(value), endpointBits);
    for (std::size_t p = 0; p < 16; ++p)
        bits.put(static_cast<std::uint64_t>(indices[p]), p == 0 ? 3 : 4);
    std::array<std::byte, 16> block;
    std::memcpy(block.data(), bits.words.data(), block.size());
    return block;
}

}

std::vector<std::byte> encodeBc6h(const std::span<const std::array<float, 4>> pixels, const int width, const int height) {
    std::vector<std::byte> result;
    result.reserve(static_cast<std::size_t>((width + 3) / 4) * ((height + 3) / 4) * 16);
    for (int blockY = 0; blockY < height; blockY += 4)
        for (int blockX = 0; blockX < width; blockX += 4) {
            std::array<std::array<int, 3>, 16> halves;
            for (int row = 0; row < 4; ++row)
                for (int column = 0; column < 4; ++column) {
                    const int x = std::min(blockX + column, width - 1), y = std::min(blockY + row, height - 1);
                    const auto& pixel = pixels[static_cast<std::size_t>(y) * width + x];
                    for (std::size_t c = 0; c < 3; ++c)
                        halves[static_cast<std::size_t>(row * 4 + column)][c] = halfBits(pixel[c]);
                }
            const auto block = encodeBlock(halves);
            result.insert(result.end(), block.begin(), block.end());
        }
    return result;
}

}
