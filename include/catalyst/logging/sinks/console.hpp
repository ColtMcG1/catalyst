/**
 * @file console.hpp
 * @brief One line per event on an ostream.
 * @details The sink every program starts with, and usually the one it keeps: `std::clog` by
 * default, colour when the stream turns out to be a terminal that understands it.
 *
 *     router.add_sink(console_sink{});
 *     router.add_sink(console_sink{std::cout, line_format{.location = true}});
 *
 * Colour is decided per sink by `color_mode`, and `automatic` asks `terminal_supports_color`, which
 * only answers yes for a standard stream attached to a terminal. Anything else - a pipe, a file, a
 * captured `ostringstream` - gets plain text, because escape sequences written where nothing renders
 * them are litter in the output rather than colour.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/logging/event.hpp>
#include <catalyst/logging/format.hpp>
#include <catalyst/logging/sink.hpp>

#include <cstdint>
#include <iosfwd>

namespace catalyst::logging
{

    /**
     * @enum color_mode
     * @brief Whether a console sink colours its output.
     */
    enum class color_mode : std::uint8_t
    {
        automatic, ///< Colour when the stream is a terminal that understands it. See `terminal_supports_color`.
        always,    ///< Colour regardless: for a pipe into something that renders escape sequences.
        never,     ///< Never colour.
    };

    /**
     * @brief Whether writing escape sequences to a stream would produce colour rather than litter.
     * @param stream The stream a sink writes to.
     * @return True when the stream is one of the standard streams, that stream is attached to a
     *         terminal, the terminal takes ANSI sequences, and NO_COLOR is not set.
     * @details A stream that is not `std::cout`, `std::cerr` or `std::clog` answers false: an
     * `ostream` gives no way back to the descriptor it writes to, so nothing can be said about where
     * a stream the module did not open ends up. On Windows the first call also turns on the console's
     * virtual terminal processing, which is what makes the sequences mean anything there.
     */
    [[nodiscard]] bool terminal_supports_color(const std::ostream &stream) noexcept;

    /**
     * @struct console_sink
     * @brief One line per event on an ostream; `std::clog` unless told otherwise.
     * @details Writes through an `osyncstream` so lines from different threads do not interleave,
     * which is what lets it declare `reentrant`: it holds no state a second thread could disturb,
     * and the stream does its own locking.
     */
    struct console_sink : sink_contract<sink_threading::reentrant>
    {
        std::ostream *stream; ///< Never null.
        line_format format{};
        line_style style = line_style::ansi(); ///< The palette used when colour is on.
        color_mode color = color_mode::automatic;

        /// Writes to std::clog.
        console_sink() noexcept;

        /// Writes to `out`, which must outlive the sink.
        explicit console_sink(std::ostream &out, line_format format = {}) noexcept;

        void write(const log_event &event) const;

        /// Takes the rendered line from `cache`, so sinks configured alike format the event once.
        void write(const log_event &event, line_cache &cache) const;

        void flush() const;

        /// Whether this sink colours its output, with `automatic` resolved against the stream.
        [[nodiscard]] bool colored() const noexcept;
    };

} // namespace catalyst::logging
