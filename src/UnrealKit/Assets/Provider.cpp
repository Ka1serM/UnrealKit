#include "UnrealKit/Assets/Provider.h"

#include <array>
#include <charconv>
#include <cstdint>
#include <format>
#include <optional>

#include "CppUE5Parse/Crypto/Decryptor.h"
#include "UnrealKit/Assets/Reading.h"
#include "UnrealKit/Platform/FileSource.h"

namespace UnrealKit::Assets {

std::expected<std::vector<std::unique_ptr<ue5parse::FileSource>>, Core::Error> openArchiveDirectory(const std::filesystem::path& directory) {
    std::vector<std::unique_ptr<ue5parse::FileSource>> archives;
    std::error_code error;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(directory, error)) {
        const std::filesystem::path extension = entry.path().extension();
        if (!entry.is_regular_file() || (extension != ".pak" && extension != ".utoc" && extension != ".ucas"))
            continue;
        auto source = Platform::openFileSource(entry.path());
        if (!source)
            return Reading::fail("mount.archive_directory", entry.path().string(), source.error());
        archives.push_back(std::move(*source));
    }
    if (error)
        return Core::fail("mount.archive_directory", std::format("'{}': {}", directory.string(), error.message()));
    return archives;
}

namespace {

std::optional<ue5parse::FGuid> parseGuid(const std::string_view text) {
    if (text.size() != 32)
        return std::nullopt;
    std::array<std::uint32_t, 4> parts{};
    for (std::size_t i = 0; i < parts.size(); ++i) {
        const std::string_view part = text.substr(i * 8, 8);
        if (std::from_chars(part.data(), part.data() + part.size(), parts[i], 16).ptr != part.data() + part.size())
            return std::nullopt;
    }
    return ue5parse::FGuid{parts[0], parts[1], parts[2], parts[3]};
}

}

std::expected<void, Core::Error> Provider::mount(MountOptions options, const std::stop_token stop) {
    if (options.archives.empty())
        return Core::fail("mount.no_files", "The game has no readable archives.");
    ue5parse::MountOptions input;
    input.game = options.game;
    input.archives = std::move(options.archives);

    if (!options.mappings.empty()) {
        auto mappings = Platform::openFileSource(options.mappings);
        if (!mappings)
            return Reading::fail("mount.mappings", options.mappings.string(), mappings.error());
        input.mappings = std::move(*mappings);
    }
    ue5parse::KeyRing keys;
    //! An empty key is no key: the game does not encrypt its archives, and an encrypted one fails below with MissingKey.
    if (!options.aesKey.empty()) {
        auto key = ue5parse::parseAesKey(options.aesKey);
        if (!key)
            return Core::fail("mount.aes_key", "The AES key is not 64 hexadecimal digits.");
        keys.add(ue5parse::FGuid{}, *key);
    }
    for (const ArchiveKey& extra : options.extraKeys) {
        const auto guid = parseGuid(extra.guid);
        if (!guid)
            return Core::fail("mount.aes_key", std::format("The key GUID '{}' is not 32 hexadecimal digits.", extra.guid));
        const auto parsed = ue5parse::parseAesKey(extra.key);
        if (!parsed)
            return Core::fail("mount.aes_key", std::format("The AES key for '{}' is not 64 hexadecimal digits.", extra.guid));
        keys.add(*guid, *parsed);
    }
    input.decryptor = std::make_unique<ue5parse::AesDecryptor>(keys);

    auto game = ue5parse::mount(std::move(input), stop);
    if (!game)
        return Reading::fail("mount.initialize", "the game", game.error());
    for (const ue5parse::ArchiveProblem& problem : game->files.getProblems()) {
        const bool keyProblem = problem.error.code == ue5parse::ErrorCode::MissingKey || problem.error.code == ue5parse::ErrorCode::BadAesKey;
        if (keyProblem)
            return Reading::fail("mount.aes_key", problem.name, problem.error);
    }
    fileCount = static_cast<int>(game->files.getFiles().size());
    if (fileCount == 0)
        return Core::fail("mount.no_files", "The archives mounted with no files.");
    files = std::make_unique<ue5parse::FileProvider>(std::move(*game));
    runtime.emplace(*files);
    shaderMaterials = std::make_unique<ShaderMaterials>(*files, std::move(options.debugViewShader));
    return {};
}

}
