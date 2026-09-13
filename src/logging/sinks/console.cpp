/**
 * @file console.cpp
 * @brief Implements `console_sink`.
 * @details Whether the sink colours at all is decided in terminal.cpp; this file only picks the
 * palette accordingly and writes the line. The write goes through an `osyncstream` so that two
 * threads logging at once produce two lines rather than one interleaved mess, which is what lets
 * the sink declare `reentrant` while holding no lock of its own.
 * License: MIT (see LICENSE).
 */

#include <catalyst/logging/sinks/console.hpp>

#include <iostream>
#include <syncstream>

namespace catalyst::logging
{

    namespace
    {
        /// The palette a sink writing somewhere that is not a terminal uses: none.
        const line_style plain{};
    } // namespace

    console_sink::console_sink() noexcept : stream(&std::clog) {}

    console_sink::console_sink(std::ostream &out, line_format format) noexcept : stream(&out), format(format) {}

    void console_sink::write(const log_event &event) const
    {
        line_cache cache(event);
        write(event, cache);
    }

    void console_sink::write(const log_event &event, line_cache &cache) const
    {
        // osyncstream so that lines from different threads do not interleave.
        std::osyncstream output(*stream);
        output << cache.line(format, colored() ? style : plain) << '\n';
    }

    void console_sink::flush() const
    {
        stream->flush();
    }

    bool console_sink::colored() const noexcept
    {
        switch (color)
        {
        case color_mode::always:
            return true;
        case color_mode::never:
            return false;
        case color_mode::automatic:
            break;
        }
        return terminal_supports_color(*stream);
    }

} // namespace catalyst::logging
