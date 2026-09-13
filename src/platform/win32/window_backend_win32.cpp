/**
 * @file window_backend_win32.cpp
 * @brief The Win32 implementation of the Catalyst platform window backend: window creation and state, translation of
 * window messages into Catalyst events, cursor modes and raw mouse input, per-monitor DPI, and the frame callback that
 * keeps a window rendering while the operating system owns the message pump.
 * License: MIT (see LICENSE).
 */

#include "../detail_backend.hpp"

#include <catalyst/events/bus.hpp>
#include <catalyst/input/feed.hpp>
#include <catalyst/input/keyboard.hpp>
#include <catalyst/input/mouse.hpp>
#include <catalyst/text/utf8.hpp>

// For basic Windows API functions and types (e.g. HWND, HMONITOR, GetKeyState, etc.). The header also
// suppresses the min/max macros, which would otherwise break every std::min/std::max spelled out below.
#include <win32/windows_lean.hpp>

#include "win32_helpers.hpp"

#include <win32/module.hpp>

#include <algorithm>
#include <cstring>
#include <memory>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include <dwmapi.h>   // For DwmGetCompositionTimingInfo (the display refresh period) and DwmSetWindowAttribute.
#include <windowsx.h> // For GET_X_LPARAM/GET_Y_LPARAM and GET_XBUTTON_WPARAM.

/**
 * @namespace catalyst::platform::detail
 * @brief The catalyst::platform::detail namespace contains internal implementation details for the Catalyst Platform library's window management on the Win32 platform. This includes helper functions, internal data structures, and other components that are not intended to be exposed to users of the library. By organizing these implementation details within a nested namespace, we can keep them separate from the public API and avoid naming conflicts, while still allowing for efficient and effective management of windows and events on the Win32 platform.
 */
namespace catalyst::platform::detail
{
    /**
     * @namespace
     * @brief An unnamed namespace holding the Win32 backend's internal state and helpers, which have internal linkage and
     * are not visible outside this translation unit.
     */
    namespace
    {
        /**
         * @fn k_window_class_name
         * @brief A constant wide string representing the name of the window class used for creating windows in this platform implementation. This name is registered with the Windows API when the first window is created, and it is used to identify the type of window being created. The window class name must be unique within the application to avoid conflicts with other window classes that may be registered by the application or by third-party libraries.
         */
        constexpr wchar_t k_window_class_name[] = L"CatalystWindow";

        /**
         * @var k_size_move_timer_id
         * @brief Identifier of the timer that is armed for the duration of a modal size/move loop. The loop runs inside
         * DefWindowProcW and does not return to the application, so a timer is the only way to get periodic control back
         * when the user is holding the border still (a stationary drag produces no WM_SIZE and no WM_PAINT at all).
         */
        constexpr UINT_PTR k_size_move_timer_id = 1;

        /**
         * @var k_dwmwa_use_immersive_dark_mode
         * @brief DWMWA_USE_IMMERSIVE_DARK_MODE. The attribute was renumbered from 19 to 20 in Windows 10 build 18985, and
         * older Windows SDKs do not declare either name, so both values are spelled out here and tried in turn.
         */
        constexpr DWORD k_dwmwa_use_immersive_dark_mode = 20;
        constexpr DWORD k_dwmwa_use_immersive_dark_mode_legacy = 19;

        /**
         * @struct window_state
         * @brief Everything the backend keeps per window: the native handle, the input bookkeeping needed to derive mouse
         * deltas, enter/leave transitions and surrogate-pair decoding, the cursor and display modes, the frame callback,
         * and the styles saved so borderless fullscreen can be undone.
         * @note A pointer to this structure is stored in the window's GWLP_USERDATA, so window_proc reaches it without a
         * hash lookup. std::unordered_map guarantees that pointers to its mapped values stay valid across insertion and
         * rehashing, so the stored pointer remains good for as long as the entry exists.
         */
        struct window_state
        {
            HWND hwnd = nullptr;
            window_id id = 0;

            /**
             * @var creating
             * @brief True between the map insertion and the end of create_window. Messages that arrive while
             * CreateWindowExW is still running (WM_GETMINMAXINFO, WM_NCCREATE, WM_CREATE, the first WM_SIZE and WM_MOVE)
             * are handled normally but publish nothing, because the application has not been given the window id yet;
             * create_window publishes the canonical initial events once it has one.
             */
            bool creating = true;

            // ---- Input bookkeeping ----
            math::vec2<std::int32_t> last_mouse_pos_px{};
            bool has_mouse_pos = false; ///< false until the first WM_MOUSEMOVE, so the first delta is zero
            bool mouse_inside = false;  ///< the cursor is over the client area and a WM_MOUSELEAVE is pending
            wchar_t pending_high_surrogate = 0;

            /**
             * @var buttons_down
             * @brief The mouse buttons currently held over this window, mirrored from the button messages. The mouse is
             * captured while this is non-empty so drags keep reporting after the cursor leaves the window.
             * @note This exists for the capture lifecycle, which is the platform's own business, and not to synthesise
             * releases on focus loss - the input registry does that from the state it already owns (see
             * input::event_feed::feed_focus_lost). The keys this window held used to be tracked here for exactly that
             * reason; they are not any more.
             */
            input::mouse_buttons buttons_down = input::mouse_buttons::none;
            bool holds_capture = false; ///< this window called SetCapture and has not released it yet

            // ---- Modes ----
            cursor_mode cursor = cursor_mode::normal;
            window_display_state display = window_display_state::restored;

            // ---- Frame callback ----
            frame_callback frame_cb = nullptr;
            void *frame_user = nullptr;
            bool in_frame_callback = false; ///< guards against a callback that re-triggers a frame request
            bool in_size_move = false;      ///< a modal size/move loop is currently running for this window
            LONGLONG last_frame_qpc = 0;    ///< when the last size/move frame started, for pacing

            // ---- Constraints and saved state ----
            math::vec2<std::int32_t> min_size_px{0, 0}; ///< client-area minimum; a zero component means unconstrained
            math::vec2<std::int32_t> max_size_px{0, 0}; ///< client-area maximum; a zero component means unconstrained

            bool fullscreen = false;
            bool layered_by_us = false; ///< set_opacity added WS_EX_LAYERED and may remove it again
            DWORD saved_style = 0;
            DWORD saved_ex_style = 0;
            WINDOWPLACEMENT saved_placement{};
        };

        /**
         * @var g_windows
         * @brief Every window this backend has created, keyed by window id. Values are addressed by pointer from
         * GWLP_USERDATA, which std::unordered_map's reference stability guarantees remains valid until the entry is erased.
         */
        std::unordered_map<window_id, window_state> g_windows;

        /**
         * @var g_next_window_id
         * @brief Source of window ids. Ids are never reused, so a stale id can always be recognised as invalid.
         */
        std::uint64_t g_next_window_id = 1;

        /**
         * @var g_bus
         * @brief The bus window events are dispatched to, or nullptr when nobody is listening. See dispatch_event.
         */
        events::bus *g_bus = nullptr;

        /**
         * @var g_feed
         * @brief The feed window-sourced input is delivered to, or nullptr when nobody is listening.
         * @details Input does not go to the bus from here. It goes to the input module, which owns the device state and
         * publishes the events onward, so that what this backend reports and what the input module believes is held can
         * never disagree - they are the same state.
         */
        input::event_feed *g_feed = nullptr;

        /**
         * @var g_raw_mouse_window
         * @brief The window raw mouse input is currently registered for (the one whose cursor is captured), or 0. Raw input
         * registration is a per-process resource, so this stays global even though the rest of the input state does not.
         */
        window_id g_raw_mouse_window = 0;

        /**
         * @var g_last_raw_absolute
         * @brief Last absolute raw-mouse position, for devices (RDP, tablets) that report absolute rather than relative motion.
         */
        math::vec2<std::int32_t> g_last_raw_absolute{};
        bool g_has_last_raw_absolute = false;

        // -------------------------------------------------------------------------------------------------------------
        // Dynamically resolved OS entry points
        // -------------------------------------------------------------------------------------------------------------

        /**
         * @struct os_entry_points
         * @brief The user32 functions this backend uses that do not exist on every version of Windows it supports. They are
         * resolved once by ensure_backend_initialised() rather than through a function-local static per call site, which
         * keeps the thread-safe-static guard off the message path.
         */
        struct os_entry_points
        {
            UINT(WINAPI *get_dpi_for_window)(HWND) = nullptr;
            UINT(WINAPI *get_dpi_for_system)() = nullptr;
            BOOL(WINAPI *adjust_window_rect_ex_for_dpi)(LPRECT, DWORD, BOOL, DWORD, UINT) = nullptr;
        };

        os_entry_points g_os{};
        bool g_backend_initialised = false;

        /**
         * @fn ensure_backend_initialised
         * @brief One-time backend setup: declares the process per-monitor DPI aware and resolves the optional user32 entry
         * points.
         * @details Declaring DPI awareness is not optional bookkeeping. A process that never declares it is treated as
         * DPI-unaware: GetDpiForWindow reports 96 no matter what the display is set to, WM_DPICHANGED is never delivered,
         * and the compositor bitmap-stretches the window on any display scaled above 100%, which is what makes an otherwise
         * correct renderer look blurry. Declaring awareness must happen before the first window is created and before
         * anything queries a DPI, so every entry point that can be reached first calls this.
         *
         * Per-monitor-v2 is preferred because it is the only mode in which non-client areas (the title bar, the resize
         * border, the menu) scale with the window as it moves between displays. The older contexts are tried in turn for
         * older systems. If the application already declared awareness through its manifest every one of these calls fails
         * harmlessly and the manifest wins, which is the correct outcome.
         */
        void ensure_backend_initialised() noexcept
        {
            if (g_backend_initialised)
                return;
            g_backend_initialised = true;

            win32::ensure_process_dpi_awareness();

            using ::catalyst::detail::win32::linked_symbol;

            g_os.get_dpi_for_window =
                linked_symbol<decltype(g_os.get_dpi_for_window)>(L"user32.dll", "GetDpiForWindow");
            g_os.get_dpi_for_system =
                linked_symbol<decltype(g_os.get_dpi_for_system)>(L"user32.dll", "GetDpiForSystem");
            g_os.adjust_window_rect_ex_for_dpi =
                linked_symbol<decltype(g_os.adjust_window_rect_ex_for_dpi)>(L"user32.dll", "AdjustWindowRectExForDpi");
        }

        /** @brief The effective DPI of @p hwnd, or the system DPI (ultimately 96) where the API is unavailable. */
        UINT dpi_for_window(HWND hwnd) noexcept
        {
            if (g_os.get_dpi_for_window && hwnd)
            {
                if (const UINT dpi = g_os.get_dpi_for_window(hwnd))
                    return dpi;
            }
            if (g_os.get_dpi_for_system)
            {
                if (const UINT dpi = g_os.get_dpi_for_system())
                    return dpi;
            }
            return USER_DEFAULT_SCREEN_DPI;
        }

        /** @brief The system DPI, or 96 where the API is unavailable. */
        UINT dpi_for_system() noexcept
        {
            if (g_os.get_dpi_for_system)
            {
                if (const UINT dpi = g_os.get_dpi_for_system())
                    return dpi;
            }
            return USER_DEFAULT_SCREEN_DPI;
        }

        /** @brief A DPI expressed as a scale factor relative to the 96 DPI baseline Win32 measures against. */
        constexpr float scale_from_dpi(UINT dpi) noexcept
        {
            return static_cast<float>(dpi) / static_cast<float>(USER_DEFAULT_SCREEN_DPI);
        }

        /** @brief The DPI scaling factor for a window. */
        float dpi_scale_for_window(HWND hwnd) noexcept
        {
            return scale_from_dpi(dpi_for_window(hwnd));
        }

        /**
         * @fn adjust_rect
         * @brief Grows a client rectangle into the window rectangle that contains it, honouring the DPI the frame will
         * actually be drawn at. AdjustWindowRectEx assumes the system DPI, which is wrong for any window that is not on the
         * primary display, so the per-DPI variant is preferred wherever it exists.
         */
        void adjust_rect(RECT &r, DWORD style, DWORD ex_style, UINT dpi) noexcept
        {
            if (g_os.adjust_window_rect_ex_for_dpi)
                g_os.adjust_window_rect_ex_for_dpi(&r, style, FALSE, ex_style, dpi);
            else
                AdjustWindowRectEx(&r, style, FALSE, ex_style);
        }

        /** @brief adjust_rect() using a live window's current styles and DPI. */
        void adjust_rect_for_window(HWND hwnd, RECT &r) noexcept
        {
            const auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
            const auto ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
            adjust_rect(r, style, ex_style, dpi_for_window(hwnd));
        }

        /** @brief The outer window size needed to give @p hwnd a client area of @p client_px pixels. */
        math::vec2<std::int32_t> window_size_for_client(HWND hwnd, const math::vec2<std::int32_t> &client_px) noexcept
        {
            RECT r{0, 0, static_cast<LONG>(client_px.x()), static_cast<LONG>(client_px.y())};
            adjust_rect_for_window(hwnd, r);
            return {static_cast<std::int32_t>(r.right - r.left), static_cast<std::int32_t>(r.bottom - r.top)};
        }

        // -------------------------------------------------------------------------------------------------------------
        // Event delivery
        // -------------------------------------------------------------------------------------------------------------

        /**
         * @fn dispatch_event
         * @brief Dispatches one window event to the installed bus, or drops it if there is none.
         * @details The event is taken by value and dispatched from that value, so this allocates nothing: the bus keys on
         * the static event type, which needs neither a runtime type id nor a heap-allocated base pointer.
         *
         * Nothing is retained when no bus is installed. That used to matter, because the operating system can hold this
         * thread inside its own modal size/move loop for the whole of a border drag, and a bounded queue with a
         * coalescing policy for resizes and moves was what kept that drag from either freezing the window or growing
         * without limit. The frame callback solves that problem properly - it hands control back mid-loop so the
         * application can render the current size rather than catch up on a queue of stale ones afterwards - so the
         * queue, its bound and its coalescing are all gone. See set_frame_callback.
         */
        template <typename E>
        void dispatch_event(E e)
        {
            if (g_bus)
                g_bus->dispatch(std::move(e));
        }

        /**
         * @fn resolve_px
         * @brief Resolves a ui::length measurement to pixels on the given axis at the given DPI scale.
         */
        std::int32_t resolve_px(const ui::length &v, ui::axis a, float dpi_scale) noexcept
        {
            ui::resolve_context ctx{};
            ctx.dpi_scale = dpi_scale;
            ctx.dpi_x = dpi_scale * 96.0f;
            ctx.dpi_y = dpi_scale * 96.0f;
            const float px = ui::resolve_or(v, a, ctx, 0.0f);
            return static_cast<std::int32_t>(px);
        }

        /**
         * @fn window_state_from_id
         * @brief Looks up the per-window state for an id, or nullptr if the id is not one of ours.
         */
        window_state *window_state_from_id(window_id id) noexcept
        {
            auto it = g_windows.find(id);
            if (it == g_windows.end())
                return nullptr;
            return &it->second;
        }

        /**
         * @fn state_from_hwnd
         * @brief The per-window state attached to a window handle, or nullptr before WM_NCCREATE has run.
         * @note This replaces a hash lookup on every single message with a single field read, which is worth doing because
         * mouse motion and raw input can reach four figures of messages per second.
         */
        window_state *state_from_hwnd(HWND hwnd) noexcept
        {
            return reinterpret_cast<window_state *>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
        }

        /** @brief The handle of a window id, or nullptr if the id is not one of ours. */
        HWND hwnd_from_id(window_id id) noexcept
        {
            const window_state *ws = window_state_from_id(id);
            return ws ? ws->hwnd : nullptr;
        }

        // -------------------------------------------------------------------------------------------------------------
        // Frame callback
        // -------------------------------------------------------------------------------------------------------------

        /**
         * @var g_frame_interval_qpc
         * @brief One display refresh period in QueryPerformanceCounter units, or 0 before it has been measured. Reset when
         * the display configuration changes, since the refresh rate can change with it.
         */
        LONGLONG g_frame_interval_qpc = 0;

        /** @brief One display refresh period in QPC units, falling back to 1/60 s if the compositor will not say. */
        LONGLONG frame_interval_qpc() noexcept
        {
            if (g_frame_interval_qpc != 0)
                return g_frame_interval_qpc;

            DWM_TIMING_INFO timing{};
            timing.cbSize = sizeof(timing);
            if (SUCCEEDED(DwmGetCompositionTimingInfo(nullptr, &timing)) && timing.qpcRefreshPeriod > 0)
            {
                g_frame_interval_qpc = static_cast<LONGLONG>(timing.qpcRefreshPeriod);
                return g_frame_interval_qpc;
            }

            LARGE_INTEGER freq{};
            QueryPerformanceFrequency(&freq);
            g_frame_interval_qpc = (freq.QuadPart > 0) ? (freq.QuadPart / 60) : 1;
            return g_frame_interval_qpc;
        }

        /**
         * @fn request_frame
         * @brief Asks the application to render @p ws now, if it installed a frame callback.
         * @details Called from the paths on which the operating system either wants the window repainted or is about to
         * keep the thread to itself: WM_PAINT, WM_SIZE, and the timer that runs for the duration of a modal size/move loop.
         *
         * Inside a size/move loop the rate has to be capped here, because nothing else caps it. The timer runs at the
         * shortest interval Windows accepts and its real resolution is finer still on a system where something has raised
         * the timer frequency, so an ungated callback can be entered several hundred times a second while the user drags a
         * border. Frames are therefore refused until one display refresh period has passed since the previous one began.
         * Outside a size/move loop no gate applies: those calls come from the OS asking for a repaint, which is already as
         * rare as it should be.
         *
         * The gate deliberately stops at a rate cap and does not also wait on the compositor (DwmFlush). Waiting for vsync
         * is the presenting code's job, and every graphics API offers it; doing it here as well would make an application
         * that already presents with vsync enabled run a resize drag at half its refresh rate, since each frame would wait
         * out two display periods instead of one.
         *
         * The callback is allowed to do almost anything, including publishing events and destroying its own window, so the
         * state is re-resolved by id afterwards rather than assuming @p ws is still alive.
         */
        void request_frame(window_state &ws) noexcept
        {
            if (!ws.frame_cb || ws.in_frame_callback)
                return;
            if (ws.display == window_display_state::minimized)
                return;

            const bool pace_to_display = ws.in_size_move;

            if (pace_to_display)
            {
                LARGE_INTEGER now{};
                QueryPerformanceCounter(&now);

                // Three quarters of a refresh period, not a whole one. The size/move timer is asked for the shortest
                // interval Windows accepts but is clamped to the system tick, which on a default configuration lands
                // within noise of a 60 Hz refresh period; gating on the full period would refuse every other tick for
                // being a fraction of a millisecond early and halve the frame rate of the drag. The margin makes this a
                // guard against a runaway tick rate rather than an attempt to pace exactly, which is the presenting
                // code's job in any case.
                const LONGLONG minimum = frame_interval_qpc() - frame_interval_qpc() / 4;

                if (ws.last_frame_qpc != 0 && (now.QuadPart - ws.last_frame_qpc) < minimum)
                    return;

                ws.last_frame_qpc = now.QuadPart;
            }

            const window_id id = ws.id;

            ws.in_frame_callback = true;
            ws.frame_cb(window{id}, ws.frame_user);

            window_state *still_alive = window_state_from_id(id);
            if (!still_alive)
                return;

            still_alive->in_frame_callback = false;
        }

        // -------------------------------------------------------------------------------------------------------------
        // Message translation helpers
        // -------------------------------------------------------------------------------------------------------------

        /**
         * @fn to_input_mouse_button
         * @brief Maps a Win32 mouse-button message (down, up or double-click) to the input::mouse_button it concerns.
         */
        input::mouse_button to_input_mouse_button(UINT msg, WPARAM wparam) noexcept
        {
            switch (msg)
            {
            case WM_LBUTTONDOWN:
            case WM_LBUTTONUP:
            case WM_LBUTTONDBLCLK:
                return input::mouse_button::left;
            case WM_RBUTTONDOWN:
            case WM_RBUTTONUP:
            case WM_RBUTTONDBLCLK:
                return input::mouse_button::right;
            case WM_MBUTTONDOWN:
            case WM_MBUTTONUP:
            case WM_MBUTTONDBLCLK:
                return input::mouse_button::middle;
            case WM_XBUTTONDOWN:
            case WM_XBUTTONUP:
            case WM_XBUTTONDBLCLK:
                // Windows only has two extended buttons, XBUTTON1 and XBUTTON2.
                return (GET_XBUTTON_WPARAM(wparam) == XBUTTON1) ? input::mouse_button::x1 : input::mouse_button::x2;
            default:
                return input::mouse_button::unknown;
            }
        }

        /**
         * @fn buttons_from_wparam
         * @brief The set of buttons held according to the MK_* flags a mouse message carries in its WPARAM.
         */
        input::mouse_buttons buttons_from_wparam(WPARAM wparam) noexcept
        {
            input::mouse_buttons b = input::mouse_buttons::none;
            if (wparam & MK_LBUTTON)
                b |= input::mouse_buttons::left;
            if (wparam & MK_RBUTTON)
                b |= input::mouse_buttons::right;
            if (wparam & MK_MBUTTON)
                b |= input::mouse_buttons::middle;
            if (wparam & MK_XBUTTON1)
                b |= input::mouse_buttons::x1;
            if (wparam & MK_XBUTTON2)
                b |= input::mouse_buttons::x2;
            return b;
        }

        /**
         * @fn current_modifiers
         * @brief Samples the modifier keys and lock states with GetKeyState, which is synchronised with the message being
         * processed, so the result reflects the state at the time of the event rather than "now".
         */
        input::key_modifiers current_modifiers() noexcept
        {
            input::key_modifiers mods = input::key_modifiers::none;

            if ((GetKeyState(VK_SHIFT) & 0x8000) != 0)
                mods |= input::key_modifiers::shift;
            if ((GetKeyState(VK_CONTROL) & 0x8000) != 0)
                mods |= input::key_modifiers::control;
            if ((GetKeyState(VK_MENU) & 0x8000) != 0)
                mods |= input::key_modifiers::alt;
            if ((GetKeyState(VK_LWIN) & 0x8000) != 0 || (GetKeyState(VK_RWIN) & 0x8000) != 0)
                mods |= input::key_modifiers::super;

            if ((GetKeyState(VK_CAPITAL) & 0x0001) != 0)
                mods |= input::key_modifiers::caps_lock;
            if ((GetKeyState(VK_NUMLOCK) & 0x0001) != 0)
                mods |= input::key_modifiers::num_lock;

            return mods;
        }

        /**
         * @var k_scancode_to_hid
         * @brief Scan code set 1 (what Windows reports in bits 16-23 of a key message's LPARAM) to USB HID keyboard usage,
         * for keys *without* the extended (0xE0) prefix. Indexed by scan code; 0 means "no mapping". Scan codes describe
         * the physical key regardless of the active keyboard layout, which is exactly what input::key_code represents.
         */
        constexpr std::uint8_t k_scancode_to_hid[128] = {
            /* 0x00 */ 0, 41, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 45, 46, 42, 43,
            /* 0x10 */ 20, 26, 8, 21, 23, 28, 24, 12, 18, 19, 47, 48, 40, 224, 4, 22,
            /* 0x20 */ 7, 9, 10, 11, 13, 14, 15, 51, 52, 53, 225, 49, 29, 27, 6, 25,
            /* 0x30 */ 5, 17, 16, 54, 55, 56, 229, 85, 226, 44, 57, 58, 59, 60, 61, 62,
            /* 0x40 */ 63, 64, 65, 66, 67, 83, 71, 95, 96, 97, 86, 92, 93, 94, 87, 89,
            /* 0x50 */ 90, 91, 98, 99, 70, 0, 100, 68, 69, 103, 0, 0, 140, 0, 0, 0,
            /* 0x60 */ 0, 0, 0, 0, 104, 105, 106, 107, 108, 109, 110, 111, 112, 113, 114, 0,
            /* 0x70 */ 136, 145, 144, 135, 0, 0, 115, 147, 146, 138, 0, 139, 0, 137, 0, 0,
        };

        /**
         * @fn to_input_key_code
         * @brief Translates the scan code and virtual key of a WM_(SYS)KEY* message into the physical key it belongs to.
         * @param wparam The virtual key code. Only consulted for keys whose scan codes collide (Pause/NumLock) or when the
         * message carries no scan code at all (input injected with SendInput), in which case the scan code is recovered
         * with MapVirtualKey.
         * @param lparam The message's LPARAM: bits 16-23 are the scan code, bit 24 the extended-key flag.
         * @param scancode_out Receives the raw scan code, with 0xE000 added for extended keys, for key_event::scancode.
         * @return The key, or input::key_code::unknown for keys Catalyst has no name for (browser/media keys and the
         * phantom Shift messages Windows synthesises around keypad keys).
         */
        input::key_code to_input_key_code(WPARAM wparam, LPARAM lparam, std::uint32_t &scancode_out) noexcept
        {
            using input::key_code;

            const UINT vk = static_cast<UINT>(wparam);
            UINT sc = static_cast<UINT>((lparam >> 16) & 0xFFu);
            bool extended = (lparam & (1 << 24)) != 0;

            if (sc == 0)
            {
                const UINT mapped = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC_EX);
                sc = mapped & 0xFFu;
                extended = (mapped & 0xFF00u) != 0;
            }

            scancode_out = extended ? (0xE000u | sc) : sc;

            // Pause and NumLock share scan code 0x45 (Pause's 0xE1 prefix is dropped by Windows) and PrintScreen shows up
            // with several different codes, so those three are resolved from the virtual key.
            switch (vk)
            {
            case VK_PAUSE:
            case VK_CANCEL:
                return key_code::pause;
            case VK_NUMLOCK:
                return key_code::num_lock;
            case VK_SNAPSHOT:
                return key_code::print_screen;
            default:
                break;
            }

            if (extended)
            {
                switch (sc)
                {
                case 0x1C: return key_code::keypad_enter;
                case 0x1D: return key_code::right_control;
                case 0x20: return key_code::mute;
                case 0x2E: return key_code::volume_down;
                case 0x30: return key_code::volume_up;
                case 0x35: return key_code::keypad_divide;
                case 0x37: return key_code::print_screen;
                case 0x38: return key_code::right_alt;
                case 0x47: return key_code::home;
                case 0x48: return key_code::up_arrow;
                case 0x49: return key_code::page_up;
                case 0x4B: return key_code::left_arrow;
                case 0x4D: return key_code::right_arrow;
                case 0x4F: return key_code::end;
                case 0x50: return key_code::down_arrow;
                case 0x51: return key_code::page_down;
                case 0x52: return key_code::insert;
                case 0x53: return key_code::delete_key;
                case 0x5B: return key_code::left_super;
                case 0x5C: return key_code::right_super;
                case 0x5D: return key_code::application;
                case 0x5E: return key_code::power;
                default:
                    return key_code::unknown; // includes the fake 0xE02A / 0xE036 Shift around keypad keys
                }
            }

            if (sc < 128 && k_scancode_to_hid[sc] != 0)
                return static_cast<key_code>(k_scancode_to_hid[sc]);

            return key_code::unknown;
        }

        /**
         * @fn utf32_from_utf16_unit
         * @brief Reassembles the UTF-16 code units Windows delivers in WM_CHAR into a single code point, buffering a high
         * surrogate until its low surrogate arrives.
         * @return The code point, or 0 if this unit was a high surrogate (nothing to publish yet) or a stray low surrogate.
         */
        input::character_code utf32_from_utf16_unit(window_state &ws, wchar_t unit) noexcept
        {
            namespace utf8 = catalyst::text::utf8;

            const char32_t u = static_cast<std::uint16_t>(unit);

            if (utf8::is_high_surrogate(u))
            {
                ws.pending_high_surrogate = unit;
                return 0;
            }

            if (utf8::is_low_surrogate(u))
            {
                if (ws.pending_high_surrogate == 0)
                    return 0; // stray low surrogate

                const char32_t hi = static_cast<std::uint16_t>(ws.pending_high_surrogate);
                ws.pending_high_surrogate = 0;
                return static_cast<input::character_code>(utf8::combine_surrogates(hi, u));
            }

            ws.pending_high_surrogate = 0;
            return static_cast<input::character_code>(u);
        }

        /** @brief True for code points that are control characters rather than text (C0 controls and DEL). */
        bool is_control_character(input::character_code cp) noexcept
        {
            return cp < 0x20u || cp == 0x7Fu;
        }

        /** @brief Cursor position of a client-area mouse message. */
        math::vec2<std::int32_t> client_pos_from_lparam(LPARAM lparam) noexcept
        {
            return {static_cast<std::int32_t>(GET_X_LPARAM(lparam)), static_cast<std::int32_t>(GET_Y_LPARAM(lparam))};
        }

        /** @brief Cursor position of a message that reports screen coordinates (wheel messages), converted to client space. */
        math::vec2<std::int32_t> client_pos_from_screen_lparam(HWND hwnd, LPARAM lparam) noexcept
        {
            POINT p{GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam)};
            ScreenToClient(hwnd, &p);
            return {static_cast<std::int32_t>(p.x), static_cast<std::int32_t>(p.y)};
        }

        // -------------------------------------------------------------------------------------------------------------
        // Cursor mode and raw input
        // -------------------------------------------------------------------------------------------------------------

        /** @brief Registers for WM_INPUT mouse messages targeted at @p hwnd (replacing any previous registration). */
        void register_raw_mouse(HWND hwnd) noexcept
        {
            RAWINPUTDEVICE rid{};
            rid.usUsagePage = input::usb_hid_page_generic_desktop;
            rid.usUsage = 0x02; // HID_USAGE_GENERIC_MOUSE
            rid.dwFlags = 0;
            rid.hwndTarget = hwnd;
            RegisterRawInputDevices(&rid, 1, sizeof(rid));
        }

        /** @brief Stops WM_INPUT mouse delivery for the process. */
        void unregister_raw_mouse() noexcept
        {
            RAWINPUTDEVICE rid{};
            rid.usUsagePage = input::usb_hid_page_generic_desktop;
            rid.usUsage = 0x02;
            rid.dwFlags = RIDEV_REMOVE;
            rid.hwndTarget = nullptr;
            RegisterRawInputDevices(&rid, 1, sizeof(rid));
        }

        /** @brief Confines the cursor to the window's client area. */
        void clip_cursor_to_client(HWND hwnd) noexcept
        {
            RECT rc{};
            if (!GetClientRect(hwnd, &rc))
                return;

            POINT tl{rc.left, rc.top};
            POINT br{rc.right, rc.bottom};
            ClientToScreen(hwnd, &tl);
            ClientToScreen(hwnd, &br);

            RECT clip{tl.x, tl.y, br.x, br.y};
            ClipCursor(&clip);
        }

        /**
         * @fn apply_cursor_mode
         * @brief Brings the OS state (cursor clip, raw input registration, cursor image) in line with the window's cursor
         * mode and focus. Called whenever either changes and whenever the window moves or resizes while captured.
         */
        void apply_cursor_mode(window_state &ws, bool focused) noexcept
        {
            if (ws.cursor == cursor_mode::captured && focused)
            {
                clip_cursor_to_client(ws.hwnd);
                if (g_raw_mouse_window != ws.id)
                {
                    register_raw_mouse(ws.hwnd);
                    g_raw_mouse_window = ws.id;
                    g_has_last_raw_absolute = false;
                }
            }
            else if (g_raw_mouse_window == ws.id)
            {
                ClipCursor(nullptr);
                unregister_raw_mouse();
                g_raw_mouse_window = 0;
            }

            // The cursor image is chosen in WM_SETCURSOR, which only fires on mouse movement; nudge it so a mode change
            // applies immediately when the cursor is already over the client area.
            POINT p{};
            if (GetCursorPos(&p) && WindowFromPoint(p) == ws.hwnd)
                PostMessageW(ws.hwnd, WM_SETCURSOR, reinterpret_cast<WPARAM>(ws.hwnd), MAKELPARAM(HTCLIENT, WM_MOUSEMOVE));
        }

        /**
         * @brief apply_cursor_mode() with the focus state taken from GetFocus(). Not usable from WM_KILLFOCUS, where the
         * window still reports as focused; pass the state explicitly there.
         */
        void apply_cursor_mode(window_state &ws) noexcept
        {
            apply_cursor_mode(ws, GetFocus() == ws.hwnd);
        }

        /**
         * @fn release_captured_buttons
         * @brief Feeds a release for every mouse button this window holds the capture for, and drops the capture.
         * @details Used when the capture is taken away by another window (WM_CAPTURECHANGED): the matching button-up
         * messages will be delivered to whoever took it, not to us, so they have to be produced here or the buttons stay
         * down forever.
         * @note This reads the backend's own buttons_down rather than asking the input module, because what is being undone
         * is the capture this backend took - a platform concern, tracked for the capture's sake either way. Focus loss is
         * different and does not come through here: it releases the *device* state, keys included, which the registry
         * knows better than this backend ever could. See feed_focus_lost at WM_KILLFOCUS.
         */
        void release_captured_buttons(window_state &ws) noexcept
        {
            const input::mouse_buttons held = ws.buttons_down;
            ws.buttons_down = input::mouse_buttons::none;
            ws.holds_capture = false;

            if (held == input::mouse_buttons::none || !g_feed)
                return;

            const input::key_modifiers mods = current_modifiers();
            for (std::size_t i = 0; i < input::mouse_button_count; ++i)
            {
                const auto b = static_cast<input::mouse_button>(i);
                if (!input::has_button(held, b))
                    continue;

                input::mouse_button_event be;
                be.window = ws.id;
                be.button = b;
                be.action = input::button_action::release;
                be.position_px = ws.last_mouse_pos_px;
                be.modifiers = mods;
                g_feed->feed_mouse_button(be);
            }
        }

        /**
         * @fn feed_text
         * @brief Feeds one code point as a text_input_event, unless it is a control character.
         */
        void feed_text(window_id id, input::character_code cp) noexcept
        {
            if (cp == 0 || is_control_character(cp) || !g_feed)
                return;

            input::text_input_event te(cp);
            te.window = id;
            te.modifiers = current_modifiers();
            g_feed->feed_text(te);
        }

        /** @brief Turns one raw mouse record into a mouse_raw_move_event, if it carries any motion. */
        void handle_raw_mouse(window_state &ws, const RAWMOUSE &m) noexcept
        {
            math::vec2<std::int32_t> delta{};

            if ((m.usFlags & MOUSE_MOVE_ABSOLUTE) != 0)
            {
                // Remote desktop / tablets report absolute positions; turn them into deltas ourselves.
                const math::vec2<std::int32_t> abs{static_cast<std::int32_t>(m.lLastX), static_cast<std::int32_t>(m.lLastY)};
                if (g_has_last_raw_absolute)
                    delta = {abs.x() - g_last_raw_absolute.x(), abs.y() - g_last_raw_absolute.y()};
                g_last_raw_absolute = abs;
                g_has_last_raw_absolute = true;
            }
            else
            {
                delta = {static_cast<std::int32_t>(m.lLastX), static_cast<std::int32_t>(m.lLastY)};
            }

            if (delta.x() == 0 && delta.y() == 0)
                return;

            input::mouse_raw_move_event re;
            re.window = ws.id;
            re.delta = delta;
            g_feed->feed_mouse_raw_move(re);
        }

        /**
         * @fn drain_raw_input
         * @brief Reads every buffered raw input record in as few calls as possible and turns each into an event.
         * @details A high-polling-rate mouse produces one WM_INPUT per report, so a 1000 Hz mouse costs sixteen messages
         * and sixteen GetRawInputData round trips per frame at 60 Hz. GetRawInputBuffer returns as many records as fit in
         * one buffer, which collapses that into a single call. Records consumed here will not be returned again by
         * GetRawInputData for the WM_INPUT messages still sitting in the queue behind this one; those calls simply fail and
         * are ignored, which is why the caller treats a failure as "nothing to do" rather than an error.
         * @return True if at least one record was processed, false if the buffered read is unavailable or found nothing, in
         * which case the caller should fall back to reading this single message's record.
         */
        bool drain_raw_input(window_state &ws) noexcept
        {
            // RAWINPUT records must be 8-byte aligned for NEXTRAWINPUTBLOCK to walk them.
            alignas(8) static BYTE buffer[16 * 1024];

            bool processed_any = false;

            for (;;)
            {
                UINT size = sizeof(buffer);
                const UINT count = GetRawInputBuffer(reinterpret_cast<PRAWINPUT>(buffer), &size, sizeof(RAWINPUTHEADER));

                if (count == 0 || count == static_cast<UINT>(-1))
                    break;

                PRAWINPUT record = reinterpret_cast<PRAWINPUT>(buffer);
                for (UINT i = 0; i < count; ++i)
                {
                    if (record->header.dwType == RIM_TYPEMOUSE)
                        handle_raw_mouse(ws, record->data.mouse);

                    // The SDK's NEXTRAWINPUTBLOCK expands to a QWORD that not every SDK/compiler combination declares, so
                    // the 8-byte alignment step it performs is spelled out here instead.
                    constexpr ULONG_PTR align = 8;
                    const ULONG_PTR next = (reinterpret_cast<ULONG_PTR>(record) + record->header.dwSize + align - 1) & ~(align - 1);
                    record = reinterpret_cast<PRAWINPUT>(next);
                }

                processed_any = true;
            }

            return processed_any;
        }

        // -------------------------------------------------------------------------------------------------------------
        // Window procedure
        // -------------------------------------------------------------------------------------------------------------

        /**
         * @fn window_proc
         * @brief The window procedure for every window this backend creates. Translates window and input messages into
         * Catalyst events (see enqueue_event) and forwards everything else to DefWindowProcW.
         * @note The per-window state is attached in WM_NCCREATE, so every message except WM_GETMINMAXINFO (which Windows
         * sends first) sees a valid state pointer. Messages that arrive before create_window returns are handled but
         * publish nothing, because the application has not yet been told the window id; see window_state::creating.
         */
        LRESULT CALLBACK window_proc(HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam)
        {
            if (msg == WM_NCCREATE)
            {
                // The state was created before CreateWindowExW and handed over as the creation parameter, so it is attached
                // here rather than after creation returns. Everything downstream can then assume it exists.
                auto *cs = reinterpret_cast<CREATESTRUCTW *>(lparam);
                auto *created = static_cast<window_state *>(cs->lpCreateParams);
                if (created)
                {
                    created->hwnd = hwnd;
                    SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(created));
                }
                return DefWindowProcW(hwnd, msg, wparam, lparam);
            }

            window_state *ws = state_from_hwnd(hwnd);
            if (!ws)
                return DefWindowProcW(hwnd, msg, wparam, lparam);

            const window_id id = ws->id;
            const bool publishes = !ws->creating;

            switch (msg)
            {
            // ---- Window lifecycle -------------------------------------------------------------------------------
            case WM_CLOSE:
            {
                if (publishes)
                {
                    window_close_requested_event we;
                    we.window = id;
                    dispatch_event(we);
                }
                return 0; // app decides when to destroy
            }
            case WM_DESTROY:
            {
                if (publishes)
                {
                    window_destroyed_event we;
                    we.window = id;
                    dispatch_event(we);
                }
                return 0;
            }
            case WM_NCDESTROY:
            {
                // The last message a window receives. Everything the window owned on the process's behalf is given back
                // here rather than in destroy_window, so a window torn down by any other route cleans up just as well.
                if (ws->holds_capture)
                    ReleaseCapture();
                if (g_raw_mouse_window == id)
                {
                    ClipCursor(nullptr);
                    unregister_raw_mouse();
                    g_raw_mouse_window = 0;
                }
                if (ws->in_size_move)
                    KillTimer(hwnd, k_size_move_timer_id);

                SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
                g_windows.erase(id); // ws dangles past this point
                return DefWindowProcW(hwnd, msg, wparam, lparam);
            }
            case WM_SIZE:
            {
                const auto size_kind = static_cast<UINT>(wparam);
                const window_display_state state = (size_kind == SIZE_MINIMIZED)  ? window_display_state::minimized
                                                   : (size_kind == SIZE_MAXIMIZED) ? window_display_state::maximized
                                                                                   : window_display_state::restored;

                if (state != ws->display)
                {
                    ws->display = state;
                    if (publishes)
                    {
                        window_display_state_event de;
                        de.window = id;
                        de.state = state;
                        dispatch_event(de);
                    }
                }

                if (publishes)
                {
                    window_resized_event we;
                    we.window = id;
                    we.width_px = ui::px(static_cast<float>(LOWORD(lparam)));
                    we.height_px = ui::px(static_cast<float>(HIWORD(lparam)));
                    dispatch_event(we);
                }

                if (ws->cursor == cursor_mode::captured)
                    apply_cursor_mode(*ws);

                // Draw the new size straight away. During a resize drag this is the message that arrives per pixel of
                // mouse travel, so rendering from here is what makes the window track the border instead of lagging it.
                if (publishes)
                    request_frame(*ws);

                break;
            }
            case WM_MOVE:
            {
                if (publishes)
                {
                    // For an overlapped window WM_MOVE reports the client area's top-left in screen coordinates, which is
                    // exactly the mapping applications need. The coordinates are signed: a monitor left of or above the
                    // primary one has negative coordinates.
                    window_moved_event me;
                    me.window = id;
                    me.position_px = client_pos_from_lparam(lparam);
                    dispatch_event(me);
                }

                if (ws->cursor == cursor_mode::captured)
                    apply_cursor_mode(*ws);
                break;
            }
            case WM_GETMINMAXINFO:
            {
                // Windows sends this before WM_NCCREATE during creation, when there is nothing to constrain yet.
                if (ws->min_size_px.x() <= 0 && ws->min_size_px.y() <= 0 && ws->max_size_px.x() <= 0 && ws->max_size_px.y() <= 0)
                    break;

                auto *mmi = reinterpret_cast<MINMAXINFO *>(lparam);

                if (ws->min_size_px.x() > 0 || ws->min_size_px.y() > 0)
                {
                    const auto outer = window_size_for_client(hwnd, ws->min_size_px);
                    if (ws->min_size_px.x() > 0)
                        mmi->ptMinTrackSize.x = outer.x();
                    if (ws->min_size_px.y() > 0)
                        mmi->ptMinTrackSize.y = outer.y();
                }

                if (ws->max_size_px.x() > 0 || ws->max_size_px.y() > 0)
                {
                    const auto outer = window_size_for_client(hwnd, ws->max_size_px);
                    if (ws->max_size_px.x() > 0)
                        mmi->ptMaxTrackSize.x = outer.x();
                    if (ws->max_size_px.y() > 0)
                        mmi->ptMaxTrackSize.y = outer.y();
                }

                return 0;
            }
            case WM_ENTERSIZEMOVE:
            {
                // From here until WM_EXITSIZEMOVE the thread is inside the OS's own message loop and pump_events() will
                // not return. The timer is the only thing that gets control back when the drag is not producing messages.
                ws->in_size_move = true;
                ws->last_frame_qpc = 0; // let the first frame of the drag go out immediately
                SetTimer(hwnd, k_size_move_timer_id, USER_TIMER_MINIMUM, nullptr);

                if (publishes)
                {
                    window_enter_size_move_event we;
                    we.window = id;
                    dispatch_event(we);
                }
                break;
            }
            case WM_EXITSIZEMOVE:
            {
                KillTimer(hwnd, k_size_move_timer_id);
                ws->in_size_move = false;

                if (publishes)
                {
                    window_exit_size_move_event we;
                    we.window = id;
                    dispatch_event(we);
                }
                break;
            }
            case WM_TIMER:
            {
                if (wparam == k_size_move_timer_id)
                {
                    request_frame(*ws);
                    return 0;
                }
                break;
            }
            case WM_PAINT:
            {
                if (ws->frame_cb)
                {
                    // The update region has to be validated whether or not anything is drawn through the DC, otherwise
                    // Windows keeps re-sending WM_PAINT forever. The actual drawing goes through the application's own
                    // graphics API, not through this DC.
                    PAINTSTRUCT ps{};
                    BeginPaint(hwnd, &ps);
                    EndPaint(hwnd, &ps);
                    request_frame(*ws);
                    return 0;
                }
                break;
            }
            case WM_ERASEBKGND:
            {
                // The client area is owned by the application's renderer; letting the OS erase it first would show a flash
                // of background colour on every resize.
                return 1;
            }
            case WM_DPICHANGED:
            {
                // Resize to the rect the OS suggests for the new DPI, which keeps the window the same physical size.
                const RECT *suggested = reinterpret_cast<const RECT *>(lparam);
                SetWindowPos(hwnd, nullptr,
                             suggested->left,
                             suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);

                if (publishes)
                {
                    window_dpi_changed_event we;
                    we.window = id;
                    we.dpi_scale = scale_from_dpi(HIWORD(wparam)); // the message already carries the new DPI
                    dispatch_event(we);
                }
                return 0;
            }
            case WM_DISPLAYCHANGE:
            {
                // Broadcast to every top-level window, so it would otherwise be republished once per window we own. All
                // copies of one broadcast share a message time, which is enough to recognise and drop the duplicates.
                g_frame_interval_qpc = 0; // the refresh rate may have changed with the mode

                static LONG s_last_display_change = 0;
                const LONG when = GetMessageTime();
                if (publishes && when != s_last_display_change)
                {
                    s_last_display_change = when;
                    for (const monitor_desc &m : get_monitor_list())
                    {
                        monitor_changed_event me;
                        me.desc = m;
                        dispatch_event(me);
                    }
                }
                break;
            }

            // ---- Focus ------------------------------------------------------------------------------------------
            case WM_SETFOCUS:
            {
                if (publishes)
                {
                    window_focus_event fe;
                    fe.window = id;
                    fe.focused = true;
                    dispatch_event(fe);
                }

                apply_cursor_mode(*ws, true);
                break;
            }
            case WM_KILLFOCUS:
            {
                if (publishes)
                {
                    // One call, and the registry produces exactly the releases its own state calls for - every key and
                    // button this window still holds, and nothing that it does not. Windows delivers the key-up messages
                    // for anything released after Alt+Tab to whoever gained focus, so without this they never arrive.
                    if (g_feed)
                        g_feed->feed_focus_lost(id);

                    ws->buttons_down = input::mouse_buttons::none;
                    ws->holds_capture = false;
                }

                apply_cursor_mode(*ws, false); // suspends the clip / raw input while unfocused

                if (publishes)
                {
                    window_focus_event fe;
                    fe.window = id;
                    fe.focused = false;
                    dispatch_event(fe);
                }
                break;
            }
            case WM_CAPTURECHANGED:
            {
                // Another window (possibly in another process) took the capture: the button-up messages will never
                // reach us, so report the buttons as released now.
                if (reinterpret_cast<HWND>(lparam) != hwnd && ws->holds_capture)
                    release_captured_buttons(*ws);
                break;
            }
            case WM_SYSCOMMAND:
            {
                // Alt or F10 on their own would enter the menu loop and stall the message pump; Alt+F4 (SC_CLOSE) and
                // the rest still go through.
                if ((wparam & 0xFFF0u) == SC_KEYMENU)
                    return 0;
                break;
            }
            case WM_MENUCHAR:
            {
                // Silence the beep for Alt+<key> combinations, which have no menu to accelerate.
                return MAKELRESULT(0, MNC_CLOSE);
            }
            case WM_SETCURSOR:
            {
                if (LOWORD(lparam) == HTCLIENT && ws->cursor != cursor_mode::normal)
                {
                    SetCursor(nullptr);
                    return TRUE;
                }
                break;
            }

            // ---- Keyboard ---------------------------------------------------------------------------------------
            case WM_KEYDOWN:
            case WM_SYSKEYDOWN:
            {
                if (publishes)
                {
                    const bool repeat = (lparam & (1 << 30)) != 0;

                    input::key_event ke;
                    ke.window = id;
                    ke.code = to_input_key_code(wparam, lparam, ke.scancode);
                    ke.action = repeat ? input::button_action::repeat : input::button_action::press;
                    ke.modifiers = current_modifiers();

                    // Phantom Shift messages around keypad keys map to unknown with a Shift virtual key; drop them.
                    if (ke.code == input::key_code::unknown && wparam == VK_SHIFT)
                        break;

                    if (g_feed)
                        g_feed->feed_key(ke);
                }
                break;
            }
            case WM_KEYUP:
            case WM_SYSKEYUP:
            {
                if (publishes)
                {
                    input::key_event ke;
                    ke.window = id;
                    ke.code = to_input_key_code(wparam, lparam, ke.scancode);
                    ke.action = input::button_action::release;
                    ke.modifiers = current_modifiers();

                    if (ke.code == input::key_code::unknown && wparam == VK_SHIFT)
                        break;

                    if (g_feed)
                        g_feed->feed_key(ke);
                }
                break;
            }
            case WM_CHAR:
            {
                if (publishes)
                    feed_text(id, utf32_from_utf16_unit(*ws, static_cast<wchar_t>(wparam)));
                break;
            }
            case WM_UNICHAR:
            {
                // Some input tools send UTF-32 directly; answering TRUE to UNICODE_NOCHAR tells them we accept it.
                if (wparam == UNICODE_NOCHAR)
                    return TRUE;
                if (publishes)
                    feed_text(id, static_cast<input::character_code>(wparam));
                return 0;
            }

            // ---- Mouse ------------------------------------------------------------------------------------------
            case WM_MOUSEMOVE:
            {
                if (publishes)
                {
                    const math::vec2<std::int32_t> pos = client_pos_from_lparam(lparam);

                    if (!ws->mouse_inside)
                    {
                        ws->mouse_inside = true;

                        TRACKMOUSEEVENT tme{};
                        tme.cbSize = sizeof(tme);
                        tme.dwFlags = TME_LEAVE;
                        tme.hwndTrack = hwnd;
                        TrackMouseEvent(&tme);

                        input::mouse_enter_event ee;
                        ee.window = id;
                        ee.position_px = pos;
                        if (g_feed)
                            g_feed->feed_mouse_enter(ee);
                    }

                    const math::vec2<std::int32_t> last = ws->has_mouse_pos ? ws->last_mouse_pos_px : pos;
                    ws->last_mouse_pos_px = pos;
                    ws->has_mouse_pos = true;

                    input::mouse_move_event me;
                    me.window = id;
                    me.position_px = pos;
                    me.delta_px = {pos.x() - last.x(), pos.y() - last.y()};
                    me.buttons = buttons_from_wparam(wparam);
                    me.modifiers = current_modifiers();
                    if (g_feed)
                        g_feed->feed_mouse_move(me);
                }
                break;
            }
            case WM_MOUSELEAVE:
            {
                if (ws->mouse_inside)
                {
                    ws->mouse_inside = false;

                    if (publishes)
                    {
                        input::mouse_leave_event le;
                        le.window = id;
                        if (g_feed)
                            g_feed->feed_mouse_leave(le);
                    }
                }
                break;
            }
            case WM_LBUTTONDOWN:
            case WM_RBUTTONDOWN:
            case WM_MBUTTONDOWN:
            case WM_XBUTTONDOWN:
            case WM_LBUTTONDBLCLK:
            case WM_RBUTTONDBLCLK:
            case WM_MBUTTONDBLCLK:
            case WM_XBUTTONDBLCLK:
            {
                if (publishes)
                {
                    const bool double_click = (msg == WM_LBUTTONDBLCLK || msg == WM_RBUTTONDBLCLK ||
                                               msg == WM_MBUTTONDBLCLK || msg == WM_XBUTTONDBLCLK);

                    input::mouse_button_event be;
                    be.window = id;
                    be.button = to_input_mouse_button(msg, wparam);
                    be.action = input::button_action::press;
                    be.clicks = double_click ? 2 : 1;
                    be.position_px = client_pos_from_lparam(lparam);
                    be.modifiers = current_modifiers();

                    // Capture on the first button so the matching release arrives even if the cursor leaves the window.
                    if (ws->buttons_down == input::mouse_buttons::none)
                    {
                        SetCapture(hwnd);
                        ws->holds_capture = true;
                    }
                    ws->buttons_down |= input::to_mouse_buttons(be.button);

                    if (g_feed)
                        g_feed->feed_mouse_button(be);
                }
                if (msg == WM_XBUTTONDOWN || msg == WM_XBUTTONDBLCLK)
                    return TRUE;
                break;
            }
            case WM_LBUTTONUP:
            case WM_RBUTTONUP:
            case WM_MBUTTONUP:
            case WM_XBUTTONUP:
            {
                if (publishes)
                {
                    input::mouse_button_event be;
                    be.window = id;
                    be.button = to_input_mouse_button(msg, wparam);
                    be.action = input::button_action::release;
                    be.clicks = 1;
                    be.position_px = client_pos_from_lparam(lparam);
                    be.modifiers = current_modifiers();

                    ws->buttons_down &= ~input::to_mouse_buttons(be.button);
                    if (ws->buttons_down == input::mouse_buttons::none && ws->holds_capture)
                    {
                        ws->holds_capture = false;
                        ReleaseCapture();
                    }

                    if (g_feed)
                        g_feed->feed_mouse_button(be);
                }
                if (msg == WM_XBUTTONUP)
                    return TRUE;
                break;
            }
            case WM_MOUSEWHEEL:
            case WM_MOUSEHWHEEL:
            {
                if (publishes)
                {
                    const float notches = static_cast<float>(GET_WHEEL_DELTA_WPARAM(wparam)) / static_cast<float>(WHEEL_DELTA);

                    input::mouse_wheel_event we;
                    we.window = id;
                    we.position_px = client_pos_from_screen_lparam(hwnd, lparam);
                    we.delta = (msg == WM_MOUSEWHEEL) ? math::vec2<float>{0.0f, notches} : math::vec2<float>{notches, 0.0f};
                    we.modifiers = current_modifiers();
                    if (g_feed)
                        g_feed->feed_mouse_wheel(we);
                }
                break;
            }
            case WM_INPUT:
            {
                if (publishes && ws->cursor == cursor_mode::captured && g_raw_mouse_window == id)
                {
                    // Prefer the buffered read, which drains every report queued behind this one in a single call.
                    if (!drain_raw_input(*ws))
                    {
                        RAWINPUT raw{};
                        UINT size = sizeof(raw);
                        const UINT got = GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &raw, &size,
                                                         sizeof(RAWINPUTHEADER));
                        if (got != static_cast<UINT>(-1) && raw.header.dwType == RIM_TYPEMOUSE)
                            handle_raw_mouse(*ws, raw.data.mouse);
                    }
                }
                break; // DefWindowProc must see WM_INPUT so the system can clean up
            }
            default:
                break;
            }

            return DefWindowProcW(hwnd, msg, wparam, lparam);
        }

        /**
         * @fn ensure_window_class_registered
         * @brief Registers the window class the backend's windows are created from, once per process.
         * @return True if the class is registered and usable.
         */
        bool ensure_window_class_registered() noexcept
        {
            static bool registered = false;
            if (registered)
                return true;

            WNDCLASSEXW wc{};
            wc.cbSize = sizeof(wc);
            wc.style = CS_DBLCLKS; // deliver WM_*BUTTONDBLCLK so mouse_button_event::clicks can report double-clicks
            wc.lpfnWndProc = &window_proc;
            wc.hInstance = GetModuleHandleW(nullptr);
            wc.lpszClassName = k_window_class_name;
            wc.hCursor = LoadCursorW(nullptr, MAKEINTRESOURCEW(32512)); // IDC_ARROW
            // hbrBackground is deliberately left null: the client area belongs to the application's renderer, and any
            // background brush would be painted over it on every resize.

            if (RegisterClassExW(&wc) == 0)
            {
                // A class of this name already existing is success as far as we are concerned; anything else is not.
                if (GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
                    return false;
            }

            registered = true;
            return true;
        }

        /** @brief The style bits that correspond to a resizable window. */
        constexpr DWORD k_resizable_style_bits = WS_THICKFRAME | WS_MAXIMIZEBOX;

        /** @brief The base window style for a window with the given resizability. */
        constexpr DWORD style_for(bool resizable) noexcept
        {
            const DWORD base = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX;
            return resizable ? (base | k_resizable_style_bits) : base;
        }

        /** @brief Applies a DwmSetWindowAttribute BOOL attribute, trying the current id and then the pre-18985 one. */
        void set_dwm_bool_attribute(HWND hwnd, DWORD attribute, DWORD legacy_attribute, bool value) noexcept
        {
            const BOOL v = value ? TRUE : FALSE;
            if (FAILED(DwmSetWindowAttribute(hwnd, attribute, &v, sizeof(v))))
                DwmSetWindowAttribute(hwnd, legacy_attribute, &v, sizeof(v));
        }
    } // namespace

    // -----------------------------------------------------------------------------------------------------------------
    // Window lifecycle
    // -----------------------------------------------------------------------------------------------------------------

    /**
     * @fn create_window
     * @brief Creates a window from @p desc and returns the id that identifies it for the rest of the platform API.
     * @details The per-window state is inserted into the registry before CreateWindowExW and handed to it as the creation
     * parameter, so the window procedure has it from WM_NCCREATE onwards rather than only after creation returns.
     *
     * Sizing happens twice on purpose. The requested client size can only be resolved against a DPI, and until the window
     * exists there is no way to know which display it will land on; the first pass uses the system DPI to get a window on
     * screen, and the second corrects the size if the window opened on a display with a different scale. Skipping the
     * correction would make a window opened on a secondary 150% display come out two thirds of its requested size.
     * @param desc The title, client size, visibility and resizability of the window to create.
     * @return The new window's id, or 0 if creation failed.
     */
    window_id create_window(const window_desc &desc)
    {
        ensure_backend_initialised();

        if (!ensure_window_class_registered())
            return 0;

        const window_id id = g_next_window_id++;

        const DWORD style = style_for(desc.resizable);
        const DWORD ex_style = WS_EX_APPWINDOW;

        std::wstring titleW = desc.title ? win32::utf8_to_wide_or_ansi(desc.title) : L"Catalyst";
        if (titleW.empty())
            titleW = L"Catalyst";

        window_state &ws = g_windows[id];
        ws.id = id;
        ws.creating = true;

        // First pass: size the window using the system DPI, which is the only DPI known before it exists.
        const UINT creation_dpi = dpi_for_system();
        const std::int32_t width_px = resolve_px(desc.width_px, ui::axis::x, scale_from_dpi(creation_dpi));
        const std::int32_t height_px = resolve_px(desc.height_px, ui::axis::y, scale_from_dpi(creation_dpi));

        RECT r{0, 0, static_cast<LONG>(width_px), static_cast<LONG>(height_px)};
        adjust_rect(r, style, ex_style, creation_dpi);

        HWND hwnd = CreateWindowExW(
            ex_style,
            k_window_class_name,
            titleW.c_str(),
            style,
            CW_USEDEFAULT,
            CW_USEDEFAULT,
            r.right - r.left,
            r.bottom - r.top,
            nullptr,
            nullptr,
            GetModuleHandleW(nullptr),
            &ws);

        if (!hwnd)
        {
            g_windows.erase(id);
            return 0;
        }

        // Second pass: the window now has a monitor, so re-resolve the requested client size at that monitor's DPI.
        const UINT actual_dpi = dpi_for_window(hwnd);
        if (actual_dpi != creation_dpi)
        {
            const float scale = scale_from_dpi(actual_dpi);
            RECT fixed{0, 0,
                       static_cast<LONG>(resolve_px(desc.width_px, ui::axis::x, scale)),
                       static_cast<LONG>(resolve_px(desc.height_px, ui::axis::y, scale))};
            adjust_rect(fixed, style, ex_style, actual_dpi);
            SetWindowPos(hwnd, nullptr, 0, 0, fixed.right - fixed.left, fixed.bottom - fixed.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }

        ws.creating = false;
        ws.display = IsIconic(hwnd) ? window_display_state::minimized
                     : IsZoomed(hwnd) ? window_display_state::maximized
                                      : window_display_state::restored;

        if (desc.visible)
            ShowWindow(hwnd, SW_SHOW);

        // Initial events. These are published here rather than from the messages that ran during CreateWindowExW, because
        // the application only learns the window id when this function returns.
        {
            window_resized_event e;
            e.window = id;
            RECT cr{};
            GetClientRect(hwnd, &cr);
            e.width_px = ui::px(static_cast<float>(cr.right - cr.left));
            e.height_px = ui::px(static_cast<float>(cr.bottom - cr.top));
            dispatch_event(e);
        }
        {
            window_moved_event e;
            e.window = id;
            POINT origin{0, 0};
            ClientToScreen(hwnd, &origin);
            e.position_px = {static_cast<std::int32_t>(origin.x), static_cast<std::int32_t>(origin.y)};
            dispatch_event(e);
        }
        {
            window_dpi_changed_event e;
            e.window = id;
            e.dpi_scale = scale_from_dpi(actual_dpi);
            dispatch_event(e);
        }
        if (GetFocus() == hwnd)
        {
            // WM_SETFOCUS was delivered while the window was still being created, so report it now.
            window_focus_event e;
            e.window = id;
            e.focused = true;
            dispatch_event(e);
        }

        return id;
    }

    /**
     * @fn destroy_window
     * @brief Destroys the window associated with @p id. The window procedure publishes window_destroyed_event and releases
     * everything the window owned (capture, cursor clip, raw input registration, timers) from WM_NCDESTROY, so nothing has
     * to be undone here.
     */
    void destroy_window(window_id id) noexcept
    {
        if (HWND hwnd = hwnd_from_id(id))
            DestroyWindow(hwnd);
    }

    /**
     * @fn is_window_valid
     * @brief Whether @p id refers to a window that still exists.
     */
    bool is_window_valid(window_id id) noexcept
    {
        HWND hwnd = hwnd_from_id(id);
        return hwnd != nullptr && IsWindow(hwnd);
    }

    /**
     * @fn get_native_handle
     * @brief The HWND behind a window id, plus the module instance, for code that needs to talk to Win32 or a graphics API
     * directly.
     */
    native_handle get_native_handle(window_id id) noexcept
    {
        HWND hwnd = hwnd_from_id(id);
        if (!hwnd)
            return {};

        native_handle h{};
        h.kind = native_handle_kind::win32_hwnd;
        h.handle = hwnd;
        h.extra = GetModuleHandleW(nullptr);
        return h;
    }

    /**
     * @fn client_rect_px
     * @brief The window's client area in pixels, with its origin at (0,0).
     */
    rect_px client_rect_px(window_id id) noexcept
    {
        HWND hwnd = hwnd_from_id(id);
        if (!hwnd)
            return {};

        RECT r{};
        GetClientRect(hwnd, &r);
        return {{static_cast<std::int32_t>(r.left), static_cast<std::int32_t>(r.top)},
                {static_cast<std::int32_t>(r.right), static_cast<std::int32_t>(r.bottom)}};
    }

    /**
     * @fn dpi_scale
     * @brief The window's DPI relative to the 96 DPI baseline.
     */
    float dpi_scale(window_id id) noexcept
    {
        ensure_backend_initialised();

        HWND hwnd = hwnd_from_id(id);
        if (!hwnd)
            return 1.0f;

        return dpi_scale_for_window(hwnd);
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Event pump
    // -----------------------------------------------------------------------------------------------------------------

    /**
     * @fn pump_events
     * @brief Drains every message waiting for this thread, translating each into Catalyst events through the window
     * procedure.
     * @note This does not return while the operating system is running a modal loop of its own (a resize or move drag, a
     * system menu, a modal dialog). That is what the frame callback exists for; see platform::set_frame_callback.
     */
    void pump_events() noexcept
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
        {
            // WM_QUIT belongs to the thread rather than to a window, so dispatching it would do nothing at all. Stop
            // draining instead, leaving anything behind it for the next call.
            if (msg.message == WM_QUIT)
                break;

            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
    }

    /**
     * @fn wait_events
     * @brief Blocks until a message or input arrives for this thread, or until @p timeout_ms elapses.
     * @param timeout_ms The maximum time to wait, or 0xFFFFFFFF to wait indefinitely.
     * @return True if something arrived, false on timeout or error.
     */
    bool wait_events(std::uint32_t timeout_ms) noexcept
    {
        const DWORD timeout = (timeout_ms == 0xFFFFFFFFu) ? INFINITE : static_cast<DWORD>(timeout_ms);

        // Wake for any kind of input/message without consuming it.
        const DWORD rc = MsgWaitForMultipleObjectsEx(
            0,
            nullptr,
            timeout,
            QS_ALLINPUT,
            MWMO_INPUTAVAILABLE);

        if (rc == WAIT_TIMEOUT)
            return false;
        else if (rc == WAIT_FAILED)
            return false;
        else
            return true;
    }

    /**
     * @fn set_event_bus
     * @brief Installs the bus window events are dispatched to, or nullptr to dispatch none.
     */
    void set_event_bus(events::bus *bus) noexcept
    {
        g_bus = bus;
    }

    /**
     * @fn event_bus
     * @brief The bus installed with set_event_bus, or nullptr.
     */
    events::bus *event_bus() noexcept
    {
        return g_bus;
    }

    /**
     * @fn set_input_feed
     * @brief Installs the feed window-sourced input is delivered to, or nullptr to deliver none.
     * @details Installing a feed mid-session is safe: this backend keeps no input state of its own beyond the mouse
     * capture, so a feed that arrives late simply starts receiving events from the next message, and one that is removed
     * stops receiving them. Neither leaves anything stuck down, because what is held is the registry's state, not this
     * backend's.
     */
    void set_input_feed(input::event_feed *feed) noexcept
    {
        g_feed = feed;
    }

    /**
     * @fn input_feed
     * @brief The feed installed with set_input_feed, or nullptr.
     */
    input::event_feed *input_feed() noexcept
    {
        return g_feed;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Cursor and frame callback
    // -----------------------------------------------------------------------------------------------------------------

    void set_cursor_mode(window_id id, cursor_mode mode) noexcept
    {
        window_state *ws = window_state_from_id(id);
        if (!ws)
            return;

        if (ws->cursor == mode)
            return;

        ws->cursor = mode;
        apply_cursor_mode(*ws);
    }

    cursor_mode get_cursor_mode(window_id id) noexcept
    {
        const window_state *ws = window_state_from_id(id);
        return ws ? ws->cursor : cursor_mode::normal;
    }

    void set_frame_callback(window_id id, frame_callback cb, void *user) noexcept
    {
        window_state *ws = window_state_from_id(id);
        if (!ws)
            return;

        ws->frame_cb = cb;
        ws->frame_user = user;
    }

    // -----------------------------------------------------------------------------------------------------------------
    // Window state
    // -----------------------------------------------------------------------------------------------------------------

    void set_title(window_id id, const char *utf8_title) noexcept
    {
        HWND hwnd = hwnd_from_id(id);
        if (!hwnd)
            return;

        const std::wstring titleW = win32::utf8_to_wide_or_ansi(utf8_title);
        SetWindowTextW(hwnd, titleW.c_str());
    }

    void set_client_size(window_id id, ui::length width_px, ui::length height_px) noexcept
    {
        HWND hwnd = hwnd_from_id(id);
        if (!hwnd)
            return;

        const float scale = dpi_scale_for_window(hwnd);
        RECT r{0, 0,
               static_cast<LONG>(resolve_px(width_px, ui::axis::x, scale)),
               static_cast<LONG>(resolve_px(height_px, ui::axis::y, scale))};
        adjust_rect_for_window(hwnd, r);

        SetWindowPos(hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                     SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    void set_position(window_id id, const math::vec2<std::int32_t> &position_px) noexcept
    {
        HWND hwnd = hwnd_from_id(id);
        if (!hwnd)
            return;

        // The caller gives the client origin, but SetWindowPos positions the frame. An empty client rect adjusted into a
        // window rect yields exactly the negative offset from the frame's origin to the client's.
        RECT offset{0, 0, 0, 0};
        adjust_rect_for_window(hwnd, offset);

        SetWindowPos(hwnd, nullptr,
                     position_px.x() + offset.left,
                     position_px.y() + offset.top,
                     0, 0,
                     SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    math::vec2<std::int32_t> position_px(window_id id) noexcept
    {
        HWND hwnd = hwnd_from_id(id);
        if (!hwnd)
            return {};

        POINT origin{0, 0};
        ClientToScreen(hwnd, &origin);
        return {static_cast<std::int32_t>(origin.x), static_cast<std::int32_t>(origin.y)};
    }

    void set_size_limits(window_id id, const math::vec2<std::int32_t> &min_px, const math::vec2<std::int32_t> &max_px) noexcept
    {
        window_state *ws = window_state_from_id(id);
        if (!ws)
            return;

        ws->min_size_px = {std::max(0, min_px.x()), std::max(0, min_px.y())};
        ws->max_size_px = {std::max(0, max_px.x()), std::max(0, max_px.y())};

        // Nudge the window so the new constraints are applied to its current size straight away rather than only on the
        // user's next drag.
        RECT r{};
        if (GetWindowRect(ws->hwnd, &r))
        {
            SetWindowPos(ws->hwnd, nullptr, 0, 0, r.right - r.left, r.bottom - r.top,
                         SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
        }
    }

    void show_window(window_id id) noexcept
    {
        if (HWND hwnd = hwnd_from_id(id))
            ShowWindow(hwnd, SW_SHOWNA);
    }

    void hide_window(window_id id) noexcept
    {
        if (HWND hwnd = hwnd_from_id(id))
            ShowWindow(hwnd, SW_HIDE);
    }

    void minimize_window(window_id id) noexcept
    {
        if (HWND hwnd = hwnd_from_id(id))
            ShowWindow(hwnd, SW_MINIMIZE);
    }

    void maximize_window(window_id id) noexcept
    {
        if (HWND hwnd = hwnd_from_id(id))
            ShowWindow(hwnd, SW_MAXIMIZE);
    }

    void restore_window(window_id id) noexcept
    {
        if (HWND hwnd = hwnd_from_id(id))
            ShowWindow(hwnd, SW_RESTORE);
    }

    void focus_window(window_id id) noexcept
    {
        HWND hwnd = hwnd_from_id(id);
        if (!hwnd)
            return;

        if (IsIconic(hwnd))
            ShowWindow(hwnd, SW_RESTORE);

        BringWindowToTop(hwnd);
        SetForegroundWindow(hwnd);
        SetFocus(hwnd);
    }

    void request_attention(window_id id) noexcept
    {
        HWND hwnd = hwnd_from_id(id);
        if (!hwnd || GetForegroundWindow() == hwnd)
            return;

        FLASHWINFO fi{};
        fi.cbSize = sizeof(fi);
        fi.hwnd = hwnd;
        fi.dwFlags = FLASHW_ALL | FLASHW_TIMERNOFG; // flash until the user brings the window forward
        fi.uCount = 0;
        fi.dwTimeout = 0;
        FlashWindowEx(&fi);
    }

    window_display_state display_state(window_id id) noexcept
    {
        const window_state *ws = window_state_from_id(id);
        return ws ? ws->display : window_display_state::restored;
    }

    void set_resizable(window_id id, bool resizable) noexcept
    {
        HWND hwnd = hwnd_from_id(id);
        if (!hwnd)
            return;

        auto style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
        const DWORD updated = resizable ? (style | k_resizable_style_bits) : (style & ~k_resizable_style_bits);
        if (updated == style)
            return;

        SetWindowLongPtrW(hwnd, GWL_STYLE, static_cast<LONG_PTR>(updated));
        // The frame changed shape, so the non-client area has to be recalculated and redrawn.
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }

    bool is_resizable(window_id id) noexcept
    {
        HWND hwnd = hwnd_from_id(id);
        if (!hwnd)
            return false;

        return (static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)) & WS_THICKFRAME) != 0;
    }

    void set_always_on_top(window_id id, bool on_top) noexcept
    {
        if (HWND hwnd = hwnd_from_id(id))
        {
            SetWindowPos(hwnd, on_top ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }
    }

    void set_opacity(window_id id, float opacity) noexcept
    {
        window_state *ws = window_state_from_id(id);
        if (!ws)
            return;

        HWND hwnd = ws->hwnd;
        const float clamped = std::clamp(opacity, 0.0f, 1.0f);
        auto ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));

        if (clamped >= 1.0f)
        {
            // Fully opaque again: drop the layered style if we were the ones who added it, so the window goes back to the
            // cheaper non-composited present path.
            if (ws->layered_by_us)
            {
                SetWindowLongPtrW(hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(ex_style & ~WS_EX_LAYERED));
                ws->layered_by_us = false;
            }
            else if ((ex_style & WS_EX_LAYERED) != 0)
            {
                SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);
            }
            return;
        }

        if ((ex_style & WS_EX_LAYERED) == 0)
        {
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(ex_style | WS_EX_LAYERED));
            ws->layered_by_us = true;
        }

        SetLayeredWindowAttributes(hwnd, 0, static_cast<BYTE>(clamped * 255.0f + 0.5f), LWA_ALPHA);
    }

    /**
     * @fn set_fullscreen
     * @brief Switches a window between borderless fullscreen on its current monitor and the rectangle and style it had
     * before.
     * @details The window's placement is saved rather than just its rectangle, so a window that was maximised before going
     * fullscreen comes back maximised rather than restored. The display mode is deliberately untouched: this composites
     * like any other window, which is why alt-tabbing out of it is instant and why nothing on the desktop is disturbed if
     * the application crashes while it is up.
     */
    void set_fullscreen(window_id id, bool fullscreen) noexcept
    {
        window_state *ws = window_state_from_id(id);
        if (!ws || ws->fullscreen == fullscreen)
            return;

        HWND hwnd = ws->hwnd;

        if (fullscreen)
        {
            ws->saved_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
            ws->saved_ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
            ws->saved_placement.length = sizeof(ws->saved_placement);
            GetWindowPlacement(hwnd, &ws->saved_placement);

            MONITORINFO mi{};
            mi.cbSize = sizeof(mi);
            if (!GetMonitorInfoW(MonitorFromWindow(hwnd, MONITOR_DEFAULTTONEAREST), &mi))
                return;

            const DWORD stripped_style =
                ws->saved_style & ~(WS_CAPTION | WS_THICKFRAME | WS_MINIMIZEBOX | WS_MAXIMIZEBOX | WS_SYSMENU);
            const DWORD stripped_ex =
                ws->saved_ex_style & ~(WS_EX_DLGMODALFRAME | WS_EX_WINDOWEDGE | WS_EX_CLIENTEDGE | WS_EX_STATICEDGE);

            SetWindowLongPtrW(hwnd, GWL_STYLE, static_cast<LONG_PTR>(stripped_style | WS_POPUP));
            SetWindowLongPtrW(hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(stripped_ex));

            SetWindowPos(hwnd, HWND_TOP,
                         mi.rcMonitor.left,
                         mi.rcMonitor.top,
                         mi.rcMonitor.right - mi.rcMonitor.left,
                         mi.rcMonitor.bottom - mi.rcMonitor.top,
                         SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

            ws->fullscreen = true;
            return;
        }

        SetWindowLongPtrW(hwnd, GWL_STYLE, static_cast<LONG_PTR>(ws->saved_style));
        SetWindowLongPtrW(hwnd, GWL_EXSTYLE, static_cast<LONG_PTR>(ws->saved_ex_style));
        SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);

        if (ws->saved_placement.length == sizeof(ws->saved_placement))
            SetWindowPlacement(hwnd, &ws->saved_placement);

        ws->fullscreen = false;
    }

    bool is_fullscreen(window_id id) noexcept
    {
        const window_state *ws = window_state_from_id(id);
        return ws && ws->fullscreen;
    }

    void set_dark_mode(window_id id, bool dark) noexcept
    {
        if (HWND hwnd = hwnd_from_id(id))
            set_dwm_bool_attribute(hwnd, k_dwmwa_use_immersive_dark_mode, k_dwmwa_use_immersive_dark_mode_legacy, dark);
    }

} // namespace catalyst::platform::detail
