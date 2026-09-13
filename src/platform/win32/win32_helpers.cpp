/**
 * @file win32_helpers.cpp
 * @brief DPI awareness and effective-DPI queries for the Win32 platform backend.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "win32_helpers.hpp"

#include <win32/module.hpp>
#include <win32/windows_lean.hpp>

namespace catalyst::platform::detail::win32
{
    namespace
    {
        using ::catalyst::detail::win32::linked_symbol;
        using ::catalyst::detail::win32::loaded_symbol;
    } // namespace

    void ensure_process_dpi_awareness() noexcept
    {
        static const bool done = []() noexcept
        {
            // DPI_AWARENESS_CONTEXT is an opaque HANDLE whose well-known values are small negative numbers. They are
            // spelled out rather than using the SDK macros so this builds against older Windows SDKs too.
            constexpr INT_PTR per_monitor_aware_v2 = -4;
            constexpr INT_PTR per_monitor_aware = -3;
            constexpr INT_PTR system_aware = -2;

            using set_context_fn = BOOL(WINAPI *)(HANDLE);
            if (const auto fn = linked_symbol<set_context_fn>(L"user32.dll", "SetProcessDpiAwarenessContext"))
            {
                for (const INT_PTR ctx : {per_monitor_aware_v2, per_monitor_aware, system_aware})
                {
                    if (fn(reinterpret_cast<HANDLE>(ctx)))
                        return true;
                }
            }

            // Windows 8.1: PROCESS_PER_MONITOR_DPI_AWARE == 2.
            using set_awareness_fn = HRESULT(WINAPI *)(int);
            if (const auto fn = loaded_symbol<set_awareness_fn>(L"shcore.dll", "SetProcessDpiAwareness"))
            {
                if (SUCCEEDED(fn(2)))
                    return true;
            }

            // Vista through 8: system-DPI aware is the best available.
            SetProcessDPIAware();
            return true;
        }();

        (void)done;
    }

    float effective_dpi_for_system() noexcept
    {
        ensure_process_dpi_awareness();

        using get_dpi_for_system_fn = UINT(WINAPI *)();
        static const auto fn = linked_symbol<get_dpi_for_system_fn>(L"user32.dll", "GetDpiForSystem");

        if (!fn)
            return 96.0f;

        const UINT dpi = fn();
        if (dpi == 0)
            return 96.0f;

        return static_cast<float>(dpi);
    }

    float effective_dpi_for_window(hwnd_handle hwnd) noexcept
    {
        ensure_process_dpi_awareness();

        using get_dpi_for_window_fn = UINT(WINAPI *)(HWND);
        static const auto fn = linked_symbol<get_dpi_for_window_fn>(L"user32.dll", "GetDpiForWindow");

        const HWND hwnd_w = reinterpret_cast<HWND>(hwnd);
        if (!fn || hwnd_w == nullptr)
            return effective_dpi_for_system();

        const UINT dpi = fn(hwnd_w);
        if (dpi == 0)
            return effective_dpi_for_system();

        return static_cast<float>(dpi);
    }

    bool try_effective_dpi_for_monitor(hmonitor_handle mon, float &dpi_x, float &dpi_y) noexcept
    {
        // Prefer GetDpiForMonitor (Win 8.1+). Resolved at run time to keep compatibility.
        // Signature: HRESULT GetDpiForMonitor(HMONITOR, MONITOR_DPI_TYPE, UINT*, UINT*)
        ensure_process_dpi_awareness();

        using get_dpi_for_monitor_fn = HRESULT(WINAPI *)(HMONITOR, int, UINT *, UINT *);
        static const auto fn = loaded_symbol<get_dpi_for_monitor_fn>(L"shcore.dll", "GetDpiForMonitor");

        const HMONITOR mon_w = reinterpret_cast<HMONITOR>(mon);
        if (!fn || mon_w == nullptr)
            return false;

        constexpr int MDT_EFFECTIVE_DPI = 0;
        UINT x = 0;
        UINT y = 0;
        if (FAILED(fn(mon_w, MDT_EFFECTIVE_DPI, &x, &y)) || x == 0 || y == 0)
            return false;

        dpi_x = static_cast<float>(x);
        dpi_y = static_cast<float>(y);
        return true;
    }
} // namespace catalyst::platform::detail::win32
