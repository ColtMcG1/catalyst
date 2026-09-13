/**
 * @file level.hpp
 * @brief The severity levels a log event can carry, and their names.
 * @details The names are the spelling used by every text sink and by configuration files, so the
 * conversions live here rather than in a sink. Both are `constexpr` on purpose: a category that
 * wants a compile-time default level, or a table of levels built at compile time, needs them.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <cstdint>
#include <format>
#include <optional>
#include <string_view>

namespace catalyst::logging
{

    /**
     * @enum log_level
     * @brief How serious an event is, in increasing order.
     * @details The order is what filtering compares against: a sink or router set to `warn` passes
     * `warn` and everything after it.
     *
     * The top three are distinguished by what is still standing after them, which is also what
     * decides whether an event is worth flushing the sinks for: an `error` is a failed operation in
     * a subsystem that carries on, a `fatal` is a subsystem that cannot, and a `critical` is a
     * process that cannot. A router flushes from `fatal` up by default, so the last thing written
     * before a process gives up is on disk rather than in a buffer.
     */
    enum class log_level : std::uint8_t
    {
        trace,    ///< Step-by-step detail, off outside a debugging session.
        debug,    ///< What a developer wants while working on the subsystem itself.
        info,     ///< The ordinary course of events: what started, what loaded, what was chosen.
        warn,     ///< Something is wrong, and the work continues.
        error,    ///< An operation failed. Its subsystem carries on.
        fatal,    ///< A subsystem cannot continue.
        critical, ///< The process cannot continue.
    };

// The compile-time floor. Define CATALYST_LOG_COMPILED_LEVEL to the name of a level - the build
// carries a CATALYST_LOG_COMPILED_LEVEL cache variable for it - and every call below that level
// becomes an empty function that the optimiser deletes, rather than a run-time check.
#ifndef CATALYST_LOG_COMPILED_LEVEL
#  define CATALYST_LOG_COMPILED_LEVEL trace
#endif

    /**
     * @brief The level below which log calls are compiled out entirely.
     * @details This has to agree across every translation unit that logs: the level functions are
     * templates, and two of them compiled against different floors are two different definitions of
     * the same function. The build defines it on the module's public interface for that reason, so
     * a consumer picks the floor up from linking `catalyst::logging` rather than by remembering to
     * define it.
     */
    inline constexpr log_level compiled_level = log_level::CATALYST_LOG_COMPILED_LEVEL;

    /**
     * @brief The display name of a level.
     * @param level The level to name.
     * @return "trace", "debug", "info", "warn", "error", "fatal" or "critical".
     */
    [[nodiscard]] constexpr std::string_view name(log_level level) noexcept
    {
        switch (level)
        {
        case log_level::trace:
            return "trace";
        case log_level::debug:
            return "debug";
        case log_level::info:
            return "info";
        case log_level::warn:
            return "warn";
        case log_level::error:
            return "error";
        case log_level::fatal:
            return "fatal";
        case log_level::critical:
            return "critical";
        }
        return "unknown";
    }

    /**
     * @brief The level with a given name.
     * @param text The name to look up, case-sensitive. "warning" is accepted as a spelling of `warn`.
     * @return The level, or `std::nullopt` for anything else.
     */
    [[nodiscard]] constexpr std::optional<log_level> parse_level(std::string_view text) noexcept
    {
        for (const log_level level : {log_level::trace, log_level::debug, log_level::info, log_level::warn,
                                      log_level::error, log_level::fatal, log_level::critical})
            if (name(level) == text)
                return level;
        if (text == "warning")
            return log_level::warn;
        return std::nullopt;
    }

} // namespace catalyst::logging

/**
 * @brief Formats a level as its name, so `std::format("[{}] {}", level, text)` works.
 * @details Inherits the string formatter's specification, so width, fill and alignment apply:
 * `"{:<8}"` gives the padded column a text sink wants.
 */
template <>
struct std::formatter<catalyst::logging::log_level> : std::formatter<std::string_view>
{
    template <typename Context>
    auto format(catalyst::logging::log_level level, Context &ctx) const
    {
        return std::formatter<std::string_view>::format(catalyst::logging::name(level), ctx);
    }
};
