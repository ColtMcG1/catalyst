/**
 * @file detail_win32.hpp
 * @brief The audio module's Windows-only helpers, shared by the WASAPI and ASIO backends.
 * @details What is genuinely audio-specific -- the mapping from the HRESULTs the audio APIs return
 * onto `error_code` -- is defined here. The UTF-8 conversion routines that used to live here are
 * now one implementation in src/win32/strings.hpp, shared with the platform and input backends, and
 * are re-exported into this namespace so the backends keep spelling them `win32::wide_to_utf8`.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#if defined(_WIN32)

#  include <win32/strings.hpp>
#  include <win32/windows_lean.hpp>

#  include <audioclient.h>

#  include <catalyst/audio/error.hpp>

namespace catalyst::audio::detail::win32
{

    using ::catalyst::detail::win32::utf8_to_wide;
    using ::catalyst::detail::win32::wide_to_utf8;

    /// Translates the HRESULTs the audio APIs actually return into the module's error codes, so
    /// callers can distinguish "someone else owns the device" from "the device vanished" without
    /// depending on Windows headers. The backend wraps the result in an `error`, adding itself.
    inline error_code error_from_hresult(HRESULT hr) noexcept
    {
        switch (hr)
        {
        case S_OK:
            return error_code::none;
        case AUDCLNT_E_DEVICE_INVALIDATED:
            return error_code::device_lost;
        case AUDCLNT_E_DEVICE_IN_USE:
        case AUDCLNT_E_EXCLUSIVE_MODE_NOT_ALLOWED:
            return error_code::device_busy;
        case AUDCLNT_E_UNSUPPORTED_FORMAT:
            return error_code::format_unsupported;
        case AUDCLNT_E_ENDPOINT_CREATE_FAILED:
            return error_code::no_device;
        case E_OUTOFMEMORY:
            return error_code::platform_error;
        default:
            return error_code::platform_error;
        }
    }

} // namespace catalyst::audio::detail::win32

#endif // _WIN32
