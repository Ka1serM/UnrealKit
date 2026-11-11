#pragma once

#include <cstdint>
#include <expected>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include <nlohmann/json.hpp>

#include "CppUE5Parse/Core/Versions.h"
#include "UnrealKit/Core/Error.h"
#include "UnrealKit/Scene/KeyReduction.h"
#include "UnrealKit/Transport/Importer.h"

namespace UnrealKit::Config {

class ConfigFile {
public:
    explicit ConfigFile(std::filesystem::path path) : path(std::move(path)) {}

    //! %APPDATA%/<product>, $XDG_CONFIG_HOME/<product in lower case> or ~/.config/<product in lower case>, or the browser storage.
    static std::expected<std::filesystem::path, Core::Error> directory();
    //! The cache folder in the config folder, or the browser cache. In the browser, call it from a job.
    static std::expected<std::filesystem::path, Core::Error> cacheDirectory();

    const std::filesystem::path& getPath() const { return path; }

    //! The JSON object of the file, or nothing when the file does not exist.
    std::expected<std::optional<nlohmann::json>, Core::Error> read() const;
    std::expected<void, Core::Error> write(const nlohmann::json& json) const;

private:
    std::filesystem::path path;
};

//! Reads the keys of a JSON object of a config file. A key that is absent keeps its value. A key of a different type is an error.
class ConfigFields {
public:
    ConfigFields(const nlohmann::json& object, const std::filesystem::path& file) : object(object), file(file) {}

    ConfigFields fieldsOf(const nlohmann::json& child) const { return {child, file}; }
    const nlohmann::json* find(std::string_view key) const;
    std::unexpected<Core::Error> invalid(std::string_view key, std::string_view type) const;

    std::expected<void, Core::Error> read(const char* key, bool& value) const;
    std::expected<void, Core::Error> read(const char* key, float& value) const;
    std::expected<void, Core::Error> read(const char* key, std::uint32_t& value) const;
    std::expected<void, Core::Error> read(const char* key, std::string& value) const;
    std::expected<void, Core::Error> read(const char* key, std::filesystem::path& value) const;
    std::expected<void, Core::Error> read(const char* key, std::vector<int>& value) const;
    std::expected<void, Core::Error> read(const char* key, std::vector<std::filesystem::path>& value) const;
    std::expected<void, Core::Error> read(const char* key, std::set<std::string>& value) const;
    std::expected<void, Core::Error> read(const char* key, Transport::Importer& value) const;
    std::expected<void, Core::Error> read(const char* key, Scene::KeyReduction& value) const;
    std::expected<void, Core::Error> read(const char* key, ue5parse::EGame& value) const;

    static std::expected<void, Core::Error> firstError(std::initializer_list<std::expected<void, Core::Error>> results);

private:
    const nlohmann::json& object;
    const std::filesystem::path& file;
};

}
