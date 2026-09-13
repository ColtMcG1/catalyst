/**
 * @file strings.cpp
 * @brief UTF-8 / UTF-16 conversion for the Win32 backends.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "strings.hpp"

#if defined(_WIN32)

#include "windows_lean.hpp"

namespace catalyst::detail::win32
{

    namespace
    {
        /// Runs the two-pass MultiByteToWideChar dance for one code page. Returns an empty string on
        /// any failure, which is how the caller distinguishes "this code page does not apply".
        [[nodiscard]] std::wstring to_wide(std::string_view text, UINT codepage, DWORD flags)
        {
            const int length = static_cast<int>(text.size());

            const int needed = MultiByteToWideChar(codepage, flags, text.data(), length, nullptr, 0);
            if (needed <= 0)
                return {};

            std::wstring out(static_cast<size_t>(needed), L'\0');

            const int written =
                MultiByteToWideChar(codepage, flags, text.data(), length, out.data(), needed);
            if (written <= 0)
                return {};

            out.resize(static_cast<size_t>(written));
            return out;
        }
    } // namespace

    std::string wide_to_utf8(std::wstring_view text)
    {
        if (text.empty())
            return {};

        const int length = static_cast<int>(text.size());

        const int needed =
            WideCharToMultiByte(CP_UTF8, 0, text.data(), length, nullptr, 0, nullptr, nullptr);
        if (needed <= 0)
            return {};

        std::string out(static_cast<size_t>(needed), '\0');

        const int written =
            WideCharToMultiByte(CP_UTF8, 0, text.data(), length, out.data(), needed, nullptr, nullptr);
        if (written <= 0)
            return {};

        out.resize(static_cast<size_t>(written));
        return out;
    }

    std::wstring utf8_to_wide(std::string_view text)
    {
        if (text.empty())
            return {};

        return to_wide(text, CP_UTF8, MB_ERR_INVALID_CHARS);
    }

    std::wstring utf8_to_wide_or_ansi(std::string_view text)
    {
        if (text.empty())
            return {};

        // MB_ERR_INVALID_CHARS is what makes the fallback safe: without it the UTF-8 pass would
        // succeed on legacy-encoded input by substituting replacement characters, and the ANSI pass
        // would never be reached.
        if (std::wstring utf8 = to_wide(text, CP_UTF8, MB_ERR_INVALID_CHARS); !utf8.empty())
            return utf8;

        return to_wide(text, CP_ACP, 0);
    }

} // namespace catalyst::detail::win32

#endif // _WIN32
