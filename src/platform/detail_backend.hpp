/**
 * @file detail_backend.hpp
 * @brief Internal header for platform-specific backend implementations. This header declares the interface for the platform-specific implementations of window management and event handling. The actual implementations will be defined in separate source files corresponding to each supported platform (e.g. win32, x11, etc.). This header should not be included directly by users of the Catalyst Platform library; instead, users should include the main platform.hpp header, which provides a stable API for interacting with the platform functionalities.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/platform/window.hpp>
#include <catalyst/platform/monitor.hpp>

/**
 * @namespace catalyst::platform::detail
 * @brief The catalyst::platform::detail namespace contains internal implementation details for the Catalyst Platform library. This includes platform-specific backend implementations for window management and event handling. The functions declared in this namespace are intended to be used by the public API of the Catalyst Platform library and should not be called directly by users of the library. By organizing these implementation details within a nested detail namespace, we can maintain a clear separation between the public API and the internal workings of the library, allowing for easier maintenance and potential future changes to the implementation without affecting users of the library.
 * @details The catalyst::platform::detail namespace is an internal namespace that contains the platform-specific implementations for window management and event handling. This includes functions for creating and destroying windows, retrieving native handles, managing event sinks, and other platform-specific operations. The actual implementations of these functions will be defined in separate source files corresponding to each supported platform (e.g. win32, x11, etc.). Users of the Catalyst Platform library should not include this header directly or call these functions directly; instead, they should use the public API provided by the main platform.hpp header, which will internally call these detail functions as needed to perform the required operations on the underlying platform.
 */
namespace catalyst::platform::detail
{

	/**
	 * @fn backend_name
	 * @brief Returns the name of the platform backend being used. This function provides a way to identify which platform-specific implementation is currently active, such as "win32" for the Windows platform. This can be useful for debugging, logging, or conditional behavior based on the platform. The returned string is typically a compile-time constant that indicates the specific backend in use.
	 * @return A string representing the name of the platform backend being used. This can be used for debugging, logging, or conditional behavior based on the platform.
	 */
	const char *backend_name();

	/**
	 * @fn create_window
	 * @brief Creates a window with the specified description and returns its unique identifier. This function is responsible for creating a window on the underlying platform based on the provided window_desc structure, which contains parameters such as title, width, height, visibility, and resizability. The function returns a window_id that can be used to reference and manage the created window in subsequent operations.
	 * @param desc A structure containing the description of the window to be created, including parameters such as title, width, height, visibility, and resizability.
	 * @return A unique identifier (window_id) for the created window, which can be used to reference and manage the window in subsequent operations.
	 */
	window_id create_window(const window_desc &desc);
	/**
	 * @fn destroy_window
	 * @brief Destroys the window associated with the given window_id. This function is responsible for properly cleaning up and releasing any resources associated with the specified window. After calling this function, the window_id should no longer be considered valid, and any further operations on that window_id may result in undefined behavior.
	 * @param id The unique identifier (window_id) of the window to be destroyed. This identifier should have been obtained from a previous call to create_window.
	 */
	void destroy_window(window_id id) noexcept;
	/**
	 * @fn is_window_valid
	 * @brief Checks if the given window_id corresponds to a valid and existing window. This function can be used to verify whether a window with the specified window_id has been successfully created and has not been destroyed. It returns true if the window_id is valid and corresponds to an existing window, and false otherwise.
	 * @param id The unique identifier (window_id) of the window to check for validity. This identifier should have been obtained from a previous call to create_window.
	 * @return True if the window_id corresponds to a valid and existing window, false otherwise.
	 */
	bool is_window_valid(window_id id) noexcept;
	/**
	 * @fn get_native_handle
	 * @brief Retrieves the native handle associated with the specified window_id. This function returns a native_handle structure that contains information about the type of native handle and the actual handle value, which can be used for platform-specific operations or interfacing with native APIs. The returned native_handle may vary depending on the underlying platform and the type of window being managed.
	 * @param id The unique identifier (window_id) of the window for which to retrieve the native handle. This identifier should have been obtained from a previous call to create_window.
	 * @return A native_handle structure containing information about the type of native handle and the actual handle value associated with the specified window_id.
	 */
	native_handle get_native_handle(window_id id) noexcept;
	/**
	 * @fn client_rect_px
	 * @brief Retrieves the client area rectangle of the specified window in pixels. This function returns a rect_px that defines the dimensions of the client area of the window, which is the area available for rendering content. The dimensions are typically given in pixels and can be used for layout calculations, rendering, or other operations that require knowledge of the window's client area size.
	 * @param id The unique identifier (window_id) of the window for which to retrieve the client area rectangle. This identifier should have been obtained from a previous call to create_window.
	 * @return A rect_px representing the dimensions of the client area of the specified window in pixels.
	 */
	rect_px client_rect_px(window_id id) noexcept;
	/**
	 * @fn dpi_scale
	 * @brief Retrieves the DPI scaling factor for the specified window. This function returns a float value representing the scaling factor that should be applied to convert between logical units (e.g. device-independent pixels) and physical pixels for the given window. The DPI scaling factor can be used to ensure that UI elements and content are rendered at the appropriate size on high-DPI displays, providing a consistent user experience across different screen resolutions and densities.
	 * @param id The unique identifier (window_id) of the window for which to retrieve the DPI scaling factor. This identifier should have been obtained from a previous call to create_window.
	 * @return A float value representing the DPI scaling factor for the specified window, which can be used to convert between logical units and physical pixels.
	 */
	float dpi_scale(window_id id) noexcept;
	/**
	 * @fn pump_events
	 * @brief Pumps the event queue, processing any pending events and dispatching them to the appropriate handlers. This function should be called regularly (e.g. once per frame) to ensure that events are processed in a timely manner and that the application remains responsive to user input and other events. The implementation of this function will typically involve retrieving events from the underlying platform's event system, translating them into the library's event format, and then dispatching them to the installed event bus and input feed.
	 */
	void pump_events() noexcept;
	/**
	 * @fn wait_events
	 * @brief Waits for events to occur, with an optional timeout. This function blocks until at least one event is available in the event queue or until the specified timeout has elapsed. If events are available, they will be processed and dispatched to the appropriate handlers. The timeout parameter allows the function to return after a certain amount of time if no events have occurred, preventing indefinite blocking. This can be useful for applications that want to perform periodic updates even when no user input or other events are occurring.
	 * @param timeout_ms The maximum amount of time to wait for events, in milliseconds. If this parameter is zero, the function will block indefinitely until an event occurs. If this parameter is non-zero, the function will return after the specified amount of time if no events have occurred.
	 * @return True if events were processed, false if the timeout elapsed without any events occurring.
	 */
	bool wait_events(std::uint32_t timeout_ms) noexcept;
	/**
	 * @fn set_event_bus
	 * @brief Sets the bus that window and monitor events are dispatched to as the backend translates them, or nullptr for
	 * none. See platform::set_event_bus for the contract the backend is required to honour.
	 * @param bus The bus to dispatch to, or nullptr to dispatch nothing.
	 */
	void set_event_bus(events::bus *bus) noexcept;
	/** @brief The bus set with set_event_bus, or nullptr. */
	[[nodiscard]] events::bus *event_bus() noexcept;
	/**
	 * @fn set_input_feed
	 * @brief Sets the feed that window-sourced input is delivered to as the backend translates it, or nullptr for none. A
	 * backend must not track held keys or buttons of its own: see platform::set_input_feed.
	 * @param feed The feed to deliver input to, or nullptr to deliver nothing.
	 */
	void set_input_feed(input::event_feed *feed) noexcept;
	/** @brief The feed set with set_input_feed, or nullptr. */
	[[nodiscard]] input::event_feed *input_feed() noexcept;
	void set_cursor_mode(window_id id, cursor_mode mode) noexcept;
	[[nodiscard]] cursor_mode get_cursor_mode(window_id id) noexcept;

	/**
	 * @fn set_frame_callback
	 * @brief Installs the function the backend invokes when the window must be redrawn from inside the platform's own
	 * message pump (see platform::set_frame_callback and platform::frame_callback for the full contract).
	 */
	void set_frame_callback(window_id id, frame_callback cb, void *user) noexcept;

	// ----------------------
	// Window state
	// ----------------------

	void set_title(window_id id, const char *utf8_title) noexcept;
	void set_client_size(window_id id, ui::length width_px, ui::length height_px) noexcept;
	void set_position(window_id id, const math::vec2<std::int32_t> &position_px) noexcept;
	[[nodiscard]] math::vec2<std::int32_t> position_px(window_id id) noexcept;
	void set_size_limits(window_id id, const math::vec2<std::int32_t> &min_px, const math::vec2<std::int32_t> &max_px) noexcept;
	void show_window(window_id id) noexcept;
	void hide_window(window_id id) noexcept;
	void minimize_window(window_id id) noexcept;
	void maximize_window(window_id id) noexcept;
	void restore_window(window_id id) noexcept;
	void focus_window(window_id id) noexcept;
	void request_attention(window_id id) noexcept;
	[[nodiscard]] window_display_state display_state(window_id id) noexcept;
	void set_resizable(window_id id, bool resizable) noexcept;
	[[nodiscard]] bool is_resizable(window_id id) noexcept;
	void set_always_on_top(window_id id, bool on_top) noexcept;
	void set_opacity(window_id id, float opacity) noexcept;
	void set_fullscreen(window_id id, bool fullscreen) noexcept;
	[[nodiscard]] bool is_fullscreen(window_id id) noexcept;
	void set_dark_mode(window_id id, bool dark) noexcept;

	// ----------------------
	// Monitor backend API
	// ----------------------

	[[nodiscard]] std::size_t get_monitor_count() noexcept;
	[[nodiscard]] std::vector<monitor_desc> get_monitor_list() noexcept;
	[[nodiscard]] monitor_desc get_monitor(monitor_id id) noexcept;
	[[nodiscard]] monitor_id primary_monitor() noexcept;
	[[nodiscard]] monitor_id monitor_for_window(window_id w) noexcept;

} // namespace catalyst::platform::detail
