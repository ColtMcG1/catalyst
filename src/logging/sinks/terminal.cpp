/**
 * @file terminal.cpp
 * @brief Decides whether escape sequences written to a stream will be rendered or shown.
 * @details Three things have to be true before a console sink colours anything: the stream has to
 * be one whose destination is knowable, that destination has to be a terminal, and the terminal has
 * to understand ANSI. An `ostream` exposes no descriptor, so the first is answered by identity - a
 * sink writing to `std::cout`, `std::cerr` or `std::clog` writes to a standard stream, and anything
 * else could be a file, a string or a socket. The rest is `isatty`, plus, on Windows, turning on the
 * console's virtual terminal processing, which is off by default.
 *
 * The answers are cached in function-local statics: the environment is read once, and the Windows
 * console mode is changed once, however many sinks ask.
 * License: MIT (see LICENSE).
 */

// The MSVC CRT deprecates getenv in favour of _dupenv_s, on the grounds that the pointer it returns
// is invalidated by a later putenv on another thread. Each use below reads the string immediately,
// once, during the initialisation of a function-local static, and copies nothing out of it. Scoped
// to this file rather than set for the build, so the warning stays live everywhere else.
#if defined(_MSC_VER) && !defined(_CRT_SECURE_NO_WARNINGS)
#  define _CRT_SECURE_NO_WARNINGS
#endif

#include <catalyst/logging/sinks/console.hpp>

#include <cstdlib>
#include <cstring>
#include <iostream>
#include <ostream>

#if defined(_WIN32)
#  include <io.h>

#  include <win32/windows_lean.hpp>

#  ifndef ENABLE_VIRTUAL_TERMINAL_PROCESSING
#    define ENABLE_VIRTUAL_TERMINAL_PROCESSING 0x0004
#  endif
#else
#  include <unistd.h>
#endif

namespace catalyst::logging
{

    namespace
    {

        enum class standard_stream
        {
            unknown,
            out,
            err,
        };

        standard_stream identify(const std::ostream &stream) noexcept
        {
            if (&stream == &std::cout)
                return standard_stream::out;
            if (&stream == &std::cerr || &stream == &std::clog)
                return standard_stream::err;
            return standard_stream::unknown;
        }

        /// NO_COLOR set to anything non-empty, or a terminal that says it is dumb, settles it.
        bool forbidden_by_environment() noexcept
        {
            const char *const no_color = std::getenv("NO_COLOR");
            if (no_color != nullptr && no_color[0] != '\0')
                return true;
            const char *const term = std::getenv("TERM");
            return term != nullptr && std::strcmp(term, "dumb") == 0;
        }

#if defined(_WIN32)

        /// Turns on virtual terminal processing for a console handle. False when it is not a console.
        bool enable_virtual_terminal(DWORD handle_id) noexcept
        {
            const HANDLE handle = GetStdHandle(handle_id);
            if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
                return false;
            DWORD mode = 0;
            if (GetConsoleMode(handle, &mode) == 0)
                return false;
            if ((mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0)
                return true;
            return SetConsoleMode(handle, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0;
        }

        bool detect(standard_stream stream) noexcept
        {
            const bool is_out = stream == standard_stream::out;
            if (_isatty(is_out ? 1 : 2) == 0)
                return false;
            if (enable_virtual_terminal(is_out ? STD_OUTPUT_HANDLE : STD_ERROR_HANDLE))
                return true;
            // A descriptor that is a tty but not a Win32 console belongs to an emulator's pty -
            // mintty, and the terminals built on it - which takes the sequences directly.
            return std::getenv("TERM") != nullptr;
        }

#else

        bool detect(standard_stream stream) noexcept
        {
            return isatty(stream == standard_stream::out ? STDOUT_FILENO : STDERR_FILENO) != 0;
        }

#endif

        bool supported(standard_stream stream) noexcept
        {
            static const bool forbidden = forbidden_by_environment();
            static const bool out_ok = !forbidden && detect(standard_stream::out);
            static const bool err_ok = !forbidden && detect(standard_stream::err);
            return stream == standard_stream::out ? out_ok : err_ok;
        }

    } // namespace

    bool terminal_supports_color(const std::ostream &stream) noexcept
    {
        const standard_stream which = identify(stream);
        return which != standard_stream::unknown && supported(which);
    }

} // namespace catalyst::logging
