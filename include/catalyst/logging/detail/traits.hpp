/**
 * @file traits.hpp
 * @brief Small type-level helpers shared by the logging headers.
 * @details Nothing here is part of the module's interface. What is left is inline because it is
 * used from a template that has to stay in a header: a transparent hash, so the router's category
 * map can be looked up with a `string_view` without allocating a `std::string` to do it.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <cstddef>
#include <functional>
#include <string_view>

namespace catalyst::logging::detail
{

    /**
     * @struct string_hash
     * @brief Hashes strings and string views alike, so a map keyed by `std::string` can be looked up
     * with a `std::string_view` without allocating one.
     */
    struct string_hash
    {
        using is_transparent = void;

        [[nodiscard]] std::size_t operator()(std::string_view s) const noexcept
        {
            return std::hash<std::string_view>{}(s);
        }
    };

} // namespace catalyst::logging::detail
