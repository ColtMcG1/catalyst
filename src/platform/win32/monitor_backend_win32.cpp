#include "../detail_backend.hpp"

// For EnumDisplayMonitors, GetMonitorInfoW, CreateDCW, GetDeviceCaps, EnumDisplaySettingsW, etc.
#include <win32/windows_lean.hpp>

#include <algorithm>
#include <string>
#include <vector>

#include "win32_helpers.hpp"

namespace catalyst::platform::detail
{
    namespace
    {
        monitor_id monitor_id_from_hmonitor(HMONITOR m) noexcept
        {
            return static_cast<monitor_id>(reinterpret_cast<std::uintptr_t>(m));
        }

        void fill_mm_and_refresh(const wchar_t *device_name, monitor_desc &out) noexcept
        {
            if (!device_name || !*device_name)
                return;

            // Physical size in mm (may be 0 or inaccurate depending on driver/reporting).
            HDC hdc = CreateDCW(L"DISPLAY", device_name, nullptr, nullptr);
            if (hdc)
            {
                const int w_mm = GetDeviceCaps(hdc, HORZSIZE);
                const int h_mm = GetDeviceCaps(hdc, VERTSIZE);
                if (w_mm > 0 && h_mm > 0)
                    out.size_mm = {static_cast<float>(w_mm), static_cast<float>(h_mm)};

                DeleteDC(hdc);
            }

            // Refresh rate.
            DEVMODEW dm{};
            dm.dmSize = sizeof(dm);
            if (EnumDisplaySettingsW(device_name, ENUM_CURRENT_SETTINGS, &dm))
            {
                if (dm.dmDisplayFrequency > 1)
                    out.refresh_rate_hz = static_cast<std::uint32_t>(dm.dmDisplayFrequency);
            }
        }

        void fill_dpi(HMONITOR mon, monitor_desc &out) noexcept
        {
            float dx = 0.0f;
            float dy = 0.0f;
            if (win32::try_effective_dpi_for_monitor(mon, dx, dy))
            {
                out.dpi_x = dx;
                out.dpi_y = dy;
                return;
            }

            const float sys = win32::effective_dpi_for_system();
            out.dpi_x = sys;
            out.dpi_y = sys;
        }

        struct enum_ctx
        {
            std::vector<monitor_desc> *out = nullptr;
        };

        BOOL CALLBACK enum_monitors_proc(HMONITOR mon, HDC, LPRECT, LPARAM user)
        {
            auto &ctx = *reinterpret_cast<enum_ctx *>(user);
            if (!ctx.out)
                return TRUE;

            MONITORINFOEXW mi{};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(mon, &mi))
                return TRUE;

            monitor_desc d;
            d.id = monitor_id_from_hmonitor(mon);
            d.name = win32::wide_to_utf8(mi.szDevice);
            d.bounds_px = {{mi.rcMonitor.left, mi.rcMonitor.top}, {mi.rcMonitor.right, mi.rcMonitor.bottom}};
            d.work_area_px = {{mi.rcWork.left, mi.rcWork.top}, {mi.rcWork.right, mi.rcWork.bottom}};
            d.primary = (mi.dwFlags & MONITORINFOF_PRIMARY) != 0;

            fill_mm_and_refresh(mi.szDevice, d);
            fill_dpi(mon, d);

            ctx.out->push_back(std::move(d));
            return TRUE;
        }

        std::vector<monitor_desc> enumerate_monitors()
        {
            std::vector<monitor_desc> out;
            enum_ctx ctx{&out};
            EnumDisplayMonitors(nullptr, nullptr, &enum_monitors_proc, reinterpret_cast<LPARAM>(&ctx));

            // Ensure a stable ordering (primary first, then by id) for deterministic tests/logs.
            std::stable_sort(out.begin(), out.end(), [](const monitor_desc &a, const monitor_desc &b) {
                if (a.primary != b.primary)
                    return a.primary;
                return a.id < b.id;
            });

            return out;
        }
    }

    std::size_t get_monitor_count() noexcept
    {
        return enumerate_monitors().size();
    }

    std::vector<monitor_desc> get_monitor_list() noexcept
    {
        return enumerate_monitors();
    }

    monitor_desc get_monitor(monitor_id id) noexcept
    {
        auto list = enumerate_monitors();
        const auto it = std::find_if(list.begin(), list.end(), [&](const monitor_desc &m) { return m.id == id; });
        if (it == list.end())
            return {};
        return *it;
    }

    monitor_id primary_monitor() noexcept
    {
        auto list = enumerate_monitors();
        const auto it = std::find_if(list.begin(), list.end(), [](const monitor_desc &m) { return m.primary; });
        if (it == list.end())
            return 0;
        return it->id;
    }

    monitor_id monitor_for_window(window_id w) noexcept
    {
        const native_handle h = get_native_handle(w);
        if (h.kind != native_handle_kind::win32_hwnd || h.handle == nullptr)
            return 0;

        const HWND hwnd = static_cast<HWND>(h.handle);
        const HMONITOR mon = MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST);
        if (!mon)
            return 0;

        return monitor_id_from_hmonitor(mon);
    }
} // namespace catalyst::platform::detail
