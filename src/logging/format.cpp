/**
 * @file format.cpp
 * @brief Implements the line formatting declared in format.hpp.
 * @details The columns are appended one at a time rather than built from a single format string,
 * because a colour sequence has to sit outside the field it decorates: `"{:<8}"` counts whatever it
 * is given, so padding a level name that already carries escape bytes would push every following
 * column out of line. Appending the sequence to the output first and formatting the name into the
 * same buffer afterwards keeps the width computation looking only at the name.
 * License: MIT (see LICENSE).
 */

#include <catalyst/logging/format.hpp>

#include <cstddef>
#include <format>
#include <functional>
#include <iterator>
#include <string_view>
#include <thread>

namespace catalyst::logging
{

    namespace
    {

        /// Appends `text` wrapped in `color`, resetting only when there was a colour to reset.
        void paint(std::string &out, std::string_view color, std::string_view reset, std::string_view text)
        {
            out += color;
            out += text;
            if (!color.empty())
                out += reset;
        }

    } // namespace

    std::string_view line_style::for_level(log_level level) const noexcept
    {
        const auto index = static_cast<std::size_t>(level);
        return index < levels.size() ? levels[index] : std::string_view{};
    }

    line_style line_style::ansi() noexcept
    {
        line_style style;
        style.reset = "\x1b[0m";
        style.time = "\x1b[90m";
        style.category = "\x1b[36m";
        style.thread = "\x1b[90m";
        style.location = "\x1b[90m";
        style.levels = {
            "\x1b[90m",     // trace: bright black
            "\x1b[34m",     // debug: blue
            "\x1b[32m",     // info: green
            "\x1b[33m",     // warn: yellow
            "\x1b[31m",     // error: red
            "\x1b[91m",     // fatal: bright red
            "\x1b[1;97;41m" // critical: bold white on red
        };
        return style;
    }

    std::string format_time(std::chrono::system_clock::time_point tp)
    {
        using namespace std::chrono;
        const auto ms = floor<milliseconds>(tp);
        try
        {
            // Looked up once: current_zone() is a lookup in the tz database, not a cheap call.
            static const time_zone *zone = current_zone();
            const auto local = zone->to_local(ms);
            return std::format("{:%H:%M:%S}", local - floor<days>(local));
        }
        catch (...)
        {
            // No tz database on this machine. UTC is still a usable timestamp.
            return std::format("{:%H:%M:%S}", ms - floor<days>(ms));
        }
    }

    std::string format_location(const std::source_location &loc)
    {
        const std::string_view file = loc.file_name();
        const std::size_t slash = file.find_last_of("/\\");
        const std::string_view base = slash == std::string_view::npos ? file : file.substr(slash + 1);
        return std::format("{}:{}", base, loc.line());
    }

    const std::string &line_cache::line(const line_format &format, const line_style &style)
    {
        // Compared by value, not by address: the point is to catch a console and a file that were
        // configured the same way, and each of them holds its own copy of the settings.
        for (std::size_t i = 0; i < count_; ++i)
            if (*entries_[i].format == format && *entries_[i].style == style)
                return entries_[i].text;

        if (count_ == capacity)
        {
            overflow_ = format_line(*event_, format, style);
            return overflow_;
        }

        entry &slot = entries_[count_];
        slot.format = &format;
        slot.style = &style;
        slot.text = format_line(*event_, format, style);
        ++count_;
        return slot.text;
    }

    std::string format_line(const log_event &event, const line_format &format, const line_style &style)
    {
        std::string out;
        out.reserve(event.message.size() + 64);

        if (format.time)
        {
            paint(out, style.time, style.reset, format_time(event.timestamp));
            out += ' ';
        }
        if (format.level)
        {
            const std::string_view color = style.for_level(event.level);
            out += color;
            std::format_to(std::back_inserter(out), "{:<8}", event.level);
            if (!color.empty())
                out += style.reset;
            out += ' ';
        }
        if (format.category)
        {
            out += style.category;
            std::format_to(std::back_inserter(out), "[{}]", event.category);
            if (!style.category.empty())
                out += style.reset;
            out += ' ';
        }
        if (format.thread)
        {
            out += style.thread;
            std::format_to(std::back_inserter(out), "({})", std::hash<std::thread::id>{}(event.thread_id) & 0xffff);
            if (!style.thread.empty())
                out += style.reset;
            out += ' ';
        }

        out += event.message;

        if (format.location || (format.error_location && event.level >= log_level::error))
        {
            out += ' ';
            paint(out, style.location, style.reset, '(' + format_location(event.location) + ')');
        }
        return out;
    }

} // namespace catalyst::logging
