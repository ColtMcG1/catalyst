/**
 * @file win32_helpers.hpp
 * @brief The platform module's Windows-only helpers: DPI awareness and effective-DPI queries.
 * @details The UTF-8 conversion routines that used to be defined here now have one implementation
 * in src/win32/strings.hpp, shared with the audio and input backends, and are re-exported into this
 * namespace so the window and monitor backends keep spelling them `win32::wide_to_utf8`. What
 * remains below is specific to this module: nothing outside the platform backend queries DPI.
 *
 * This header stays free of <windows.h> -- the backends that include it also include much else, and
 * the opaque handle typedefs cost nothing.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <win32/strings.hpp>

#include <string>

namespace catalyst::platform::detail::win32
{
    using ::catalyst::detail::win32::utf8_to_wide;
    using ::catalyst::detail::win32::utf8_to_wide_or_ansi;
    using ::catalyst::detail::win32::wide_to_utf8;

    // Opaque handles to avoid including <windows.h> in the header.
    // These are ABI-compatible with Win32's HWND/HMONITOR (both are pointer types).
    using hwnd_handle = void *;
    using hmonitor_handle = void *;

    /**
     * @fn ensure_process_dpi_awareness
     * @brief Declares this process per-monitor DPI aware, once. Idempotent, and safe to call from any entry point that can
     * be the first one an application reaches.
     * @details A process that never declares DPI awareness is treated as DPI-unaware, which is not a neutral default: the
     * DPI query APIs report a flat 96 regardless of the display, WM_DPICHANGED is never delivered, and the compositor
     * bitmap-stretches every window on any display scaled above 100%. Declaring awareness must happen before any window is
     * created and before any DPI is queried, so it is done here rather than in the window backend alone -- an application
     * that enumerates monitors before opening a window would otherwise cache DPI-unaware values.
     *
     * Per-monitor-v2 is preferred because it is the only context in which the non-client area scales with the window as it
     * moves between displays; the older contexts are tried in turn for older systems. If the application declared awareness
     * itself through an application manifest every call here fails harmlessly and the manifest's choice stands, which is
     * the correct outcome.
     */
    void ensure_process_dpi_awareness() noexcept;

    // Returns the system effective DPI (px/in). Falls back to 96.
    [[nodiscard]] float effective_dpi_for_system() noexcept;

    // Returns the window effective DPI (px/in). Falls back to 96.
    [[nodiscard]] float effective_dpi_for_window(hwnd_handle hwnd) noexcept;

    // Attempts to read the monitor effective DPI (px/in). Returns true on success.
    [[nodiscard]] bool try_effective_dpi_for_monitor(hmonitor_handle mon, float &dpi_x, float &dpi_y) noexcept;
} // namespace catalyst::platform::detail::win32
