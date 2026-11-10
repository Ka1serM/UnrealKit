#include "UnrealKit/Assets/Listing.h"

#include <algorithm>
#include <array>
#include <format>

namespace UnrealKit::Assets {

std::string objectPathOf(std::string_view filePath) {
    while (filePath.starts_with("../"))
        filePath.remove_prefix(3);
    if (filePath.starts_with('/'))
        filePath.remove_prefix(1);
    const std::size_t content = filePath.find("/Content/");
    const std::size_t dot = filePath.rfind('.');
    if (content == std::string_view::npos || dot == std::string_view::npos || dot < content)
        return {};
    const std::string_view owner = filePath.substr(0, content);
    const std::size_t ownerSlash = owner.rfind('/');
    const std::string_view ownerName = ownerSlash == std::string_view::npos ? owner : owner.substr(ownerSlash + 1);
    const bool plugin = owner.find("/Plugins/") != std::string_view::npos;
    const std::string_view root = plugin || ownerName == "Engine" ? ownerName : std::string_view("Game");
    const std::string_view package = filePath.substr(content + 9, dot - content - 9);
    const std::size_t slash = package.rfind('/');
    const std::string_view name = slash == std::string_view::npos ? package : package.substr(slash + 1);
    return std::format("/{}/{}.{}", root, package, name);
}

std::vector<std::string> listMaps(const ue5parse::FileProvider& files) {
    std::vector<std::string> maps;
    std::array<char, 1024> buffer{};
    const ue5parse::FileSystem& system = files.getFiles();
    for (const ue5parse::FileEntry& entry : system.getFiles()) {
        if (!system.getFileName(entry).ends_with(".umap"))
            continue;
        std::string path = objectPathOf(system.writePath(entry, buffer));
        if (!path.empty())
            maps.push_back(std::move(path));
    }
    std::ranges::sort(maps);
    const auto [first, last] = std::ranges::unique(maps);
    maps.erase(first, last);
    return maps;
}

}
