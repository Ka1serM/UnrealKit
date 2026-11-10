#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <vector>

namespace UnrealKit::Scene {

std::vector<std::byte> encodeBc6h(std::span<const std::array<float, 4>> pixels, int width, int height);

}
