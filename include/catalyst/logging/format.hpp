/**
 * @file format.hpp
 * @brief Turning an event into one line of text.
 * @details Every text sink shares this: `line_format` says which columns are printed in front of the
 * message, and `format_line` builds the line. Sinks that write somewhere structured - a ring buffer,
 * a GUI panel - keep the event itself and never come here.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/logging/event.hpp>

#include <array>
#include <chrono>
#include <cstddef>
#include <source_location>
#include <string>
#include <string_view>

namespace catalyst::logging
{

    /**
     * @struct line_format
     * @brief Which columns a text sink prints, in front of the message.
     */
    struct line_format
    {
        bool time = true;           ///< "13:05:42.117"
        bool level = true;          ///< "warn", padded to the widest name
        bool category = true;       ///< "[vulkan]"
        bool thread = false;        ///< The logging thread's id, for multi-threaded traces.
        bool location = false;      ///< "context.cpp:378" on every line ...
        bool error_location = true; ///< ... or only on errors and above.

        [[nodiscard]] bool operator==(const line_format &) const noexcept = default;
    };

    /**
     * @struct line_style
     * @brief The escape sequences a text sink wraps each column in.
     * @details Every member is the sequence that turns a colour on; `reset` turns it off again, and
     * is only emitted after a column whose sequence is non-empty. A default-constructed style is
     * therefore plain text, which is what a file wants - only a sink that knows its destination
     * understands escape sequences asks for `ansi()`.
     *
     * The strings are `string_view`s, so a palette built from anything but literals or otherwise
     * static storage has to outlive the formatting call.
     */
    struct line_style
    {
        std::string_view reset{};    ///< Emitted after any column that was coloured.
        std::string_view time{};     ///< "13:05:42.117"
        std::string_view category{}; ///< "[vulkan]"
        std::string_view thread{};   ///< "(4f21)"
        std::string_view location{}; ///< "(context.cpp:378)"

        /// One sequence per level, indexed by the level, for the level column.
        std::array<std::string_view, 7> levels{};

        /// The sequence for a level's column, or empty when the level is out of range.
        [[nodiscard]] std::string_view for_level(log_level level) const noexcept;

        /// The stock palette: dim for the incidentals, and the level column coloured by severity.
        [[nodiscard]] static line_style ansi() noexcept;

        [[nodiscard]] bool operator==(const line_style &) const noexcept = default;
    };

    /**
     * @brief The time of day an event was logged.
     * @param tp The event's timestamp.
     * @return The time in the local zone, or in UTC when no zone database is available: "13:05:42.117".
     */
    [[nodiscard]] std::string format_time(std::chrono::system_clock::time_point tp);

    /**
     * @brief The call site of an event, without the directory.
     * @param loc The event's source location.
     * @return The file name and line: "context.cpp:378".
     */
    [[nodiscard]] std::string format_location(const std::source_location &loc);

    /**
     * @brief One line of text for an event, without the trailing newline.
     * @param event The event to render.
     * @param format Which columns to print.
     * @param style The escape sequences to wrap them in; plain text by default.
     * @return The formatted line.
     */
    [[nodiscard]] std::string format_line(const log_event &event, const line_format &format = {},
                                          const line_style &style = {});

    /**
     * @class line_cache
     * @brief One event's rendered lines, shared between the sinks of a single dispatch.
     * @details Two text sinks configured the same way - a console and a file both printing the
     * default columns - used to format the same event twice. The router builds one of these per
     * event and hands it to every sink, so the second sink asking for a line it has already built
     * gets the first sink's string back.
     *
     * Sinks with different settings still each get their own line: the cache is keyed on the
     * settings themselves, not on the sink. It holds a few of them and formats without caching
     * beyond that, so a process with an unusual number of differently configured text sinks is no
     * worse off than before.
     *
     * The cache borrows the event and the settings passed to it. It lives for one dispatch, on the
     * stack of the thread that logged, and is not shared between threads.
     */
    class line_cache
    {
    public:
        /// @param event The event being dispatched; it must outlive the cache.
        explicit line_cache(const log_event &event) noexcept : event_(&event) {}

        line_cache(const line_cache &) = delete;
        line_cache &operator=(const line_cache &) = delete;

        /**
         * @brief The event rendered with these settings, formatting it only if nothing else has.
         * @param format Which columns to print.
         * @param style The escape sequences to wrap them in.
         * @return The line, borrowed from the cache: it lives as long as the cache does - one
         *         dispatch - and a sink that wants to keep it past its own `write` copies it.
         */
        [[nodiscard]] const std::string &line(const line_format &format, const line_style &style);

    private:
        /// A rendered line and the settings it was rendered with. Both are borrowed from the sink.
        struct entry
        {
            const line_format *format = nullptr;
            const line_style *style = nullptr;
            std::string text{};
        };

        /// Enough for a console, a file and a panel with their own settings.
        static constexpr std::size_t capacity = 4;

        const log_event *event_;
        std::array<entry, capacity> entries_{};
        std::size_t count_ = 0;
        std::string overflow_{};
    };

} // namespace catalyst::logging
