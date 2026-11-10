#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "scene_generated.h"

namespace UnrealKit::Materials {

struct MaterialProgram {
    struct Parameter {
        std::string name;
        std::vector<float> value;
    };
    struct Texture {
        std::string image;
        std::string name;
        scene::stream::TextureAddress addressU = scene::stream::TextureAddress::Wrap;
        scene::stream::TextureAddress addressV = scene::stream::TextureAddress::Wrap;
        bool nearest = false;
    };

    std::vector<scene::stream::MaterialInstruction> instructions;
    std::vector<float> constants;
    std::vector<Parameter> parameters;
    std::vector<Texture> textures;
    uint32_t baseColor = 0;
    uint32_t normal = 0;
    uint32_t emission = 0;
    uint32_t roughness = 0;
    uint32_t metallic = 0;
    uint32_t specular = 0;
    int32_t opacity = -1;
    int32_t refraction = -1;
    int32_t displacement = -1;
};

}
