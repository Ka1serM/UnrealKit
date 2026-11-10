#include "UnrealKit/Materials/ShaderCompiler.h"

#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "UnrealKit/Assets/Reading.h"

namespace UnrealKit::Materials {

void dumpSpirVToEnvDir(std::string_view owner, std::string_view type, std::span<const std::uint32_t> spirv) {
    const char* dir = std::getenv("UNREALKIT_DUMP_SPIRV");
    if (!dir)
        return;
    std::string name = std::string(owner) + "_" + std::string(type);
    for (char& c : name)
        if (c == '/' || c == '.' || c == ' ')
            c = '_';
    std::ofstream(std::string(dir) + "/" + name + ".spv", std::ios::binary)
        .write(reinterpret_cast<const char*>(spirv.data()), static_cast<std::streamsize>(spirv.size() * 4));
}

std::expected<CompiledShader, Core::Error> compileShader(const MaterialData& data, const ShaderData& shader) {
    auto compiled = ue5parse::conversion::compileMaterialShader(data, shader);
    if (compiled) {
        dumpSpirVToEnvDir(data.owner, shader.type, {compiled->spirv.data(), compiled->spirv.size()});
        return std::move(*compiled);
    }
    const ue5parse::conversion::ConversionError& error = compiled.error();
    if (error.cause)
        return Assets::Reading::fail("material.shader", data.owner, *error.cause);
    return Core::fail("material.shader", error.message);
}

}
