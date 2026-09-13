/**
 * @file window.cpp
 * @brief Implementation of the window management functions for the Catalyst Platform library. This file contains the definitions of the functions declared in window.hpp, which provide an interface for creating, destroying, and managing windows in a platform-agnostic way. The actual implementation of these functions is delegated to platform-specific backends, which are defined in the detail_backend.hpp header and implemented in separate source files for each supported platform (e.g., Win32, X11, etc.). By using this approach, we can maintain a clean separation between the public API of the Catalyst Platform library and the underlying platform-specific details, allowing users to interact with windows without needing to worry about the complexities of different operating systems.
 * @details The Catalyst Platform library provides a collection of platform utilities and types commonly used in game development and real-time applications. By including the appropriate headers from the Catalyst Platform library, users can access all the functionality they need for their projects, such as creating windows, handling events, and interacting with the underlying operating system in a platform-agnostic way. This allows developers to focus on building their applications without worrying about the complexities of platform-specific code, while still having access to powerful tools for managing windows and events effectively.
 * License: MIT (see LICENSE).
 */

#include <catalyst/platform/window.hpp>

#include <catalyst/events/bus.hpp>
#include <catalyst/input/feed.hpp>

/**
 * @include "detail_backend.hpp"
 * @namespace catalyst::platform::detail
 * @brief This header is included to access the platform-specific backend implementations for window management. The functions declared in detail_backend.hpp provide the actual implementations for creating, destroying, and managing windows on the underlying platform. By including this header, we can call these platform-specific functions from the public API functions defined in this source file, allowing us to maintain a clean separation between the public interface and the internal implementation details of the Catalyst Platform library. Users of the library should not include detail_backend.hpp directly or call its functions directly; instead, they should use the public API provided by window.hpp, which will internally call the appropriate backend functions as needed.
 * @details The detail_backend.hpp header contains the platform-specific implementations for window management and event handling. This includes functions for creating and destroying windows, retrieving native handles, managing event sinks, and other platform-specific operations. By including this header in the window.cpp source file, we can implement the public API functions declared in window.hpp by calling the appropriate backend functions defined in detail_backend.hpp. This allows us to provide a consistent and platform-agnostic interface for users of the Catalyst Platform library while still leveraging the specific capabilities of each supported platform through the backend implementations.
 * License: MIT (see LICENSE).
 */
#include "detail_backend.hpp"

/**
 * @namespace catalyst::platform
 * @brief The catalyst::platform namespace contains all the platform-specific types and functions provided by the Catalyst Platform library. This includes window management, event handling, and other platform-related utilities. By organizing all platform-specific functionality within this namespace, we can avoid naming conflicts and provide a clear structure for users of the library to access the various platform tools they need for their applications. The Catalyst Platform library is designed to be efficient, easy to use, and compatible with modern C++ standards, making it a valuable resource for developers building real-time applications and games.
 * @details The Catalyst Platform library provides a collection of platform utilities and types commonly used in game development and real-time applications. By including the appropriate headers from the Catalyst Platform library, users can access all the functionality they need for their projects, such as creating windows, handling events, and interacting with the underlying operating system in a platform-agnostic way. This allows developers to focus on building their applications without worrying about the complexities of platform-specific code, while still having access to powerful tools for managing windows and events effectively.
 */
namespace catalyst::platform
{

    /**
     * @fn create_window
     * @brief Creates a new window based on the provided window description. This function takes a window_desc structure as input, which contains various parameters for configuring the window (e.g., title, size, visibility). The function returns a window object that represents the created window and can be used for further operations such as querying its properties or managing its lifecycle.
     * @param desc A structure containing the parameters for configuring the window to be created. This includes properties such as the title, initial size, visibility, and whether the window should be resizable. By providing a window_desc structure, users can easily specify the desired characteristics of the window they want to create.
     * @return A window object representing the created window. This object can be used for further operations such as querying its properties or managing its lifecycle (e.g., destroying it when no longer needed).
     */
    window create_window(const window_desc &desc)
    {
        return window{detail::create_window(desc)};
    }

    /**
     * @fn destroy_window
     * @brief Destroys the specified window and releases any associated resources. This function takes a reference to a window object and destroys the underlying window represented by that object. After calling this function, the window object will be reset to an empty state (i.e., it will no longer represent a valid window). It is important to call this function when a window is no longer needed to ensure that system resources are properly released.
     * @param w A reference to the window object representing the window to be destroyed. After calling this function, the window object will be reset to an empty state and will no longer represent a valid window.
     */
    void destroy_window(window &w) noexcept
    {
        if (!w)
            return;

        detail::destroy_window(w.id());
        w = window{};
    }

    /**
     * @fn is_valid
     * @brief Checks whether the specified window is valid and can be used for operations. This function takes a reference to a window object and returns true if the window is valid (i.e., it represents an existing window that has not been destroyed), or false if the window is invalid (e.g., it has been destroyed or was never created). This function can be used to verify that a window object is in a usable state before performing operations on it.
     * @param w A reference to the window object to be checked for validity. The function will return true if the window is valid and can be used for operations, or false if the window is invalid (e.g., it has been destroyed or was never created).
     * @return A boolean value indicating whether the specified window is valid and can be used for operations. True if the window is valid, false otherwise.
     */
    bool is_valid(const window &w) noexcept
    {
        if (!w)
            return false;
        return detail::is_window_valid(w.id());
    }

    /**
     * @fn get_native_handle
     * @brief Retrieves the native handle associated with the specified window. This function takes a reference to a window object and returns a native_handle structure that contains information about the type of native handle and the actual handle value. The native handle can be used for platform-specific operations or interfacing with native APIs that require access to the underlying windowing system. If the provided window is invalid, the function will return an empty native_handle structure.
     * @param w A reference to the window object for which to retrieve the native handle. If the window is invalid, the function will return an empty native_handle structure.
     * @return A native_handle structure containing information about the type of native handle and the actual handle value associated with the specified window. If the provided window is invalid, an empty native_handle structure will be returned.
     */
    native_handle get_native_handle(const window &w) noexcept
    {
        if (!w)
            return {};
        return detail::get_native_handle(w.id());
    }

    /**
     * @fn client_rect_px
     * @brief Retrieves the client area rectangle of the specified window in pixels. This function takes a reference to a window object and returns a rect_px representing the dimensions of the client area of the window in pixel coordinates. The client area is the portion of the window where content can be rendered, excluding any borders, title bars, or other non-client elements. If the provided window is invalid, the function will return an empty rectangle with zero dimensions.
     * @param w A reference to the window object for which to retrieve the client area rectangle. If the window is invalid, the function will return an empty rectangle with zero dimensions.
     * @return A rect_px representing the dimensions of the client area of the specified window in pixel coordinates. If the provided window is invalid, an empty rectangle with zero dimensions will be returned.
     */
    rect_px client_rect_px(const window &w) noexcept
    {
        if (!w)
            return {};
        return detail::client_rect_px(w.id());
    }

    /**
     * @fn dpi_scale
     * @brief Retrieves the DPI scaling factor for the specified window. This function takes a reference to a window object and returns a floating-point value representing the DPI scaling factor for that window. The DPI scaling factor can be used to adjust rendering and layout calculations to account for different display densities, ensuring that content appears at an appropriate size on high-DPI displays. If the provided window is invalid, the function will return a default DPI scaling factor of 1.0f.
     * @param w A reference to the window object for which to retrieve the DPI scaling factor. If the window is invalid, the function will return a default DPI scaling factor of 1.0f.
     * @return A floating-point value representing the DPI scaling factor for the specified window. If the provided window is invalid, a default DPI scaling factor of 1.0f will be returned.
     */
    float dpi_scale(const window &w) noexcept
    {
        if (!w)
            return 1.0f;
        return detail::dpi_scale(w.id());
    }
    /**
     * @fn resolve_context_for_window
     * @brief Creates a ui::resolve_context populated with DPI information for the given window.
     * @details This helper is intended to make physical unit resolution (in/cm/mm) correct on
     * systems that do not map to 96 px/in. The returned context sets both legacy `dpi_scale` and
     * the newer per-axis `dpi_x`/`dpi_y` fields.
     * @param w A reference to the window object for which to create the resolve context. If the window is invalid, the returned context will have default DPI values.
     * @return A ui::resolve_context structure populated with DPI information for the specified window. If the provided window is invalid, the returned context will have default DPI values.
     */
    ui::resolve_context resolve_context_for_window(const window &w) noexcept
    {
        ui::resolve_context ctx{};

        const float scale = dpi_scale(w);
        ctx.dpi_scale = scale;

        // Populate per-axis effective DPI for physical unit resolution.
        // Win32 DPI is defined relative to 96 DPI.
        ctx.dpi_x = scale * 96.0f;
        ctx.dpi_y = scale * 96.0f;

        return ctx;
    }

    /**
     * @fn pump_events
     * @brief Pumps the event queue, processing any pending events for all windows. This function should be called regularly (e.g., once per frame) to ensure that events are processed and dispatched to the appropriate handlers. Pumping events allows the application to respond to user input, window messages, and other events generated by the operating system or the Catalyst Platform library. It is important to call this function in a timely manner to maintain responsiveness and ensure that events are handled correctly.
     */
    void pump_events() noexcept
    {
        detail::pump_events();
    }

    /**
     * @fn wait_events
     * @brief Waits for events to occur, with an optional timeout. This function blocks until at least one event is available in the event queue or until the specified timeout has elapsed. If events are available, they will be processed and dispatched to the appropriate handlers. The timeout parameter allows the function to return after a certain amount of time if no events have occurred, preventing indefinite blocking. This can be useful for applications that want to perform periodic updates even when no user input or other events are occurring.
     * @param timeout_ms The maximum amount of time (in milliseconds) to wait for events before returning. If set to 0, the function will block indefinitely until an event is available. If set to a positive value, the function will return after the specified time if no events have occurred.
     * @return A boolean value indicating whether any events were processed during the wait period. True if events were processed, false if the timeout elapsed without any events occurring.
     */
    bool wait_events(std::uint32_t timeout_ms) noexcept
    {
        return detail::wait_events(timeout_ms);
    }

    /**
     * @fn set_event_bus
     * @brief Installs the bus that window and monitor events are dispatched to. See platform::set_event_bus.
     * @param bus The bus to dispatch window events to, or nullptr for none.
     */
    void set_event_bus(events::bus *bus) noexcept
    {
        detail::set_event_bus(bus);
    }

    /**
     * @fn event_bus
     * @brief Returns the bus installed with set_event_bus, or nullptr if none is installed.
     */
    events::bus *event_bus() noexcept
    {
        return detail::event_bus();
    }

    /**
     * @fn set_input_feed
     * @brief Installs the feed that window-sourced input is delivered to. See platform::set_input_feed.
     * @param feed The feed to deliver input to, or nullptr for none.
     */
    void set_input_feed(input::event_feed *feed) noexcept
    {
        detail::set_input_feed(feed);
    }

    /**
     * @fn input_feed
     * @brief Returns the feed installed with set_input_feed, or nullptr if none is installed.
     */
    input::event_feed *input_feed() noexcept
    {
        return detail::input_feed();
    }

    void set_cursor_mode(const window &w, cursor_mode mode) noexcept
    {
        if (!w)
            return;
        detail::set_cursor_mode(w.id(), mode);
    }

    cursor_mode get_cursor_mode(const window &w) noexcept
    {
        if (!w)
            return cursor_mode::normal;
        return detail::get_cursor_mode(w.id());
    }

    void set_frame_callback(const window &w, frame_callback cb, void *user) noexcept
    {
        if (!w)
            return;
        detail::set_frame_callback(w.id(), cb, user);
    }

    void set_title(const window &w, const char *utf8_title) noexcept
    {
        if (!w)
            return;
        detail::set_title(w.id(), utf8_title);
    }

    void set_client_size(const window &w, ui::length width_px, ui::length height_px) noexcept
    {
        if (!w)
            return;
        detail::set_client_size(w.id(), width_px, height_px);
    }

    void set_position(const window &w, const math::vec2<std::int32_t> &position_px) noexcept
    {
        if (!w)
            return;
        detail::set_position(w.id(), position_px);
    }

    math::vec2<std::int32_t> position_px(const window &w) noexcept
    {
        if (!w)
            return {};
        return detail::position_px(w.id());
    }

    void set_size_limits(const window &w, const math::vec2<std::int32_t> &min_px, const math::vec2<std::int32_t> &max_px) noexcept
    {
        if (!w)
            return;
        detail::set_size_limits(w.id(), min_px, max_px);
    }

    void show(const window &w) noexcept
    {
        if (!w)
            return;
        detail::show_window(w.id());
    }

    void hide(const window &w) noexcept
    {
        if (!w)
            return;
        detail::hide_window(w.id());
    }

    void minimize(const window &w) noexcept
    {
        if (!w)
            return;
        detail::minimize_window(w.id());
    }

    void maximize(const window &w) noexcept
    {
        if (!w)
            return;
        detail::maximize_window(w.id());
    }

    void restore(const window &w) noexcept
    {
        if (!w)
            return;
        detail::restore_window(w.id());
    }

    void focus(const window &w) noexcept
    {
        if (!w)
            return;
        detail::focus_window(w.id());
    }

    void request_attention(const window &w) noexcept
    {
        if (!w)
            return;
        detail::request_attention(w.id());
    }

    window_display_state display_state(const window &w) noexcept
    {
        if (!w)
            return window_display_state::restored;
        return detail::display_state(w.id());
    }

    void set_resizable(const window &w, bool resizable) noexcept
    {
        if (!w)
            return;
        detail::set_resizable(w.id(), resizable);
    }

    bool is_resizable(const window &w) noexcept
    {
        if (!w)
            return false;
        return detail::is_resizable(w.id());
    }

    void set_always_on_top(const window &w, bool on_top) noexcept
    {
        if (!w)
            return;
        detail::set_always_on_top(w.id(), on_top);
    }

    void set_opacity(const window &w, float opacity) noexcept
    {
        if (!w)
            return;
        detail::set_opacity(w.id(), opacity);
    }

    void set_fullscreen(const window &w, bool fullscreen) noexcept
    {
        if (!w)
            return;
        detail::set_fullscreen(w.id(), fullscreen);
    }

    bool is_fullscreen(const window &w) noexcept
    {
        if (!w)
            return false;
        return detail::is_fullscreen(w.id());
    }

    void set_dark_mode(const window &w, bool dark) noexcept
    {
        if (!w)
            return;
        detail::set_dark_mode(w.id(), dark);
    }

} // namespace catalyst::platform
