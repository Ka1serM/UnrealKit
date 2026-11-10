#pragma once

#include <optional>
#include <string_view>

namespace UnrealKit::Transport {

enum class Importer { Blender, Unreal };

struct Product {
    std::string_view name;
    std::string_view version;
    int blenderPort;
    int unrealPort;
};

std::string_view importerName(Importer importer);
std::string_view importerId(Importer importer);
std::optional<Importer> importerOfId(std::string_view id);
int importerPort(const Product& product, Importer importer);

}
