#include "UnrealKit/Transport/ImporterInstall.h"

#include <charconv>
#include <cstdlib>
#include <format>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#include "UnrealKit/Core/File.h"
#include "UnrealKit/Core/Utf8Path.h"

namespace UnrealKit::Transport {
namespace {

std::filesystem::path environmentPath(const char* name) {
    const char* value = std::getenv(name);
    return value != nullptr && *value != '\0' ? Core::pathFromUtf8(value) : std::filesystem::path();
}

std::filesystem::path blenderConfigDirectory() {
#if PLATFORM_WINDOWS
    const std::filesystem::path appData = environmentPath("APPDATA");
    return appData.empty() ? appData : appData / "Blender Foundation" / "Blender";
#else
    if (const std::filesystem::path config = environmentPath("XDG_CONFIG_HOME"); !config.empty())
        return config / "blender";
    const std::filesystem::path home = environmentPath("HOME");
    return home.empty() ? home : home / ".config" / "blender";
#endif
}

//! The major and minor number of a Blender config folder such as `4.5`.
std::optional<std::pair<int, int>> blenderVersion(const std::string& name) {
    int major = 0;
    int minor = 0;
    const char* end = name.data() + name.size();
    const auto [dot, majorError] = std::from_chars(name.data(), end, major);
    if (majorError != std::errc() || dot == end || *dot != '.')
        return std::nullopt;
    const auto [last, minorError] = std::from_chars(dot + 1, end, minor);
    if (minorError != std::errc() || last != end)
        return std::nullopt;
    return std::pair{major, minor};
}

std::expected<void, Core::Error> replaceFolder(const std::filesystem::path& folder, const std::span<const ImporterFile> files) {
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    if (error)
        return Core::fail("importer.install", std::format("'{}' cannot be removed: {}", Core::utf8(folder), error.message()));
    for (const ImporterFile& file : files) {
        const std::filesystem::path target = folder / file.path;
        std::filesystem::create_directories(target.parent_path(), error);
        if (error)
            return Core::fail("importer.install", std::format("'{}' cannot be created: {}", Core::utf8(target.parent_path()), error.message()));
        std::filesystem::path temporary = target;
        temporary += ".part";
        if (auto written = Core::replaceFile(target, temporary, file.bytes); !written)
            return written;
    }
    return {};
}

}

std::expected<std::filesystem::path, Core::Error> installUnrealPlugin(const std::filesystem::path& project, const std::string_view name,
                                                                      const std::span<const ImporterFile> files) {
    if (project.extension() != ".uproject")
        return Core::fail("importer.install", std::format("'{}' is not an Unreal project file (.uproject).", Core::utf8(project)));
    const std::filesystem::path folder = project.parent_path() / "Plugins" / name;
    return replaceFolder(folder, files).transform([&] { return folder; });
}

std::expected<std::vector<std::filesystem::path>, Core::Error> installBlenderExtension(const std::string_view id, const std::span<const ImporterFile> files) {
    const std::filesystem::path config = blenderConfigDirectory();
    if (config.empty())
        return Core::fail("importer.install", "The Blender config folder is unknown, because its environment variable is not set.");
    std::vector<std::filesystem::path> installed;
    std::error_code error;
    for (auto entry = std::filesystem::directory_iterator(config, error); !error && entry != std::filesystem::directory_iterator(); entry.increment(error)) {
        const auto version = blenderVersion(entry->path().filename().string());
        if (!entry->is_directory() || !version || *version < std::pair{4, 2})
            continue;
        const std::filesystem::path folder = entry->path() / "extensions" / "user_default" / id;
        if (auto replaced = replaceFolder(folder, files); !replaced)
            return std::unexpected(std::move(replaced.error()));
        installed.push_back(folder);
    }
    if (installed.empty())
        return Core::fail("importer.install", std::format("'{}' has no Blender 4.2 or later. Start Blender once, then install again.", Core::utf8(config)));
    return installed;
}

}
