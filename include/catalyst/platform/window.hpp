/*
 * @file window.hpp
 * @brief Window management and event handling for the Catalyst platform library.
 * @details This header defines the window class and related functions for creating and managing windows, as well as handling events such as input and window messages. It provides a platform-agnostic interface for working with windows, allowing applications to create windows, retrieve native handles, and process events in a consistent manner across different operating systems. The event system includes various types of events such as window resizing, key presses, mouse movement, and more, enabling developers to build interactive applications with ease.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/input/input.hpp>
#include <catalyst/math/vector.hpp>
#include <catalyst/platform/platform.hpp>
#include <catalyst/ui/measurement.hpp>

#include <cstddef>
#include <cstdint>

using namespace catalyst::ui::literals;

/**
 * @namespace catalyst::platform
 * @brief The catalyst::platform namespace contains all the platform-specific types and functions provided by the Catalyst Platform library. This includes window management, event handling, and other platform-related utilities. By organizing all platform-specific functionality within this namespace, we can avoid naming conflicts and provide a clear structure for users of the library to access the various platform tools they need for their applications. The Catalyst Platform library is designed to be efficient, easy to use, and compatible with modern C++ standards, making it a valuable resource for developers building real-time applications and games.
 * @details The Catalyst Platform library provides a collection of platform utilities and types commonly used in game development and real-time applications. By including the appropriate headers from the Catalyst Platform library, users can access all the functionality they need for their projects, such as creating windows, handling events, and interacting with the underlying operating system in a platform-agnostic way. This allows developers to focus on building their applications without worrying about the complexities of platform-specific code, while still having access to powerful tools for managing windows and events effectively.
 */
namespace catalyst::events
{
    class bus; // Forward declared so this header does not pull the event bus into every consumer of a window.
}

namespace catalyst::input
{
    class event_feed; // Forward declared: the platform pushes window-sourced input through this, nothing more.
}

/*
 * @namespace catalyst::platform
 * @brief The catalyst::platform namespace contains all the platform-specific types and functions provided by the Catalyst Platform library. This includes window management, event handling, and other platform-related utilities. By organizing all platform-specific functionality within this namespace, we can avoid naming conflicts and provide a clear structure for users of the library to access the various platform tools they need for their applications. The Catalyst Platform library is designed to be efficient, easy to use, and compatible with modern C++ standards, making it a valuable resource for developers building real-time applications and games.
 * @details The Catalyst Platform library provides a collection of platform utilities and types commonly used in game development and real-time applications. By including the appropriate headers from the Catalyst Platform library, users can access all the functionality they need for their projects, such as creating windows, handling events, and interacting with the underlying operating system in a platform-agnostic way. This allows developers to focus on building their applications without worrying about the complexities of platform-specific code, while still having access to powerful tools for managing windows and events effectively.
 */
namespace catalyst::platform
{
    /**
     * @typedef window_id
     * @brief A unique identifier for a window. This type is used to represent and manage individual windows within the Catalyst Platform library. Each window created by the library is assigned a unique window_id, which can be used to reference and manipulate that specific window throughout its lifecycle. The window_id is typically implemented as a 64-bit unsigned integer, allowing for a large number of windows to be managed simultaneously without conflicts.
     * @details The window_id type serves as a fundamental building block for the Catalyst Platform library's window management system. By using unique identifiers for each window, the library can efficiently track and manage multiple windows, allowing developers to create complex applications with multiple views or interfaces. The window_id can be used in various functions and methods within the library to perform operations on specific windows, such as resizing, retrieving native handles, or processing events related to that window.
     */
    using window_id = std::uint64_t;

    /**
     * @enum native_handle_kind
     * @brief An enumeration representing the type of native handle associated with a window. This enum is used to indicate the specific platform or type of native handle that a window may have, such as a Win32 HWND on Windows. The native_handle_kind allows the Catalyst Platform library to provide a consistent interface for retrieving native handles while still supporting multiple platforms and handle types. By using this enumeration, developers can easily determine the type of native handle they are working with and use it accordingly in their applications.
     * @details The native_handle_kind enumeration is an important part of the Catalyst Platform library's abstraction layer for window management. It allows the library to support various platforms and their respective native handle types without exposing platform-specific details to the user. When retrieving a native handle for a window, the library can return the appropriate handle type based on the platform and indicate it using the native_handle_kind enum, enabling developers to write cross-platform code that can interact with native windowing systems when necessary.
     */
    enum class native_handle_kind : std::uint8_t
    {
        none,
        win32_hwnd,
    };

    /**
     * @struct native_handle
     * @brief A structure representing a native handle associated with a window. This structure contains information about the type of native handle (using the native_handle_kind enumeration) and the actual handle value, which is typically a pointer or platform-specific identifier. The native_handle struct allows the Catalyst Platform library to provide a consistent way to retrieve and work with native handles across different platforms, while still encapsulating platform-specific details within the structure.
     * @details The native_handle struct is used in conjunction with functions that retrieve native handles for windows in the Catalyst Platform library. By providing both the kind of handle and the handle value, developers can easily determine how to use the native handle in their applications, whether it's for interfacing with platform-specific APIs or for other purposes. The structure also allows for future expansion to support additional types of native handles as needed.
     */
    struct native_handle
    {
        /**
         * @var kind
         * @brief An enumeration value indicating the type of native handle contained in this structure. This
         */
        native_handle_kind kind = native_handle_kind::none;
        /**
         * @var handle
         * @brief The actual native handle value, which is typically a pointer or platform-specific identifier. The meaning and usage of this value depend on the kind of native handle specified in the 'kind' member. For example, if the kind is win32_hwnd, this handle would be a pointer to a Win32 HWND structure representing a window on the Windows platform.
         */
        void *handle = nullptr;
        /**
         * @var extra
         * @brief An optional pointer for additional platform-specific data or context related to the native handle. This member can be used to store any extra information that may be needed when working with the native handle, such as additional handles, context pointers, or other relevant data that is specific to the platform or handle type. The use of this member is optional and depends on the requirements of the application and the platform being targeted.
         */
        void *extra = nullptr;
    };

    /**
     * @struct window_desc
     * @brief A structure representing the description of a window to be created. This structure contains various parameters that define the properties and behavior of a window, such as its title, dimensions, visibility, and resizability. The window_desc struct is used when creating a new window using the Catalyst Platform library, allowing developers to specify the desired characteristics of the window they want to create.
     * @details The window_desc struct provides a convenient way to encapsulate all the necessary information for creating a window in a single structure. By filling out the fields of this struct, developers can easily configure the appearance and behavior of their windows without needing to call multiple functions or set properties individually. This promotes cleaner code and makes it easier to manage window creation in applications that may require multiple windows with different configurations.
     */
    struct window_desc
    {
        /**
         * @var title
         * @brief The title of the window, which is typically displayed in the title bar of the window. This member is a C-style string (const char*) that specifies the text to be shown as the window's title. The title can be used to provide context or information about the window's purpose or content to the user.
         */
        const char *title = "Catalyst";
        /**
         * @var width_px
         * @brief The initial width of the window in pixels. This member specifies the desired width of the window when it is created. The actual width of the window may be adjusted by the operating system or window manager based on various factors such as screen resolution, available space, or user preferences.
         */
        ui::length width_px = 1280.0_px;
        /**
         * @var height_px
         * @brief The initial height of the window in pixels. This member specifies the desired height of the window when it is created. Similar to the width, the actual height of the window may be adjusted by the operating system or window manager based on various factors such as screen resolution, available space, or user preferences.
         */
        ui::length height_px = 720.0_px;
        /**
         * @var visible
         * @brief A boolean value indicating whether the window should be initially visible when created. If set to true, the window will be shown on the screen immediately after creation. If set to false, the window will be created in a hidden state and can be shown later using appropriate functions provided by the Catalyst Platform library.
         */
        bool visible = true;
        /**
         * @var resizable
         * @brief A boolean value indicating whether the window should be resizable by the user. If set to true, the window will allow the user to resize it by dragging the edges or corners. If set to false, the window will have a fixed size and cannot be resized by the user. This property can be useful for applications that require a specific window size or for creating modal dialogs that should not be resized.
         */
        bool resizable = true;
    };

    // -----------------------------------------------------------------------------
    // Window events
    // -----------------------------------------------------------------------------
    //
    // Plain structs: the event bus keys on the static type, so an event needs no base class and no runtime type id. They
    // are dispatched to the bus installed with set_event_bus(). Input events are *not* here - those go to the
    // input::event_feed installed with set_input_feed(), and reach the bus from the input module's registry.

    /**
     * @struct window_close_requested_event
     * @brief An event that is published when a window close request is made. This event is typically generated when the user attempts to close a window, such as by clicking the close button on the title bar or using a keyboard shortcut. The event contains a window_id member that identifies which window is being requested to close. Handlers for this event can choose to allow the window to close by not marking the event as handled, or they can prevent the window from closing by marking the event as handled, allowing for custom behavior such as prompting the user to save changes before closing.
     * @details The window_close_requested_event is an important part of the Catalyst Platform library's event system, as it allows developers to respond to user-initiated close requests in a flexible manner. By subscribing to this event, developers can implement custom logic to determine whether a window should be allowed to close, such as checking for unsaved data or confirming the user's intent. This enhances the user experience by providing a way to prevent accidental closures and ensuring that important data is not lost without warning.
     */
    struct window_close_requested_event
    {
        window_id window = 0;
    };

    /**
     * @struct window_destroyed_event
     * @brief An event that is published when a window has been destroyed. This event is typically generated after a window has been closed and all associated resources have been released. The event contains a window_id member that identifies which window was destroyed. Handlers for this event can perform any necessary cleanup or updates in response to the destruction of the window, such as removing references to the window or updating the user interface to reflect the change.
     * @details The window_destroyed_event is an important part of the Catalyst Platform library's event system, as it allows developers to respond to the destruction of windows in a timely manner. By subscribing to this event, developers can ensure that their applications remain stable and responsive by properly handling the cleanup of resources and updating the application state when windows are destroyed.
     */
    struct window_destroyed_event
    {
        window_id window = 0;
    };

    /**
     * @struct window_resized_event
     * @brief An event that is published when a window has been resized. This event is typically generated when the user resizes a window by dragging its edges or corners, or when the window is programmatically resized through the Catalyst Platform library. The event contains a window_id member that identifies which window was resized, as well as width_px and height_px members that specify the new dimensions of the window in pixels. Handlers for this event can use this information to adjust the layout of the user interface, resize rendering targets, or perform other necessary updates in response to the change in window size.
     * @details The window_resized_event is an important part of the Catalyst Platform library's event system, as it allows developers to respond to changes in window size in a timely manner. By subscribing to this event, developers can ensure that their applications remain visually consistent and functional when windows are resized, providing a better user experience across different screen sizes and resolutions.
     */
    struct window_resized_event
    {
        window_id window = 0;
        ui::length width_px = 0.0_px;
        ui::length height_px = 0.0_px;
    };

    /**
     * @struct window_enter_size_move_event
     * @brief An event that is published when a window enters a size or move operation. This event is typically generated when the user begins resizing or moving a window, such as by clicking and dragging the edges or title bar. The event contains a window_id member that identifies which window is entering the size or move operation. Handlers for this event can use this information to prepare for potential changes in the window's position or size, such as pausing certain updates or adjusting the user interface to accommodate the ongoing operation.
     * @details The window_enter_size_move_event is an important part of the Catalyst Platform library's event system, as it allows developers to respond to user-initiated size and move operations in a flexible manner. By subscribing to this event, developers can implement custom logic to enhance the user experience during these operations, such as providing visual feedback or temporarily adjusting application behavior while the user is resizing or moving a window.
     */
    struct window_enter_size_move_event
    {
        window_id window = 0;
    };

    /**
     * @struct window_exit_size_move_event
     * @brief An event that is published when a window exits a size or move operation. This event is typically generated when the user finishes resizing or moving a window, such as by releasing the mouse button after dragging the edges or title bar. The event contains a window_id member that identifies which window is exiting the size or move operation. Handlers for this event can use this information to finalize any updates or adjustments that were made during the size or move operation, such as resuming paused updates or applying final layout changes based on the new position or size of the window.
     * @details The window_exit_size_move_event is an important part of the Catalyst Platform library's event system, as it allows developers to respond to the completion of user-initiated size and move operations in a timely manner. By subscribing to this event, developers can ensure that their applications remain responsive and visually consistent after these operations, providing a better user experience across different screen sizes and resolutions.
     */
    struct window_exit_size_move_event
    {
        window_id window = 0;
    };

    /**
     * @struct window_dpi_changed_event
     * @brief An event that is published when the DPI scaling factor for a window has changed. This event is typically generated when a window is moved to a different display with a different DPI scaling factor, or when the system's DPI settings are changed while the application is running. The event contains a window_id member that identifies which window's DPI scaling factor has changed, as well as a dpi_scale member that specifies the new DPI scaling factor for the window. Handlers for this event can use this information to adjust rendering and layout calculations to account for the new display density, ensuring that content appears at an appropriate size on high-DPI displays.
     * @details The window_dpi_changed_event is an important part of the Catalyst Platform library's event system, as it allows developers to respond to changes in DPI scaling in a timely manner. By subscribing to this event, developers can ensure that their applications remain visually consistent and functional across different display densities, providing a better user experience on high-DPI displays.
     */
    struct window_dpi_changed_event
    {
        window_id window = 0;
        float dpi_scale = 1.0f;
    };

    /**
     * @struct window_focus_event
     * @brief Published when a window gains or loses keyboard focus. On focus loss the backend also publishes a release for
     * every key and mouse button it still considers held, so consumers never see stuck input, and any cursor capture is
     * suspended until focus returns.
     */
    struct window_focus_event
    {
        window_id window = 0;
        bool focused = false;
    };

    /**
     * @struct window_moved_event
     * @brief Published when the window's client area changes position on the virtual desktop. The position is the top-left
     * corner of the *client* area in screen pixels, which is what an application needs to map between client and screen
     * space; it is not the top-left of the window frame.
     */
    struct window_moved_event
    {
        window_id window = 0;
        math::vec2<std::int32_t> position_px{};
    };

    /**
     * @enum window_display_state
     * @brief Whether a window is in its normal restored state, iconified, or zoomed to fill the work area.
     */
    enum class window_display_state : std::uint8_t
    {
        /** @brief The window occupies its normal, user-set rectangle. */
        restored,
        /** @brief The window is iconified. Its client area is 0x0 and it should not be rendered to. */
        minimized,
        /** @brief The window is zoomed to fill the monitor's work area. */
        maximized
    };

    /**
     * @struct window_display_state_event
     * @brief Published when a window is minimised, maximised or restored. Renderers should use this rather than inferring
     * the state from a 0x0 window_resized_event: a minimised window still receives resize events, and presenting to a
     * zero-sized surface is an error on every graphics API.
     */
    struct window_display_state_event
    {
        window_id window = 0;
        window_display_state state = window_display_state::restored;
    };

    /**
     * @typedef frame_callback
     * @brief A function the backend calls when the window needs a frame drawn *right now*, from inside pump_events() or
     * wait_events().
     * @details Resizing and moving a window runs a modal loop inside the operating system's own message pump, which does not
     * return control to the application until the user releases the mouse. Events published during that loop therefore pile
     * up unrendered and the window's contents freeze. Installing a frame callback with set_frame_callback gives the backend
     * a way to ask the application to render mid-loop, which is what keeps the window live while it is being dragged.
     *
     * The callback is invoked re-entrantly from within pump_events()/wait_events(), possibly many times per call and
     * possibly while a window/input event handler higher up the stack is still running. It must therefore be safe to call
     * at any point, must not create or destroy windows, and must not itself call pump_events(). The backend guards against
     * direct recursion (a callback that triggers another frame request will not re-enter), but nothing else.
     * @param w The window that needs to be drawn.
     * @param user The opaque pointer that was passed to set_frame_callback.
     */
    using frame_callback = void (*)(const class window &w, void *user);

    /**
     * @enum cursor_mode
     * @brief How the mouse cursor behaves over a window's client area. See set_cursor_mode.
     */
    enum class cursor_mode : std::uint8_t
    {
        /** @brief The cursor is visible and moves freely. */
        normal,
        /** @brief The cursor is hidden while over the client area but still moves freely (for a custom software cursor). */
        hidden,
        /**
         * @brief The cursor is hidden and confined to the client area while the window has focus, and unaccelerated motion
         * is delivered as input::mouse_raw_move_event. Use this for first-person camera control.
         */
        captured
    };

    /**
     * @class window
     * @brief A class representing a window in the Catalyst Platform library. This class provides a platform-agnostic interface for managing windows, allowing developers to create, destroy, and interact with windows in a consistent manner across different operating systems. The window class encapsulates a unique window_id that identifies the window within the library, and it provides member functions for retrieving the window's ID and checking its validity. By using the window class, developers can easily manage multiple windows in their applications while maintaining a clean and organized codebase.
     * @details The window class is a fundamental part of the Catalyst Platform library's window management system. It provides a simple and efficient way to represent windows and perform operations on them without exposing platform-specific details. The class includes an explicit constructor that takes a window_id, allowing for the creation of window instances that reference specific windows managed by the library. Additionally, the class provides an operator bool() to allow for easy validity checks, enabling developers to write clean and intuitive code when working with windows in their applications.
     */
    class window
    {
    public:
        /**
         * @fn window
         * @brief Default constructor for the window class. This constructor initializes a window instance with a default state, where the window_id is set to 0, indicating that it does not reference a valid window. This allows for the creation of window instances that can be assigned valid window_ids later on, such as when a new window is created using the Catalyst Platform library. The default constructor is noexcept, ensuring that it does not throw exceptions and can be safely used in various contexts without the need for exception handling.
         */
        window() noexcept = default;
        /**
         * @fn window
         * @brief Explicit constructor for the window class that takes a window_id. This constructor allows for the creation of a window instance that references a specific window managed by the Catalyst Platform library. By providing a valid window_id, developers can create window instances that are associated with existing windows, enabling them to perform operations on those windows using the member functions provided by the class. The constructor is explicit to prevent unintended conversions from window_id to window, ensuring that developers must intentionally create window instances with specific IDs.
         * @param id The unique identifier for the window that this instance will reference. This value should be obtained from the Catalyst Platform library when creating or managing windows, and it allows the instance to be associated with a specific window within the library's management system.
         */
        explicit window(window_id id) noexcept : id_(id) {}

        /**
         * @fn id
         * @brief Retrieves the unique identifier for this window. This member function returns the window_id associated with this window instance, which can be used to reference and manipulate the specific window within the Catalyst Platform library. The window_id is a unique identifier that allows the library to manage multiple windows simultaneously without conflicts. By calling this function, developers can obtain the window_id for a given window instance and use it in various functions and methods provided by the library to perform operations on that specific window.
         * @return The window_id associated with this window instance. This value can be used to reference and manipulate the specific window within the Catalyst Platform library. If the window instance is invalid (i.e., it does not reference a valid window), this function will return a window_id value of 0.
         */
        [[nodiscard]] window_id id() const noexcept { return id_; }
        /**
         * @fn operator bool
         * @brief Checks whether this window instance is valid. This operator allows for easy validity checks of a window instance by returning true if the instance references a valid window (i.e., its window_id is not 0) and false otherwise. This can be used in conditional statements to ensure that operations are only performed on valid windows, preventing errors and improving the robustness of the application.
         * @return True if this window instance is valid (i.e., it references a valid window with a non-zero window_id), false otherwise.
         */
        [[nodiscard]] explicit operator bool() const noexcept { return id_ != 0; }

    private:
        /**
         * @var id_
         * @brief The unique identifier for the window that this instance references. This member variable stores the window_id associated with this window instance, allowing it to reference a specific window managed by the Catalyst Platform library. The window_id is a unique identifier that enables the library to manage multiple windows simultaneously without conflicts. When a new window is created using the library, it is assigned a unique window_id, which can then be stored in this member variable to associate the window instance with that specific window. If the value of id_ is 0, it indicates that this window instance does not reference a valid window.
         */
        window_id id_{};
    };

    /**
     * @fn create_window
     * @brief Creates a new window based on the specified window description. This function takes a reference to a window_desc structure that contains the desired properties and behavior of the window to be created, such as its title, dimensions, visibility, and resizability. The function returns a window instance that references the newly created window. If the window creation fails for any reason (e.g., due to invalid parameters or platform limitations), the function will return an invalid window instance (i.e., a window with a window_id of 0). Developers can use this function to create windows in their applications and then interact with those windows using the various member functions and event handling capabilities provided by the Catalyst Platform library.
     * @param desc A reference to a window_desc structure that specifies the desired properties and behavior of the window to be created. This structure should be filled out with the appropriate values for the window's title, dimensions, visibility, and resizability before calling this function.
     * @return A window instance that references the newly created window. If the window creation fails for any reason, an invalid window instance (i.e., a window with a window_id of 0) will be returned.
     */
    [[nodiscard]] window create_window(const window_desc &desc);
    /**
     * @fn destroy_window
     * @brief Destroys the specified window and releases all associated resources. This function takes a reference to a window instance that references the window to be destroyed. If the provided window instance is valid (i.e., it references a valid window), the function will proceed to destroy the window and free any resources associated with it. After calling this function, the provided window instance will no longer reference a valid window, and any further operations on that instance should be avoided. It is important to call this function when a window is no longer needed to ensure that resources are properly released and to prevent memory leaks in the application.
     * @param w A reference to a window instance that references the window to be destroyed. This instance should be valid (i.e., it should reference a valid window) for the function to successfully destroy the window and release its resources.
     */
    void destroy_window(window &w) noexcept;
    /**
     * @fn is_valid
     * @brief Checks whether the specified window instance references a valid window. This function takes a reference to a window instance and returns true if the instance references a valid window (i.e., its window_id is not 0) and false otherwise. This can be used to verify that a window instance is valid before performing operations on it, helping to prevent errors and improve the robustness of the application.
     * @param w A reference to a window instance that will be checked for validity. This instance should be properly initialized and may reference a valid window or be invalid (i.e., have a window_id of 0).
     * @return True if the provided window instance references a valid window, false otherwise.
     */
    [[nodiscard]] bool is_valid(const window &w) noexcept;
    /**
     * @fn get_native_handle
     * @brief Retrieves the native handle associated with the specified window. This function takes a reference to a window instance and returns a native_handle structure that contains information about the type of native handle and the actual handle value. The native_handle can be used to interact with platform-specific APIs or for other purposes that require access to the underlying windowing system. If the provided window instance is invalid (i.e., it does not reference a valid window), the function will return a native_handle with a kind of native_handle_kind::none and a null handle value.
     * @param w A reference to a window instance for which the native handle will be retrieved. This instance should be valid (i.e., it should reference a valid window) for the function to successfully retrieve the native handle.
     * @return A native_handle structure containing information about the type of native handle and the actual handle value associated with the specified window. If the provided window instance is invalid, a native_handle with a kind of native_handle_kind::none and a null handle value will be returned.
     */
    [[nodiscard]] native_handle get_native_handle(const window &w) noexcept;
    /**
     * @fn client_rect_px
     * @brief Retrieves the client area rectangle of the specified window in pixels. This function takes a reference to a window instance and returns a rect_px that defines the dimensions of the client area of the window in pixels. The client area is the portion of the window where content can be rendered, excluding any non-client areas such as title bars, borders, and scroll bars. If the provided window instance is invalid (i.e., it does not reference a valid window), the function will return an empty rectangle with all dimensions set to zero.
     * @param w A reference to a window instance for which the client area rectangle will be retrieved. This instance should be valid (i.e., it should reference a valid window) for the function to successfully retrieve the client area rectangle.
     * @return A rect_px defining the dimensions of the client area of the specified window in pixels. If the provided window instance is invalid, an empty rectangle with all dimensions set to zero will be returned.
     */
    [[nodiscard]] rect_px client_rect_px(const window &w) noexcept;
    /**
     * @fn dpi_scale
     * @brief Retrieves the current DPI scaling factor for the specified window. This function takes a reference to a window instance and returns a float value representing the DPI scaling factor for that window. The DPI scaling factor indicates how much the content of the window should be scaled to appear at an appropriate size on high-DPI displays. A value of 1.0f indicates no scaling (i.e., 100% scale), while values greater than 1.0f indicate that content should be scaled up, and values less than 1.0f indicate that content should be scaled down. If the provided window instance is invalid (i.e., it does not reference a valid window), the function will return a default DPI scaling factor of 1.0f.
     * @param w A reference to a window instance for which the DPI scaling factor will be retrieved. This instance should be valid (i.e., it should reference a valid window) for the function to successfully retrieve the DPI scaling factor.
     * @return A float value representing the DPI scaling factor for the specified window. If the provided window instance is invalid, a default DPI scaling factor of 1.0f will be returned.
     */
    [[nodiscard]] float dpi_scale(const window &w) noexcept;

    /**
     * @fn resolve_context_for_window
     * @brief Creates a ui::resolve_context populated with DPI information for the given window.
     * @details This helper is intended to make physical unit resolution (in/cm/mm) correct on
     * systems that do not map to 96 px/in. The returned context sets both legacy `dpi_scale` and
     * the newer per-axis `dpi_x`/`dpi_y` fields.
     */
    [[nodiscard]] ui::resolve_context resolve_context_for_window(const window &w) noexcept;

    /**
     * @fn pump_events
     * @brief Pumps the OS event queue and processes all pending events. This function should be called regularly (e.g., once per frame) to ensure that the application remains responsive to user input and other system events. When called, this function will retrieve and process all pending events from the operating system's event queue, such as window events (e.g., resize, close) and input events (e.g., keyboard, mouse). By pumping events regularly, developers can ensure that their applications respond promptly to user interactions and system changes, providing a smooth and responsive user experience.
     */
    void pump_events() noexcept;

    /**
     * @fn wait_events
     * @brief Waits for events to be available in the OS event queue and processes them. This function is similar to pump_events, but instead of returning immediately if there are no pending events, it will block and wait until at least one event is available before processing. The function takes an optional timeout parameter (in milliseconds) that specifies how long to wait for events before returning. If the timeout is set to 0xFFFFFFFFu (the default), the function will wait indefinitely until an event is available. By using wait_events, developers can reduce CPU usage in scenarios where the application does not need to update continuously (e.g., when idle or waiting for user input), while still ensuring that events are processed in a timely manner when they occur.
     * @param timeout_ms An optional timeout value in milliseconds that specifies how long to wait for events before returning. If set to 0xFFFFFFFFu (the default), the function will wait indefinitely until at least one event is available.
     * @return True if events were processed, false if the wait timed out without any events becoming available.
     */
    [[nodiscard]] bool wait_events(std::uint32_t timeout_ms = 0xFFFFFFFFu) noexcept;

    /**
     * @fn set_event_bus
     * @brief Installs the bus that window events are dispatched to, synchronously from pump_events()/wait_events() on the
     * calling thread. Pass nullptr to stop dispatching them.
     * @details Only the window and monitor events declared in this module and in monitor.hpp go here. Input events go to
     * the feed installed with set_input_feed, so that every input event reaches a bus from the input module's registry
     * rather than from two places that can disagree about what is held down.
     *
     * With no bus installed the backend still translates and processes messages - a window stays responsive, resizes and
     * reports its state correctly - it simply dispatches nothing. Events are not retained for a bus installed later: there
     * is no queue behind this, because the one case that needed it (the OS holding the thread in a modal size/move loop)
     * is served by the frame callback instead. See set_frame_callback.
     * @param bus The bus to dispatch window events to, or nullptr for none. It must outlive the installation, or be
     * replaced before it is destroyed.
     */
    void set_event_bus(events::bus *bus) noexcept;

    /** @brief The bus installed with set_event_bus, or nullptr if none is installed. */
    [[nodiscard]] events::bus *event_bus() noexcept;

    /**
     * @fn set_input_feed
     * @brief Installs the feed that window-sourced input - keyboard, text, mouse, touch and pen - is delivered to,
     * synchronously from pump_events()/wait_events() on the calling thread. Pass nullptr to stop delivering it.
     * @details Keyboards and mice belong to a window, so their events arrive on the platform's message loop rather than
     * from a device the input module polls. This hands them straight to input::context (which implements event_feed), whose
     * registry owns the device state and publishes the events onward. The platform's job is translation only: it does not
     * track what is held, and on focus loss it calls feed_focus_lost and lets the registry synthesise the releases for
     * everything that window still holds.
     * @param feed The feed to deliver input to, or nullptr for none. It must outlive the installation, or be replaced
     * before it is destroyed.
     */
    void set_input_feed(input::event_feed *feed) noexcept;

    /** @brief The feed installed with set_input_feed, or nullptr if none is installed. */
    [[nodiscard]] input::event_feed *input_feed() noexcept;

    /**
     * @fn set_cursor_mode
     * @brief Changes how the cursor behaves over the window (see cursor_mode). Capture takes effect immediately if the
     * window has focus, otherwise when it next gains focus, and is released automatically while the window is unfocused.
     * Invalid windows are ignored.
     */
    void set_cursor_mode(const window &w, cursor_mode mode) noexcept;

    /** @brief The mode set with set_cursor_mode, or cursor_mode::normal for invalid windows. */
    [[nodiscard]] cursor_mode get_cursor_mode(const window &w) noexcept;

    // -----------------------------------------------------------------------------
    // Frame callback
    // -----------------------------------------------------------------------------

    /**
     * @fn set_frame_callback
     * @brief Installs (or, with a null callback, removes) the function the backend calls when @p w needs to be redrawn
     * while the application is not in control of the message pump. See frame_callback for the re-entrancy contract.
     * @details Install this if you want the window to keep rendering while the user drags its border or title bar. Without
     * it the operating system's modal size/move loop holds the thread for the whole drag and the window's contents are
     * stretched or frozen until the user lets go. The backend calls it on OS repaint requests, on every size change, and on
     * a timer while a size/move loop is running, so a stationary drag still produces frames.
     *
     * While a size/move loop is running the backend caps the callback at one call per display refresh period, so a drag
     * cannot spin it faster than the display can show the result. It caps only; it does not wait for vsync, because that
     * is the presenting code's job and doing both would halve the frame rate of a drag in any application that already
     * presents with vsync enabled. Outside such a loop the callback fires only when the OS asks for a repaint; it does not
     * replace the application's own render loop.
     * @param w The window to install the callback for. Invalid windows are ignored.
     * @param cb The function to call, or nullptr to stop receiving frame requests for this window.
     * @param user An opaque pointer passed back to @p cb unchanged. The backend does not own or inspect it.
     */
    void set_frame_callback(const window &w, frame_callback cb, void *user) noexcept;

    // -----------------------------------------------------------------------------
    // Window state
    // -----------------------------------------------------------------------------

    /** @brief Replaces the window's title. @p utf8_title is UTF-8; null or invalid windows are ignored. */
    void set_title(const window &w, const char *utf8_title) noexcept;

    /**
     * @brief Resizes the window so that its *client* area becomes the given size, keeping its top-left corner in place.
     * Lengths are resolved against the window's current DPI. Ignored for invalid windows.
     */
    void set_client_size(const window &w, ui::length width_px, ui::length height_px) noexcept;

    /** @brief Moves the window so that the top-left of its client area lands on @p position_px in screen coordinates. */
    void set_position(const window &w, const math::vec2<std::int32_t> &position_px) noexcept;

    /** @brief The top-left of the window's client area in screen coordinates, or {0,0} for invalid windows. */
    [[nodiscard]] math::vec2<std::int32_t> position_px(const window &w) noexcept;

    /**
     * @brief Constrains how far the user can resize the window, in client-area pixels. A zero component means
     * "unconstrained" on that axis, so {0,0},{0,0} (the default) removes all limits.
     * @note A minimum size is the simplest defence against the zero-sized swapchain a minimised or over-shrunk window would
     * otherwise hand a renderer.
     */
    void set_size_limits(const window &w, const math::vec2<std::int32_t> &min_px, const math::vec2<std::int32_t> &max_px) noexcept;

    /** @brief Shows the window without activating it if it is hidden. */
    void show(const window &w) noexcept;

    /** @brief Hides the window. A hidden window still exists and still receives non-input messages. */
    void hide(const window &w) noexcept;

    /** @brief Iconifies the window. Publishes a window_display_state_event. */
    void minimize(const window &w) noexcept;

    /** @brief Zooms the window to the work area of its monitor. Publishes a window_display_state_event. */
    void maximize(const window &w) noexcept;

    /** @brief Returns the window to its normal rectangle from a minimised or maximised state. */
    void restore(const window &w) noexcept;

    /** @brief Brings the window to the front and gives it keyboard focus. */
    void focus(const window &w) noexcept;

    /**
     * @brief Asks the desktop environment to draw the user's attention to the window (a flashing taskbar button on
     * Windows). Does nothing if the window already has focus.
     */
    void request_attention(const window &w) noexcept;

    /** @brief The window's current display state, or window_display_state::restored for invalid windows. */
    [[nodiscard]] window_display_state display_state(const window &w) noexcept;

    /** @brief Adds or removes the window's resize border and maximise button after creation. */
    void set_resizable(const window &w, bool resizable) noexcept;

    /** @brief Whether the user can resize the window. */
    [[nodiscard]] bool is_resizable(const window &w) noexcept;

    /** @brief Keeps the window above all non-topmost windows, or stops doing so. */
    void set_always_on_top(const window &w, bool on_top) noexcept;

    /**
     * @brief Sets whole-window opacity, from 0 (fully transparent) to 1 (opaque). Values outside that range are clamped.
     * @note Making a window translucent forces it down a composited path that is measurably slower to present; set it back
     * to 1 when the effect is no longer needed.
     */
    void set_opacity(const window &w, float opacity) noexcept;

    /**
     * @brief Puts the window into borderless fullscreen on the monitor it currently occupies, or takes it back out to the
     * rectangle and style it had beforehand.
     * @details This is "borderless windowed" fullscreen: the window is restyled and resized to cover the monitor, and the
     * display mode is left alone. It composites like any other window, so alt-tabbing is instant and the desktop resolution
     * never changes. Exclusive fullscreen (a real display mode change) is a rendering-backend concern, not a window one.
     */
    void set_fullscreen(const window &w, bool fullscreen) noexcept;

    /** @brief Whether the window is in borderless fullscreen (see set_fullscreen). */
    [[nodiscard]] bool is_fullscreen(const window &w) noexcept;

    /**
     * @brief Asks the desktop environment to draw the window's title bar and frame in its dark or light variant. Silently
     * does nothing where the platform has no such notion.
     */
    void set_dark_mode(const window &w, bool dark) noexcept;

} // namespace catalyst::platform
