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

int importerPort(const Product& product, const Importer importer) {
    return importer == Importer::Blender ? product.blenderPort : product.unrealPort;
}

}
