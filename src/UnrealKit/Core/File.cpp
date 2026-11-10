#include "UnrealKit/Core/File.h"

#include <cstdio>
#include <format>
#include <fstream>
#include <memory>
#include <system_error>

#include "UnrealKit/Core/Utf8Path.h"

namespace UnrealKit::Core {

namespace {

struct FileClose {
    void operator()(std::FILE* file) const noexcept { std::fclose(file); }
};

}

std::expected<std::vector<std::byte>, Core::Error> readFile(const std::filesystem::path& file) {
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(file, error);
    if (error)
        return fail("file.read", std::format("'{}' cannot be opened: {}", utf8(file), error.message()));
#if PLATFORM_WINDOWS
    const std::unique_ptr<std::FILE, FileClose> stream(_wfopen(file.c_str(), L"rb"));
#else
    const std::unique_ptr<std::FILE, FileClose> stream(std::fopen(file.c_str(), "rb"));
#endif
    if (!stream)
        return fail("file.read", std::format("'{}' cannot be opened.", utf8(file)));
    std::vector<std::byte> data(static_cast<std::size_t>(size));
    if (std::fread(data.data(), 1, data.size(), stream.get()) != data.size())
        return fail("file.read", std::format("'{}' cannot be read.", utf8(file)));
    return data;
}

std::expected<void, Core::Error> readFileAt(const std::filesystem::path& file, const std::uint64_t offset, const std::span<std::byte> out) {
#if PLATFORM_WINDOWS
    const std::unique_ptr<std::FILE, FileClose> stream(_wfopen(file.c_str(), L"rb"));
#else
    const std::unique_ptr<std::FILE, FileClose> stream(std::fopen(file.c_str(), "rb"));
#endif
    if (!stream)
        return fail("file.read", std::format("'{}' cannot be opened.", utf8(file)));
#if PLATFORM_WINDOWS
    const int seek = _fseeki64(stream.get(), static_cast<long long>(offset), SEEK_SET);
#else
    const int seek = fseeko(stream.get(), static_cast<off_t>(offset), SEEK_SET);
#endif
    if (seek != 0 || std::fread(out.data(), 1, out.size(), stream.get()) != out.size())
        return fail("file.read", std::format("{} bytes at {} of '{}' cannot be read.", out.size(), offset, utf8(file)));
    return {};
}

std::expected<void, Core::Error> replaceFile(const std::filesystem::path& file, const std::filesystem::path& temporary, const std::span<const std::byte> data) {
    std::error_code error;
    std::filesystem::create_directories(file.parent_path(), error);
    if (error)
        return fail("file.write", std::format("The folder '{}' cannot be made: {}", utf8(file.parent_path()), error.message()));
    {
        std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
        stream.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        if (!stream)
            return fail("file.write", std::format("'{}' cannot be written.", utf8(temporary)));
    }
    std::filesystem::rename(temporary, file, error);
    if (error)
        return fail("file.write", std::format("'{}' cannot be renamed to '{}': {}", utf8(temporary), utf8(file), error.message()));
    return {};
}

}
