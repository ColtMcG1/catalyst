/**
 * @file joystick.hpp
 * @brief Generic HID sticks, wheels, HOTAS and throttle quadrants - everything Generic-Desktop that is not a gamepad.
 * @details A gamepad has a layout the whole industry agreed on, so gamepad.hpp can name every control. Nothing else
 * does. A flight stick has eleven axes and thirty-two buttons, a wheel has three pedals and a shifter, and no enum will
 * ever cover the next one, so a joystick's controls are addressed *by index* and named at runtime from the device's own
 * report descriptor.
 *
 * The slot map is fixed and generous rather than packed, so `control_of` stays constexpr and a binding can name
 * "joystick axis 6" before any hardware is plugged in:
 *
 *     [0, max_joystick_buttons)                       buttons
 *     [axis base, + max_joystick_axes)                axes
 *     [hat base, + max_joystick_hats * 2)             hats, as an (x, y) pair each
 *
 * Slots a given device does not have simply stay at rest. The unused floats cost a few hundred bytes per device, which
 * is a good trade for never having to ask the layout a question before you can write a binding. What the descriptor
 * *does* decide is the `control_info::name` - so a bindings screen shows "Throttle", not "Axis 5".
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/input/device.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <utility>

namespace catalyst::input
{
    /** @brief Buttons a joystick device can report. HID allows more; nothing in practice has this many. */
    inline constexpr std::size_t max_joystick_buttons = 128;
    /** @brief Analog axes a joystick device can report - enough for a HOTAS with a throttle quadrant and pedals. */
    inline constexpr std::size_t max_joystick_axes = 16;
    /** @brief Hat switches a joystick device can report. */
    inline constexpr std::size_t max_joystick_hats = 4;

    /**
     * @enum hat_direction
     * @brief The nine positions of a hat switch. Values are ordered clockwise from `up` so that
     * `(value - 1) * 45` degrees recovers the angle, matching how HID reports them.
     */
    enum class hat_direction : std::uint8_t
    {
        centred = 0,
        up,
        up_right,
        right,
        down_right,
        down,
        down_left,
        left,
        up_left
    };

    /** @brief Number of values in hat_direction. */
    inline constexpr std::size_t hat_direction_count = 9;

    /** @brief The x component of a hat direction: -1 left, 0 centred, +1 right. */
    [[nodiscard]] inline constexpr float hat_x(hat_direction d) noexcept
    {
        switch (d)
        {
        case hat_direction::up_right:
        case hat_direction::right:
        case hat_direction::down_right:
            return 1.0f;
        case hat_direction::down_left:
        case hat_direction::left:
        case hat_direction::up_left:
            return -1.0f;
        default:
            return 0.0f;
        }
    }

    /** @brief The y component of a hat direction: +1 up, 0 centred, -1 down. Matches the module's +y-up convention. */
    [[nodiscard]] inline constexpr float hat_y(hat_direction d) noexcept
    {
        switch (d)
        {
        case hat_direction::up_left:
        case hat_direction::up:
        case hat_direction::up_right:
            return 1.0f;
        case hat_direction::down_right:
        case hat_direction::down:
        case hat_direction::down_left:
            return -1.0f;
        default:
            return 0.0f;
        }
    }

    /** @brief The hat direction nearest a pair of axis components. Exact for the values hat_x()/hat_y() produce. */
    [[nodiscard]] hat_direction hat_from_axes(float x, float y) noexcept;

    // ------------------------------------------------------------------------------------------------------------------
    // Controls
    // ------------------------------------------------------------------------------------------------------------------

    /** @brief Slot of joystick button 0. */
    inline constexpr std::size_t joystick_button_control_base = 0;
    /** @brief Slot of joystick axis 0. */
    inline constexpr std::size_t joystick_axis_control_base = joystick_button_control_base + max_joystick_buttons;
    /** @brief Slot of hat 0's x component; each hat occupies two consecutive slots, x then y. */
    inline constexpr std::size_t joystick_hat_control_base = joystick_axis_control_base + max_joystick_axes;
    /** @brief Total number of controls a joystick device has. */
    inline constexpr std::size_t joystick_control_count = joystick_hat_control_base + max_joystick_hats * 2;

    /** @brief The slot of joystick button @p n, or no_control if @p n is out of range. */
    [[nodiscard]] inline constexpr control_id joystick_button_control(std::size_t n) noexcept
    {
        return n < max_joystick_buttons ? control_at(joystick_button_control_base + n) : no_control;
    }

    /** @brief The slot of joystick axis @p n, or no_control if @p n is out of range. */
    [[nodiscard]] inline constexpr control_id joystick_axis_control(std::size_t n) noexcept
    {
        return n < max_joystick_axes ? control_at(joystick_axis_control_base + n) : no_control;
    }

    /** @brief The slot of hat @p n's x component, or no_control if @p n is out of range. y is the next slot. */
    [[nodiscard]] inline constexpr control_id joystick_hat_x_control(std::size_t n) noexcept
    {
        return n < max_joystick_hats ? control_at(joystick_hat_control_base + n * 2) : no_control;
    }

    /** @brief The slot of hat @p n's y component, or no_control if @p n is out of range. */
    [[nodiscard]] inline constexpr control_id joystick_hat_y_control(std::size_t n) noexcept
    {
        return n < max_joystick_hats ? control_at(joystick_hat_control_base + n * 2 + 1) : no_control;
    }

    /**
     * @fn default_joystick_layout
     * @brief A layout with every slot present and generically named ("Button 3", "Axis 5", "Hat 1 X").
     * @details Used for a device whose descriptor could not be read, and as the starting point a backend edits when it
     * can: `make_joystick_layout()` takes the real names for the controls the device actually has.
     */
    [[nodiscard]] const layout_ref &default_joystick_layout();

    /**
     * @fn make_joystick_layout
     * @brief Builds a joystick layout in which the controls @p named covers carry the device's own names.
     * @param named Pairs of (slot, name) from the report descriptor. Slots not covered keep their generic names, and
     * names must have static storage duration or outlive the layout - a backend interning descriptor strings satisfies
     * this by owning them alongside the device.
     * @param usages Optional (slot, USB HID usage) pairs, so a bindings screen can group axes by what they mean.
     */
    [[nodiscard]] layout_ref make_joystick_layout(std::span<const std::pair<control_id, std::string_view>> named,
                                                  std::span<const std::pair<control_id, usb_hid>> usages = {});

    // ------------------------------------------------------------------------------------------------------------------
    // Events
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @struct joystick_button_event
     * @brief A button on a generic HID device changed state.
     */
    struct joystick_button_event : device_event<tags::joystick_button>
    {
        /** @brief Zero-based button index, as ordered in the device's report descriptor. */
        std::uint16_t button{0};
        button_action action{button_action::press};

        [[nodiscard]] constexpr control_id control() const noexcept { return joystick_button_control(button); }
        [[nodiscard]] constexpr bool down() const noexcept { return is_down_action(action); }
    };

    /**
     * @struct joystick_axis_event
     * @brief An axis on a generic HID device changed, after its dead zone.
     * @note Axes are normalised by the backend from the descriptor's logical range: an axis that rests centred reports
     * [-1, 1], and one that rests at an end - a throttle, a pedal - reports [0, 1]. `kind` says which, because a binding
     * cannot apply a sensible dead zone without knowing.
     */
    struct joystick_axis_event : device_event<tags::joystick_axis>
    {
        /** @brief Zero-based axis index, as ordered in the device's report descriptor. */
        std::uint16_t axis{0};
        /** @brief control_kind::axis for a centred axis, control_kind::ratio for a one-ended one. */
        control_kind kind{control_kind::axis};
        double value{0.0};
        double previous{0.0};

        [[nodiscard]] constexpr control_id control() const noexcept { return joystick_axis_control(axis); }
    };

    /**
     * @struct joystick_hat_event
     * @brief A hat switch moved.
     */
    struct joystick_hat_event : device_event<tags::joystick_hat>
    {
        /** @brief Zero-based hat index. */
        std::uint16_t hat{0};
        hat_direction direction{hat_direction::centred};
        hat_direction previous{hat_direction::centred};
    };

} // namespace catalyst::input
