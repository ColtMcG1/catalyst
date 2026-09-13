/**
 * @file monitor.hpp
 * @brief Monitor management and event handling for the Catalyst platform library.
 * @details This header defines types and functions related to monitor management and event handling in the Catalyst platform library. It includes definitions for monitor descriptors, monitor events, and functions for retrieving monitor information. The monitor management system allows developers to query connected monitors, their properties, and handle events related to monitor changes (e.g. when a monitor is connected or disconnected). By using the functionality provided in this header, developers can create applications that are responsive to changes in the display environment and can adapt to different monitor configurations effectively.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/math/vector.hpp>
#include <catalyst/platform/platform.hpp>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

/**
 * @namespace catalyst::platform
 * @brief The catalyst::platform namespace contains all the platform-specific types and functions provided by the Catalyst Platform library. This includes window management, event handling, and other platform-related utilities. By organizing all platform-specific functionality within this namespace, we can avoid naming conflicts and provide a clear structure for users of the library to access the various platform tools they need for their applications. The Catalyst Platform library is designed to be efficient, easy to use, and compatible with modern C++ standards, making it a valuable resource for developers building real-time applications and games.
 * @details The Catalyst Platform library provides a collection of platform utilities and types commonly used in game development and real-time applications. By including the appropriate headers from the Catalyst Platform library, users can access all the functionality they need for their projects, such as creating windows, handling events, and interacting with the underlying operating system in a platform-agnostic way. This allows developers to focus on building their applications without worrying about the complexities of platform-specific code, while still having access to powerful tools for managing windows and events effectively.
 */
namespace catalyst::platform
{

    class window;

    /**
     * @typedef monitor_id
     * @brief A unique identifier for a monitor. This type is used to represent and manage individual monitors within the Catalyst Platform library. Each monitor connected to the system is assigned a unique monitor_id, which can be used to reference and manipulate that specific monitor throughout its lifecycle. The monitor_id is typically implemented as a 64-bit unsigned integer, allowing for a large number of monitors to be managed simultaneously without conflicts.
     * @details The monitor_id type serves as a fundamental building block for the Catalyst Platform library's monitor management system. By using unique identifiers for each monitor, the library can efficiently track and manage multiple monitors, allowing developers to create applications that can adapt to different display configurations and respond to changes in the display environment effectively. The monitor_id can be used in various functions and methods within the library to perform operations on specific monitors, such as retrieving their properties or processing events related to those monitors.
     */
    using monitor_id = std::uint64_t;

    /**
     * @struct monitor_desc
     * @brief A structure that describes the properties of a monitor. This includes the monitor's name, position in pixels, size in pixels, size in millimeters, and refresh rate in hertz. The monitor_desc structure is used to represent the characteristics of a monitor when it is connected, disconnected, or changed, allowing applications to respond to changes in the display environment effectively.
     */
    struct monitor_desc
    {
        /**
         * @var id
         * @brief The unique identifier for the monitor. This value is assigned by the system and can be used to reference the monitor in various functions and events within the Catalyst Platform library.
         */
        monitor_id id = 0;

        /**
         * @var name
         * @brief The name of the monitor. This is typically a human-readable string that describes the monitor, such as its model name or manufacturer. The name can be used for display purposes or for identifying the monitor in logs and debugging output.
         */
        std::string name;

        /**
         * @var bounds_px
         * @brief The monitor bounds in virtual desktop pixels.
         * @details This rectangle includes both position (min) and extent (max). For multi-monitor setups,
         * this is typically relative to a common origin (often the primary monitor).
         */
        rect_px bounds_px{};

        /**
         * @var work_area_px
         * @brief The usable work area rectangle in pixels.
         * @details This typically excludes OS UI such as the taskbar/dock/menu bar.
         */
        rect_px work_area_px{};

        /**
         * @var size_mm
         * @brief The physical size of the monitor in millimeters.
         * @details May be {0,0} if unknown.
         */
        math::vec2<float> size_mm{};

        /**
         * @var dpi_x
         * @brief Effective pixels-per-inch on the x axis.
         * @details This is the value you want for physical unit conversion (in/cm/mm) in UI.
         */
        float dpi_x = 96.0f;

        /**
         * @var dpi_y
         * @brief Effective pixels-per-inch on the y axis.
         * @details This is the value you want for physical unit conversion (in/cm/mm) in UI.
         */
        float dpi_y = 96.0f;

        /**
         * @var refresh_rate_hz
         * @brief The refresh rate of the monitor in hertz. This represents how many times per second the monitor updates its display. The refresh rate can be used to manage rendering and animation timing in applications, ensuring that they are synchronized with the monitor's capabilities for smoother visuals and better performance.
         */
        std::uint32_t refresh_rate_hz = 0;

        /**
         * @var primary
         * @brief Whether this is the primary monitor.
         */
        bool primary = false;
    };

    /**
     * @struct monitor_connected_event
     * @brief An event that is generated when a monitor is connected to the system. This event contains a monitor_desc structure that describes the properties of the newly connected monitor, allowing applications to respond to the new display environment effectively.
     */
    struct monitor_connected_event
    {
        monitor_desc desc;
    };

    /**
     * @struct monitor_disconnected_event
     * @brief An event that is generated when a monitor is disconnected from the system. This event contains a monitor_desc structure that describes the properties of the disconnected monitor, allowing applications to respond to the change in the display environment effectively.
     */
    struct monitor_disconnected_event
    {
        monitor_desc desc;
    };

    /**
     * @struct monitor_changed_event
     * @brief An event that is generated when a monitor's properties are changed (e.g. resolution change, position change). This event contains a monitor_desc structure that describes the new properties of the monitor, allowing applications to respond to changes in the display environment effectively.
     */
    struct monitor_changed_event
    {
        monitor_desc desc;
    };

    /**
     * @fn get_monitor_count
     * @brief Retrieves the number of monitors currently connected to the system. This function allows applications to query how many monitors are available, which can be useful for managing multiple display configurations and responding to changes in the display environment.
     * @return The number of monitors currently connected to the system. This value can be used by applications to determine how many monitors are available for use and to manage multiple display configurations effectively.
     */
    [[nodiscard]] std::size_t get_monitor_count() noexcept;

    /**
     * @fn get_monitor_list
     * @brief Retrieves a list of monitor_desc structures describing the properties of all currently connected monitors. This function allows applications to query detailed information about each connected monitor, such as their names, positions, sizes in pixels, and sizes in millimeters, enabling them to respond to changes in the display environment effectively.
     * @return A vector of monitor_desc structures describing the properties of all currently connected monitors. Each structure includes the monitor's name, position in pixels, size in pixels, and size in millimeters, allowing applications to respond to changes in the display environment effectively.
     */
    [[nodiscard]] std::vector<monitor_desc> get_monitor_list() noexcept;

    /**
     * @fn get_monitor
     * @brief Retrieves monitor information by stable monitor_id.
     * @return A monitor_desc for the specified monitor, or a default-initialized descriptor if not found.
     */
    [[nodiscard]] monitor_desc get_monitor(monitor_id id) noexcept;

    /**
     * @fn primary_monitor
     * @brief Returns the primary monitor id, or 0 if unknown.
     */
    [[nodiscard]] monitor_id primary_monitor() noexcept;

    /**
     * @fn monitor_for_window
     * @brief Returns the monitor id that a window is currently on, or 0 if unknown.
     */
    [[nodiscard]] monitor_id monitor_for_window(const window &w) noexcept;
}