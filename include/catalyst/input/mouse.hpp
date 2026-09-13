/**
 * @file mouse.hpp
 * @brief The mouse device: buttons, the axes that describe where it is and how it moved, and the events for both.
 * @details Positions are in client-area pixels of the window named by the event; every event also carries that window's
 * id (as a plain integer, so the input module does not depend on the platform module) and the modifier state sampled at
 * the time, which is what shift-click and control-drag need.
 *
 * Two kinds of motion are reported and they are not interchangeable. `mouse_move_event` is the cursor: accelerated,
 * clipped to the screen, and what a UI wants. `mouse_raw_move_event` is the device: unaccelerated, unclipped, published
 * only while the window's cursor mode is captured, and what a first-person camera wants - it keeps reporting motion
 * after the cursor has been pinned against the edge of the screen, which the cursor stream by definition cannot.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/input/device.hpp>
#include <catalyst/input/keyboard.hpp>
#include <catalyst/math/vector.hpp>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace catalyst::input
{
    /**
     * @enum mouse_button
     * @brief One mouse button. x1 and x2 are the "back" and "forward" thumb buttons found on most mice.
     */
    enum class mouse_button : std::uint8_t
    {
        left,
        right,
        middle,
        x1,
        x2,
        unknown
    };

    /** @brief Number of distinct buttons in mouse_button, not counting `unknown`. */
    inline constexpr std::size_t mouse_button_count = 5;

    /**
     * @enum mouse_buttons
     * @brief Bit set of mouse buttons; one bit per mouse_button value, bit index == enumerator value.
     */
    enum class mouse_buttons : std::uint8_t
    {
        none = 0x00,
        left = 0x01,
        right = 0x02,
        middle = 0x04,
        x1 = 0x08,
        x2 = 0x10
    };

    [[nodiscard]] inline constexpr mouse_buttons operator|(mouse_buttons a, mouse_buttons b) noexcept
    {
        using u = std::underlying_type_t<mouse_buttons>;
        return static_cast<mouse_buttons>(static_cast<u>(a) | static_cast<u>(b));
    }
    [[nodiscard]] inline constexpr mouse_buttons operator&(mouse_buttons a, mouse_buttons b) noexcept
    {
        using u = std::underlying_type_t<mouse_buttons>;
        return static_cast<mouse_buttons>(static_cast<u>(a) & static_cast<u>(b));
    }
    [[nodiscard]] inline constexpr mouse_buttons operator^(mouse_buttons a, mouse_buttons b) noexcept
    {
        using u = std::underlying_type_t<mouse_buttons>;
        return static_cast<mouse_buttons>(static_cast<u>(a) ^ static_cast<u>(b));
    }
    [[nodiscard]] inline constexpr mouse_buttons operator~(mouse_buttons a) noexcept
    {
        using u = std::underlying_type_t<mouse_buttons>;
        return static_cast<mouse_buttons>(static_cast<u>(~static_cast<u>(a) & 0x1Fu));
    }
    inline constexpr mouse_buttons &operator|=(mouse_buttons &a, mouse_buttons b) noexcept { return a = (a | b); }
    inline constexpr mouse_buttons &operator&=(mouse_buttons &a, mouse_buttons b) noexcept { return a = (a & b); }

    /** @brief A single button as a bit set. `unknown` maps to `none`. */
    [[nodiscard]] inline constexpr mouse_buttons to_mouse_buttons(mouse_button b) noexcept
    {
        if (b == mouse_button::unknown)
            return mouse_buttons::none;
        return static_cast<mouse_buttons>(1u << static_cast<std::uint8_t>(b));
    }

    /** @brief True if @p set contains @p b. */
    [[nodiscard]] inline constexpr bool has_button(mouse_buttons set, mouse_button b) noexcept
    {
        return (set & to_mouse_buttons(b)) != mouse_buttons::none;
    }

    /**
     * @enum mouse_axis
     * @brief The mouse's analog controls.
     * @details `x`/`y` are absolute; the rest accumulate over a frame and are cleared by input_state::new_frame(), which
     * is what makes them bindable as a look axis without the application differencing positions itself.
     */
    enum class mouse_axis : std::uint8_t
    {
        /** @brief Cursor position in client-area pixels. */
        x,
        y,
        /** @brief Cursor motion this frame, in pixels. Accelerated and clipped, like the cursor itself. */
        delta_x,
        delta_y,
        /** @brief Wheel movement this frame, in notches; +y away from the user, +x to the right. */
        wheel_x,
        wheel_y,
        /** @brief Device motion this frame, in device counts. Non-zero only while the cursor is captured. */
        raw_x,
        raw_y
    };

    /** @brief Number of values in mouse_axis. */
    inline constexpr std::size_t mouse_axis_count = 8;

    // ------------------------------------------------------------------------------------------------------------------
    // Controls
    // ------------------------------------------------------------------------------------------------------------------

    /** @brief Slot of the first button control on a mouse device; buttons run [0, mouse_button_count). */
    inline constexpr std::size_t mouse_button_control_base = 0;
    /** @brief Slot of the first axis control on a mouse device. */
    inline constexpr std::size_t mouse_axis_control_base = mouse_button_count;
    /** @brief Total number of controls a mouse device has. */
    inline constexpr std::size_t mouse_control_count = mouse_axis_control_base + mouse_axis_count;

    /** @brief The slot @p b occupies on a mouse device, or no_control for mouse_button::unknown. */
    [[nodiscard]] inline constexpr control_id control_of(mouse_button b) noexcept
    {
        if (b == mouse_button::unknown)
            return no_control;
        return control_at(mouse_button_control_base + static_cast<std::size_t>(b));
    }

    /** @brief The slot @p a occupies on a mouse device. */
    [[nodiscard]] inline constexpr control_id control_of(mouse_axis a) noexcept
    {
        return control_at(mouse_axis_control_base + static_cast<std::size_t>(a));
    }

    /**
     * @fn mouse_layout
     * @brief The layout every mouse device shares: five buttons then eight axes, in the order above.
     */
    [[nodiscard]] const layout_ref &mouse_layout();

    // ------------------------------------------------------------------------------------------------------------------
    // Events
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @struct mouse_move_event
     * @brief The cursor moved inside the window's client area - or outside it, while a button is held and the window has
     * the mouse captured.
     */
    struct mouse_move_event : device_event<tags::mouse_move>
    {
        /** @brief The platform::window_id of the window the event belongs to. */
        std::uint64_t window{0};
        /** @brief Cursor position in client-area pixels. May be outside the client area during a drag. */
        math::vec2<std::int32_t> position_px{};
        /** @brief Motion since the previous move event for this window, in pixels. Zero on the first event. */
        math::vec2<std::int32_t> delta_px{};
        /** @brief The buttons held while the cursor moved. */
        mouse_buttons buttons{mouse_buttons::none};
        /** @brief Modifier state at the time of the event. */
        key_modifiers modifiers{key_modifiers::none};
    };

    /**
     * @struct mouse_button_event
     * @brief A button was pressed or released over the window, or while the window held the mouse capture.
     * @note A double-click is a press with `clicks == 2`, not a separate event, so every press is still paired with
     * exactly one release and code that ignores `clicks` still balances.
     */
    struct mouse_button_event : device_event<tags::mouse_button>
    {
        /** @brief The platform::window_id of the window the event belongs to. */
        std::uint64_t window{0};
        mouse_button button{mouse_button::unknown};
        /** @brief Press or release; a mouse never produces button_action::repeat. */
        button_action action{button_action::press};
        /** @brief For a press, how many consecutive clicks it completes (1 = single, 2 = double). Always 1 for a release. */
        std::uint8_t clicks{1};
        /** @brief Cursor position in client-area pixels. */
        math::vec2<std::int32_t> position_px{};
        /** @brief Modifier state at the time of the event. */
        key_modifiers modifiers{key_modifiers::none};

        /** @brief The slot this event changed. */
        [[nodiscard]] constexpr control_id control() const noexcept { return control_of(button); }
        /** @brief True if the button is down afterwards. */
        [[nodiscard]] constexpr bool down() const noexcept { return is_down_action(action); }
    };

    /**
     * @struct mouse_wheel_event
     * @brief The wheel turned, or a touchpad scroll gesture happened.
     */
    struct mouse_wheel_event : device_event<tags::mouse_wheel>
    {
        /** @brief The platform::window_id of the window the event belongs to. */
        std::uint64_t window{0};
        /** @brief Cursor position in client-area pixels. */
        math::vec2<std::int32_t> position_px{};
        /**
         * @brief Scroll in wheel notches: +y away from the user, +x to the right. High-resolution wheels and touchpads
         * report fractions. Multiply by the application's lines-per-notch setting for line scrolling.
         */
        math::vec2<float> delta{};
        /** @brief Modifier state at the time of the event. */
        key_modifiers modifiers{key_modifiers::none};
    };

    /**
     * @struct mouse_enter_event
     * @brief The cursor entered the window's client area. Eventually followed by a mouse_leave_event.
     */
    struct mouse_enter_event : device_event<tags::mouse_enter>
    {
        std::uint64_t window{0};
        math::vec2<std::int32_t> position_px{};
    };

    /**
     * @struct mouse_leave_event
     * @brief The cursor left the window's client area.
     */
    struct mouse_leave_event : device_event<tags::mouse_leave>
    {
        std::uint64_t window{0};
    };

    /**
     * @struct mouse_raw_move_event
     * @brief Unaccelerated, unclipped relative motion straight from the device.
     * @details Published only while the window's cursor mode is platform::cursor_mode::captured. This is what a
     * first-person camera should be driven from: it keeps reporting motion once the cursor has been pinned to the edge
     * of the screen, where mouse_move_event necessarily reports nothing.
     */
    struct mouse_raw_move_event : device_event<tags::mouse_raw_move>
    {
        /** @brief The platform::window_id of the window holding the capture. */
        std::uint64_t window{0};
        /** @brief Motion in device counts, not pixels. Signs match screen axes, so +y is towards the user. */
        math::vec2<std::int32_t> delta{};
    };

} // namespace catalyst::input
