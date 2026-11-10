#pragma once

#include <expected>

#include "Conversion/Materials/MaterialShaderCompiler.h"
#include "UnrealKit/Core/Error.h"
#include "UnrealKit/Materials/MaterialData.h"

namespace UnrealKit::Materials {

using CompiledShader = ue5parse::conversion::CompiledMaterialShader;

std::expected<CompiledShader, Core::Error> compileShader(const MaterialData& data, const ShaderData& shader);

}
