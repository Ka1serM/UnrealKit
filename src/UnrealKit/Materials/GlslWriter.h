#pragma once

#include <string>
#include <vector>

#include "UnrealKit/Materials/MaterialProgram.h"

namespace UnrealKit::Materials {

extern const char* const glslFragmentInputs;
extern const char* const glslVertexOutputs;
extern const char* const glslCommon;

std::string glslMaterial(const MaterialProgram& program, const std::vector<bool>& srgb, const std::vector<bool>& perInstance);
std::string glslDisplacement(const MaterialProgram& program, const std::vector<bool>& srgb, const std::vector<bool>& perInstance);

}
