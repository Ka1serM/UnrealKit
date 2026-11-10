#pragma once

#include <expected>
#include <string>
#include <utility>

namespace UnrealKit::Core {

struct Error {
    std::string code;
    std::string message;

    bool isCancelled() const { return code == "cancelled"; }
};

[[nodiscard]] inline std::unexpected<Error> fail(std::string code, std::string message) {
    return std::unexpected(Error{std::move(code), std::move(message)});
}

[[nodiscard]] inline std::unexpected<Error> cancelled() {
    return fail("cancelled", "The job was cancelled.");
}

}
