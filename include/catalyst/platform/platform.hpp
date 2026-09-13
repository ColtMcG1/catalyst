/*
 * @file platform.hpp
 * @brief Main header for Catalyst Platform library, including all platform components.
 * @details This header serves as the primary include for the Catalyst Platform library, which provides a collection of platform utilities and types commonly used in game development and real-time applications. By including this single header, users can access all the functionality provided by the Catalyst Platform library without needing to include individual component headers. The library is designed to be efficient, easy to use, and compatible with modern C++ standards.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/math/vector.hpp>

#include <cstdint>

/**
 * @namespace catalyst::platform
 * @brief The catalyst::platform namespace contains all the platform-specific types and functions provided by the Catalyst Platform library. This includes window management, event handling, and other platform-related utilities. By organizing all platform-specific functionality within this namespace, we can avoid naming conflicts and provide a clear structure for users of the library to access the various platform tools they need for their applications. The Catalyst Platform library is designed to be efficient, easy to use, and compatible with modern C++ standards, making it a valuable resource for developers building real-time applications and games.
 * @details The Catalyst Platform library provides a collection of platform utilities and types commonly used in game development and real-time applications. By including the appropriate headers from the Catalyst Platform library, users can access all the functionality they need for their projects, such as creating windows, handling events, and interacting with the underlying operating system in a platform-agnostic way. This allows developers to focus on building their applications without worrying about the complexities of platform-specific code, while still having access to powerful tools for managing windows and events effectively.
 */
namespace catalyst::platform
{

    /**
     * @struct rect_px
     * @brief An axis-aligned rectangle in whole pixels, stored as its two corners.
     * @details The math library deals in vectors and matrices and no longer carries a rectangle type, so the platform
     * module defines the one shape it needs for window and monitor bounds. Pixels are integral because that is what the
     * operating system reports: a window's client area and a monitor's bounds are never fractional. `min` is the top-left
     * corner and `max` the bottom-right, with y increasing downwards, matching every windowing system Catalyst targets.
     */
    struct rect_px
    {
        /**
         * @brief The top-left corner, in pixels.
         */
        math::vec2<std::int32_t> min{};

        /**
         * @brief The bottom-right corner, in pixels.
         */
        math::vec2<std::int32_t> max{};

        /**
         * @fn size
         * @brief Returns the width and height of the rectangle, in pixels.
         */
        [[nodiscard]] constexpr math::vec2<std::int32_t> size() const noexcept
        {
            return math::vec2<std::int32_t>{max.x() - min.x(), max.y() - min.y()};
        }

        /**
         * @fn width
         * @brief Returns the width of the rectangle, in pixels.
         */
        [[nodiscard]] constexpr std::int32_t width() const noexcept { return max.x() - min.x(); }

        /**
         * @fn height
         * @brief Returns the height of the rectangle, in pixels.
         */
        [[nodiscard]] constexpr std::int32_t height() const noexcept { return max.y() - min.y(); }

        /**
         * @fn is_empty
         * @brief Whether the rectangle encloses no pixels.
         */
        [[nodiscard]] constexpr bool is_empty() const noexcept { return !(max.x() > min.x() && max.y() > min.y()); }

        /**
         * @brief Compares two rectangles corner by corner.
         */
        [[nodiscard]] constexpr bool operator==(const rect_px &other) const noexcept = default;
    };

    /**
     * @fn module_name
     * @brief Returns the name of this module as a string. This can be used for logging, debugging, or any situation where you want to identify the module by name.
     * @return A string literal representing the name of this module.
     */
    const char *module_name();

} // namespace catalyst::platform
