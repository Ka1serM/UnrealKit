#include "UnrealKit/Assets/Mappings.h"

#include <cstdint>
#include <cstring>
#include <format>
#include <system_error>

#include <zstd.h>

#include "UsmapGen/UsmapGen.h"
#include "UnrealKit/Core/File.h"

namespace UnrealKit::Assets {

namespace {

class MemorySource final : public UsmapGen::Source {
public:
    MemorySource(const std::span<const char> file, const std::stop_token stop) : file(file), stop(stop) {}

    std::uint64_t getSize() const noexcept override { return file.size(); }

    std::expected<void, UsmapGen::Error> read(const std::uint64_t offset, const std::span<std::byte> buffer) const noexcept override {
        if (stop.stop_requested())
            return std::unexpected(UsmapGen::Error{"The job was cancelled."});
        std::memcpy(buffer.data(), file.data() + offset, buffer.size());
        return {};
    }

private:
    std::span<const char> file;
    std::stop_token stop;
};

class LocalSource final : public UsmapGen::Source {
public:
    LocalSource(const ue5parse::FileSource& file, const std::stop_token stop) : file(file), stop(stop) {}

    std::uint64_t getSize() const noexcept override { return file.getSize(); }

    std::expected<void, UsmapGen::Error> read(const std::uint64_t offset, const std::span<std::byte> buffer) const noexcept override {
        if (stop.stop_requested())
            return std::unexpected(UsmapGen::Error{"The job was cancelled."});
        const ue5parse::Result<void> read = file.read(offset, buffer);
        if (!read)
            return std::unexpected(UsmapGen::Error{std::format("{} at {}", ue5parse::getErrorName(read.error().code), read.error().where)});
        return {};
    }

private:
    const ue5parse::FileSource& file;
    std::stop_token stop;
};

//! UsmapGen writes the header without versioning: magic, version, bHasVersioning, compression method, compressed size, size.
constexpr std::size_t usmapHeaderSize = 2 + 1 + 4 + 1 + 4 + 4;
constexpr std::size_t usmapCompressionOffset = 7;
constexpr std::uint8_t usmapCompressionNone = 0;
constexpr std::uint8_t usmapCompressionZstandard = 3;
//! Close to the ratio of the maximum level, at a fraction of its time. The file compresses once a release.
constexpr int usmapCompressionLevel = 19;

std::expected<std::vector<std::byte>, Core::Error> compressUsmap(const std::span<const std::byte> usmap) {
    if (usmap.size() < usmapHeaderSize || std::uint8_t(usmap[usmapCompressionOffset]) != usmapCompressionNone)
        return Core::fail("mappings.compress", "UsmapGen did not write an uncompressed usmap without versioning.");
    const std::span<const std::byte> body = usmap.subspan(usmapHeaderSize);
    std::vector<std::byte> compressed(usmapHeaderSize + ZSTD_compressBound(body.size()));
    const std::size_t size =
        ZSTD_compress(compressed.data() + usmapHeaderSize, compressed.size() - usmapHeaderSize, body.data(), body.size(), usmapCompressionLevel);
    if (ZSTD_isError(size))
        return Core::fail("mappings.compress", std::format("Zstd does not compress the mappings: {}", ZSTD_getErrorName(size)));
    compressed.resize(usmapHeaderSize + size);
    std::memcpy(compressed.data(), usmap.data(), usmapCompressionOffset);
    compressed[usmapCompressionOffset] = std::byte{usmapCompressionZstandard};
    const std::uint32_t sizes[] = {std::uint32_t(size), std::uint32_t(body.size())};
    std::memcpy(compressed.data() + usmapCompressionOffset + 1, sizes, sizeof(sizes));
    return compressed;
}

std::expected<std::vector<std::byte>, Core::Error> generate(const UsmapGen::Source& executable, const std::stop_token stop) {
    const std::expected<std::vector<std::byte>, UsmapGen::Error> mappings = UsmapGen::generateUsmap(executable);
    if (stop.stop_requested())
        return Core::cancelled();
    if (!mappings)
        return Core::fail("mappings", std::format("The mappings do not generate from the game executable: {}", mappings.error().message));
    return compressUsmap(*mappings);
}

}

std::expected<std::vector<std::byte>, Core::Error> generateMappings(const ue5parse::FileSource& executable, const std::stop_token stop) {
    return generate(LocalSource(executable, stop), stop);
}

std::expected<std::vector<std::byte>, Core::Error> generateMappings(const std::span<const char> executable, const std::stop_token stop) {
    return generate(MemorySource(executable, stop), stop);
}

std::expected<std::filesystem::path, Core::Error> cachedMappings(const std::filesystem::path& file, const std::function<std::expected<std::vector<std::byte>, Core::Error>()>& generate) {
    std::error_code error;
    if (std::filesystem::exists(file, error))
        return file;
    const std::expected<std::vector<std::byte>, Core::Error> mappings = generate();
    if (!mappings)
        return std::unexpected(mappings.error());
    for (const std::filesystem::directory_entry& entry : std::filesystem::directory_iterator(file.parent_path(), error))
        if (entry.path().extension() == ".usmap")
            std::filesystem::remove(entry.path(), error);
    std::filesystem::path temporary = file;
    temporary += ".tmp";
    if (std::expected<void, Core::Error> written = Core::replaceFile(file, temporary, *mappings); !written)
        return std::unexpected(written.error());
    return file;
}

}
