#pragma once

#include <optional>
#include <vector>

#include "Conversion/Materials/Surface.h"
#include "UnrealKit/Materials/ShaderCompiler.h"

namespace UnrealKit::Materials {

std::optional<ue5parse::conversion::SurfaceAttributes> debugViewSurface(ue5parse::conversion::MaterialEvaluator& evaluator,
                                                                        const std::vector<CompiledShader>& shaders, BlendMode blendMode);

}
