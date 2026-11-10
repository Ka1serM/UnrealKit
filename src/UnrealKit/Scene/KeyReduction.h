#pragma once

#include <optional>
#include <string_view>
#include <utility>

namespace UnrealKit::Scene {

enum class KeyReduction { Off, Fine, Coarse };

constexpr float positionBudget(const KeyReduction reduction) {
    switch (reduction) {
    case KeyReduction::Off:
        return 0.0f;
    case KeyReduction::Fine:
        return 0.01f;
    case KeyReduction::Coarse:
        return 0.1f;
    }
    std::unreachable();
}

constexpr std::string_view keyReductionId(const KeyReduction reduction) {
    switch (reduction) {
    case KeyReduction::Off:
        return "off";
    case KeyReduction::Fine:
        return "fine";
    case KeyReduction::Coarse:
        return "coarse";
    }
    std::unreachable();
}

constexpr std::optional<KeyReduction> keyReductionOfId(const std::string_view id) {
    for (const KeyReduction reduction : {KeyReduction::Off, KeyReduction::Fine, KeyReduction::Coarse})
        if (keyReductionId(reduction) == id)
            return reduction;
    return std::nullopt;
}

}
