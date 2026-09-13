/**
 * @file call_site.hpp
 * @brief A checked format string that also remembers where it was written.
 * @details The level functions take this instead of a bare `std::format_string` so that the call
 * site is captured where the literal is converted - in the caller - rather than inside a logging
 * header, which is what a defaulted `source_location` parameter would give. The conversion is
 * `consteval`, as `std::format_string`'s is, so a malformed format string is still a compile error.
 *
 * Callers never name this type; it exists so that `log::info("...")` records the line it was
 * written on.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <concepts>
#include <format>
#include <source_location>
#include <string_view>

namespace catalyst::logging::detail
{

    /**
     * @struct format_with_location
     * @brief A `std::format_string` plus the source location of the literal it was built from.
     * @tparam Args The formatted argument types.
     */
    template <typename... Args>
    struct format_with_location
    {
        std::format_string<Args...> fmt;
        std::source_location location;

        template <typename S>
            requires std::convertible_to<const S &, std::string_view>
        consteval format_with_location(const S &s, std::source_location loc = std::source_location::current())
            : fmt(s), location(loc)
        {
        }
    };

} // namespace catalyst::logging::detail
