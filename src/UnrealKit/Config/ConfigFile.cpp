#include "UnrealKit/Config/ConfigFile.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <format>
#include <fstream>
#include <system_error>
#include <utility>

#include "UnrealKit/Core/Utf8Path.h"
#include "UnrealKit/Platform/BrowserFiles.h"

namespace UnrealKit::Config {

namespace {

[[maybe_unused]] std::filesystem::path environmentPath(const char* name) {
    const char* value = std::getenv(name);
    return value && *value ? std::filesystem::path(value) : std::filesystem::path();
}

[[maybe_unused]] std::string lowerCase(const std::string_view text) {
    std::string lower(text);
    std::ranges::transform(lower, lower.begin(), [](const unsigned char letter) { return char(std::tolower(letter)); });
    return lower;
}

}

std::expected<std::filesystem::path, Core::Error> ConfigFile::directory() {
#if PLATFORM_WINDOWS
    const std::filesystem::path base = environmentPath("APPDATA");
    if (base.empty())
        return Core::fail("config", "APPDATA is not set, so the config folder is unknown.");
    return base / PRODUCT_NAME;
#elif PLATFORM_LINUX
    if (const std::filesystem::path base = environmentPath("XDG_CONFIG_HOME"); !base.empty())
        return base / lowerCase(PRODUCT_NAME);
    const std::filesystem::path home = environmentPath("HOME");
    if (home.empty())
        return Core::fail("config", "Neither XDG_CONFIG_HOME nor HOME is set, so the config folder is unknown.");
    return home / ".config" / lowerCase(PRODUCT_NAME);
#elif PLATFORM_WEB
    return Platform::browserStorageDirectory;
#endif
}

std::expected<std::filesystem::path, Core::Error> ConfigFile::cacheDirectory() {
#if PLATFORM_WEB
    return Platform::openBrowserCacheDirectory();
#else
    return directory().transform([](const std::filesystem::path& directory) { return directory / "cache"; });
#endif
}

std::expected<std::optional<nlohmann::json>, Core::Error> ConfigFile::read() const {
    std::error_code error;
    if (!std::filesystem::exists(path, error)) {
        if (!error)
            return std::nullopt;
        return Core::fail("config", std::format("Cannot find the file '{}': {}", Core::utf8(path), error.message()));
    }
    std::ifstream stream(path);
    if (!stream)
        return Core::fail("config", std::format("Cannot open the file '{}'.", Core::utf8(path)));
    nlohmann::json json = nlohmann::json::parse(stream, nullptr, false);
    if (json.is_discarded())
        return Core::fail("config", std::format("The file '{}' is not JSON.", Core::utf8(path)));
    if (!json.is_object())
        return Core::fail("config", std::format("The file '{}' is not a JSON object.", Core::utf8(path)));
    return json;
}

std::expected<void, Core::Error> ConfigFile::write(const nlohmann::json& json) const {
    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error)
        return Core::fail("config", std::format("Cannot create the folder '{}': {}", Core::utf8(path.parent_path()), error.message()));
    std::filesystem::path temporary = path;
    temporary += ".tmp";
    const std::string contents = json.dump(4) + '\n';
    {
        std::ofstream output(temporary, std::ios::trunc);
        output << contents;
        if (!output)
            return Core::fail("config", std::format("Cannot write the file '{}'.", Core::utf8(temporary)));
    }
    std::filesystem::rename(temporary, path, error);
    if (error)
        return Core::fail("config", std::format("Cannot replace the file '{}': {}", Core::utf8(path), error.message()));
#if PLATFORM_WEB
    Platform::keepBrowserFile(path, contents);
#endif
    return {};
}

const nlohmann::json* ConfigFields::find(const std::string_view key) const {
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &*found;
}

std::unexpected<Core::Error> ConfigFields::invalid(const std::string_view key, const std::string_view type) const {
    return Core::fail("config", std::format("The file '{}' is not valid: '{}' is not {}.", Core::utf8(file), key, type));
}

std::expected<void, Core::Error> ConfigFields::read(const char* key, bool& value) const {
    const nlohmann::json* found = find(key);
    if (!found)
        return {};
    if (!found->is_boolean())
        return invalid(key, "true or false");
    value = found->get<bool>();
    return {};
}

std::expected<void, Core::Error> ConfigFields::read(const char* key, float& value) const {
    const nlohmann::json* found = find(key);
    if (!found)
        return {};
    if (!found->is_number())
        return invalid(key, "a number");
    value = found->get<float>();
    return {};
}

std::expected<void, Core::Error> ConfigFields::read(const char* key, std::uint32_t& value) const {
    const nlohmann::json* found = find(key);
    if (!found)
        return {};
    if (!found->is_number_unsigned())
        return invalid(key, "a whole number from 0");
    value = found->get<std::uint32_t>();
    return {};
}

std::expected<void, Core::Error> ConfigFields::read(const char* key, std::string& value) const {
    const nlohmann::json* found = find(key);
    if (!found)
        return {};
    if (!found->is_string())
        return invalid(key, "a string");
    value = found->get_ref<const std::string&>();
    return {};
}

std::expected<void, Core::Error> ConfigFields::read(const char* key, std::filesystem::path& value) const {
    const nlohmann::json* found = find(key);
    if (!found)
        return {};
    if (!found->is_string())
        return invalid(key, "a path");
    value = Core::pathFromUtf8(found->get_ref<const std::string&>().c_str());
    return {};
}

std::expected<void, Core::Error> ConfigFields::read(const char* key, std::vector<int>& value) const {
    const nlohmann::json* found = find(key);
    if (!found)
        return {};
    if (!found->is_array() || !std::ranges::all_of(*found, [](const nlohmann::json& number) { return number.is_number_integer(); }))
        return invalid(key, "a list of whole numbers");
    value = found->get<std::vector<int>>();
    return {};
}

std::expected<void, Core::Error> ConfigFields::read(const char* key, std::vector<std::filesystem::path>& value) const {
    const nlohmann::json* found = find(key);
    if (!found)
        return {};
    if (!found->is_array() || !std::ranges::all_of(*found, [](const nlohmann::json& path) { return path.is_string(); }))
        return invalid(key, "a list of paths");
    std::vector<std::filesystem::path> paths;
    paths.reserve(found->size());
    for (const nlohmann::json& path : *found)
        paths.push_back(Core::pathFromUtf8(path.get_ref<const std::string&>().c_str()));
    value = std::move(paths);
    return {};
}

std::expected<void, Core::Error> ConfigFields::read(const char* key, std::set<std::string>& value) const {
    const nlohmann::json* found = find(key);
    if (!found)
        return {};
    if (!found->is_array() || !std::ranges::all_of(*found, [](const nlohmann::json& string) { return string.is_string(); }))
        return invalid(key, "a list of strings");
    std::set<std::string> strings;
    for (const nlohmann::json& string : *found)
        strings.insert(string.get_ref<const std::string&>());
    value = std::move(strings);
    return {};
}

std::expected<void, Core::Error> ConfigFields::read(const char* key, Transport::Importer& value) const {
    const nlohmann::json* found = find(key);
    if (!found)
        return {};
    const std::optional<Transport::Importer> importer = found->is_string() ? Transport::importerOfId(found->get_ref<const std::string&>()) : std::nullopt;
    if (!importer)
        return invalid(key, "\"blender\" or \"unreal\"");
    value = *importer;
    return {};
}

std::expected<void, Core::Error> ConfigFields::read(const char* key, ue5parse::EGame& value) const {
    const nlohmann::json* found = find(key);
    if (!found)
        return {};
    const std::optional<ue5parse::EGame> game = found->is_string() ? ue5parse::gameOfName(found->get_ref<const std::string&>()) : std::nullopt;
    if (!game)
        return invalid(key, "a game preset such as \"GAME_UE5_4\"");
    value = *game;
    return {};
}

std::expected<void, Core::Error> ConfigFields::read(const char* key, Scene::KeyReduction& value) const {
    const nlohmann::json* found = find(key);
    if (!found)
        return {};
    const std::optional<Scene::KeyReduction> reduction = found->is_string() ? Scene::keyReductionOfId(found->get_ref<const std::string&>()) : std::nullopt;
    if (!reduction)
        return invalid(key, "\"off\", \"fine\" or \"coarse\"");
    value = *reduction;
    return {};
}

std::expected<void, Core::Error> ConfigFields::firstError(const std::initializer_list<std::expected<void, Core::Error>> results) {
    for (const std::expected<void, Core::Error>& result : results)
        if (!result)
            return result;
    return {};
}

}
