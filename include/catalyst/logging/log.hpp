/**
 * @file log.hpp
 * @brief The functions the rest of the code calls.
 * @details
 *
 *     log::info("Catalyst {}", version);
 *     log::warn<vk_category>("frame {} was never ended", n);
 *
 * A category is a type with a static `name`, so a subsystem declares one once and every call names
 * it at compile time. Sinks and the router's per-category levels see the name as a string.
 *
 * The level is checked before the message is formatted: a debug call in a hot loop costs an atomic
 * load when debug is off. Below `compiled_level` there is not even that - the body of the call is
 * discarded at compile time. What a call below the floor still pays for is evaluating its own
 * arguments, which happens in the caller before any of this is reached, so an argument expensive
 * enough to matter belongs behind `enabled<level, category>()` rather than in the call.
 *
 * Every call goes through `default_logger()`. There is no per-subsystem router; audiences are
 * separated at the sink end with filters.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/logging/detail/call_site.hpp>
#include <catalyst/logging/event.hpp>
#include <catalyst/logging/level.hpp>
#include <catalyst/logging/router.hpp>

#include <any>
#include <concepts>
#include <format>
#include <source_location>
#include <string_view>
#include <type_traits>
#include <utility>

namespace catalyst::logging
{

    /**
     * @concept log_category
     * @brief A type that names a subsystem: anything with a static `name` that reads as a string.
     * @tparam C The candidate category type.
     */
    template <typename C>
    concept log_category = requires {
        { C::name } -> std::convertible_to<std::string_view>;
    };

    /**
     * @struct default_category
     * @brief The category of calls that do not name one.
     */
    struct default_category
    {
        static constexpr const char *name = "default";
    };

    /// The name of a category type, as sinks will see it.
    template <log_category C>
    [[nodiscard]] constexpr std::string_view category_name() noexcept
    {
        return static_cast<std::string_view>(C::name);
    }

    /// Whether a call at `Level` in `C` would reach any sink right now.
    template <log_level Level, log_category C = default_category>
    [[nodiscard]] bool enabled()
    {
        if constexpr (Level < compiled_level)
            return false;
        else
            return default_logger().should_log(Level, category_name<C>());
    }

    /**
     * @brief Hands an already-built event to the default router.
     * @param event The event; its timestamp and thread are filled in when left at their defaults.
     * @details For producers that have something structured to say - a validation pass's diagnostic -
     * and set `payload` themselves.
     */
    void emit(log_event event);

    /**
     * @brief Formats and emits an event with a payload attached, from an explicit call site.
     * @tparam Level The level to log at.
     * @tparam C The category.
     * @param payload The structured data to attach.
     * @param location The call site to record.
     * @param fmt The format string.
     * @param args The arguments to format.
     */
    template <log_level Level, log_category C, typename... Args>
    void log_with(std::any payload, std::source_location location, std::format_string<Args...> fmt, Args &&...args)
    {
        if constexpr (Level < compiled_level)
        {
            // Below the compile-time floor: an empty body the optimiser deletes. The parameters are
            // named here only so that naming them does not draw an unused-parameter warning.
            (void)payload;
            (void)location;
            (void)fmt;
            ((void)args, ...);
        }
        else
        {
            if (!enabled<Level, C>())
                return;
            emit(log_event{
                .level = Level,
                .category = category_name<C>(),
                .message = std::format(fmt, std::forward<Args>(args)...),
                .location = location,
                .payload = std::move(payload),
            });
        }
    }

    /// Formats and emits at a level and category from an explicit call site.
    template <log_level Level, log_category C, typename... Args>
    void log_at(std::source_location location, std::format_string<Args...> fmt, Args &&...args)
    {
        log_with<Level, C>(std::any{}, location, fmt, std::forward<Args>(args)...);
    }

    /// Formats and emits at a level and category, from wherever the format string was written.
    template <log_level Level, log_category C, typename... Args>
    void log(detail::format_with_location<std::type_identity_t<Args>...> fmt, Args &&...args)
    {
        log_at<Level, C>(fmt.location, fmt.fmt, std::forward<Args>(args)...);
    }

    // --- One function per level ------------------------------------------------------------

    template <log_category C = default_category, typename... Args>
    void trace(detail::format_with_location<std::type_identity_t<Args>...> fmt, Args &&...args)
    {
        log<log_level::trace, C>(fmt, std::forward<Args>(args)...);
    }

    template <log_category C = default_category, typename... Args>
    void debug(detail::format_with_location<std::type_identity_t<Args>...> fmt, Args &&...args)
    {
        log<log_level::debug, C>(fmt, std::forward<Args>(args)...);
    }

    template <log_category C = default_category, typename... Args>
    void info(detail::format_with_location<std::type_identity_t<Args>...> fmt, Args &&...args)
    {
        log<log_level::info, C>(fmt, std::forward<Args>(args)...);
    }

    template <log_category C = default_category, typename... Args>
    void warn(detail::format_with_location<std::type_identity_t<Args>...> fmt, Args &&...args)
    {
        log<log_level::warn, C>(fmt, std::forward<Args>(args)...);
    }

    template <log_category C = default_category, typename... Args>
    void error(detail::format_with_location<std::type_identity_t<Args>...> fmt, Args &&...args)
    {
        log<log_level::error, C>(fmt, std::forward<Args>(args)...);
    }

    template <log_category C = default_category, typename... Args>
    void fatal(detail::format_with_location<std::type_identity_t<Args>...> fmt, Args &&...args)
    {
        log<log_level::fatal, C>(fmt, std::forward<Args>(args)...);
    }

    template <log_category C = default_category, typename... Args>
    void critical(detail::format_with_location<std::type_identity_t<Args>...> fmt, Args &&...args)
    {
        log<log_level::critical, C>(fmt, std::forward<Args>(args)...);
    }

} // namespace catalyst::logging
