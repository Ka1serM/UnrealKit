#include "UnrealKit/Transport/Importer.h"

namespace UnrealKit::Transport {

std::string_view importerName(const Importer importer) {
    return importer == Importer::Blender ? "Blender" : "Unreal Editor";
}

std::string_view importerId(const Importer importer) {
    return importer == Importer::Blender ? "blender" : "unreal";
}

std::optional<Importer> importerOfId(const std::string_view id) {
    if (id == "blender")
        return Importer::Blender;
    if (id == "unreal")
        return Importer::Unreal;
    return std::nullopt;
}

std::string_view importerVersion() {
    return UNREALKIT_VERSION;
}

int importerPort(const Importer importer) {
    return importer == Importer::Blender ? UNREALKIT_BLENDER_PORT : UNREALKIT_UNREAL_PORT;
}

}
