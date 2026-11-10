#pragma once

//! Emscripten's libc++ lacks std::move_only_function and GCC 12's libstdc++ lacks std::ranges::contains; the guards allow one copy per kit.

#include <algorithm>
#include <functional>
#include <memory>
#include <ranges>
#include <stop_token>
#include <type_traits>
#include <utility>
#include <version>

namespace std {

#if !defined(__cpp_lib_move_only_function) && !defined(STD_GAPS_MOVE_ONLY_FUNCTION)
#define STD_GAPS_MOVE_ONLY_FUNCTION

template <typename Signature>
class move_only_function;

#define WEB_MOVE_ONLY_FUNCTION(CONST)                                                                                                                          \
    template <typename R, typename... Args>                                                                                                                    \
    class move_only_function<R(Args...) CONST> {                                                                                                               \
    public:                                                                                                                                                    \
        move_only_function() = default;                                                                                                                        \
        move_only_function(nullptr_t) {}                                                                                                                       \
        template <typename F>                                                                                                                                  \
            requires(!is_same_v<remove_cvref_t<F>, move_only_function>)                                                                                        \
        move_only_function(F&& f) : callable(make_unique<Model<decay_t<F>>>(std::forward<F>(f))) {}                                                            \
        R operator()(Args... args) CONST { return callable->call(std::forward<Args>(args)...); }                                                               \
        explicit operator bool() const { return static_cast<bool>(callable); }                                                                                 \
                                                                                                                                                               \
    private:                                                                                                                                                   \
        struct Concept {                                                                                                                                       \
            virtual ~Concept() = default;                                                                                                                      \
            virtual R call(Args... args) CONST = 0;                                                                                                            \
        };                                                                                                                                                     \
        template <typename F>                                                                                                                                  \
        struct Model final : Concept {                                                                                                                         \
            explicit Model(F f) : f(std::move(f)) {}                                                                                                           \
            R call(Args... args) CONST override { return std::invoke(f, std::forward<Args>(args)...); }                                                        \
            mutable F f;                                                                                                                                       \
        };                                                                                                                                                     \
        unique_ptr<Concept> callable;                                                                                                                          \
    };

WEB_MOVE_ONLY_FUNCTION()
WEB_MOVE_ONLY_FUNCTION(const)

#undef WEB_MOVE_ONLY_FUNCTION

#endif

}

#if !defined(__cpp_lib_ranges_contains) && !defined(STD_GAPS_RANGES_CONTAINS)
#define STD_GAPS_RANGES_CONTAINS

namespace std::ranges {

inline constexpr auto contains = []<input_range R, typename T, typename Projection = identity>(R&& range, const T& value, Projection projection = {}) {
    return ranges::find(range, value, std::move(projection)) != ranges::end(range);
};

}

#endif
