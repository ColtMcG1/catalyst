/**
 * @file strings.hpp
 * @brief Conversion between the UTF-8 that Catalyst's public API speaks and the UTF-16 that the
 * wide Win32 entry points speak.
 * @details Catalyst uses UTF-8 throughout -- window titles, device names, endpoint identifiers --
 * while every Win32 function that takes text has a wide form that the narrow form merely wraps.
 * The boundary is therefore crossed by nearly every backend, and it used to be crossed by two
 * separately maintained copies of these routines that had already drifted apart: the audio module's
 * took string_views and required well-formed UTF-8, the platform module's took NUL-terminated
 * pointers and retried in the active code page. This header keeps both behaviours but only one
 * implementation of each.
 *
 * Every function is total: invalid input, an empty range or a conversion the OS rejects all yield
 * an empty string rather than an error, because none of the call sites can do anything useful with
 * a failure beyond substituting an empty name.
 *
 * The pointer overloads exist so that a NUL-terminated Win32 buffer -- a WCHAR[32] out of
 * MONITORINFOEX, a caller-supplied const char* that may be null -- converts without the caller
 * having to guard against null first. They are preferred over the view overloads by overload
 * resolution for array and pointer arguments, since array-to-pointer decay is a standard
 * conversion where forming a view is a user-defined one.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#if defined(_WIN32)

#  include <string>
#  include <string_view>

namespace catalyst::detail::win32
{

    /// Converts UTF-16 to UTF-8. Returns an empty string if the text is empty or ill-formed.
    [[nodiscard]] std::string wide_to_utf8(std::wstring_view text);

    /// Converts strictly-UTF-8 input to UTF-16. Returns an empty string if the text is empty or is
    /// not valid UTF-8; use utf8_to_wide_or_ansi() where the input comes from an application and
    /// may not be UTF-8 at all.
    [[nodiscard]] std::wstring utf8_to_wide(std::string_view text);

    /**
     * @brief Converts application-supplied text to UTF-16, treating it as UTF-8 and falling back to
     * the process's active code page.
     * @details Catalyst documents its char strings as UTF-8, but an application that hands over a
     * string literal from a source file saved in a legacy code page, or text it read from a
     * non-UTF-8 file, would otherwise get a blank window title. Strict UTF-8 is tried first so that
     * correct input is never reinterpreted -- the ANSI attempt only ever runs on input that is
     * already known not to be UTF-8.
     */
    [[nodiscard]] std::wstring utf8_to_wide_or_ansi(std::string_view text);

    /// @overload Null-safe: a null pointer converts to an empty string.
    [[nodiscard]] inline std::string wide_to_utf8(const wchar_t *text)
    {
        return text ? wide_to_utf8(std::wstring_view{text}) : std::string{};
    }

    /// @overload Null-safe: a null pointer converts to an empty string.
    [[nodiscard]] inline std::wstring utf8_to_wide(const char *text)
    {
        return text ? utf8_to_wide(std::string_view{text}) : std::wstring{};
    }

    /// @overload Null-safe: a null pointer converts to an empty string.
    [[nodiscard]] inline std::wstring utf8_to_wide_or_ansi(const char *text)
    {
        return text ? utf8_to_wide_or_ansi(std::string_view{text}) : std::wstring{};
    }

} // namespace catalyst::detail::win32

#endif // _WIN32
