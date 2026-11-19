#pragma once

#include <optional>
#include <string_view>

namespace UnrealKit::Transport {

enum class Importer { Blender, Unreal };

std::string_view importerName(Importer importer);
std::string_view importerId(Importer importer);
std::optional<Importer> importerOfId(std::string_view id);
std::string_view importerVersion();
int importerPort(Importer importer);

}
