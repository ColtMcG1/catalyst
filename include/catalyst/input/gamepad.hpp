/**
 * @file gamepad.hpp
 * @brief The gamepad device: the fixed Xbox-style button and axis layout, its snapshot type, dead zones, and events.
 * @details A gamepad is a joystick whose layout everyone already agrees on, which is why it gets named enums where
 * joystick.hpp gets indices. Anything Generic-Desktop that is *not* one of these - a wheel, a HOTAS, a throttle
 * quadrant - is a `device_kind::joystick` and is described by a layout discovered at connect time instead.
 *
 * Stick axes are normalised to [-1, 1] with **+y up**, triggers to [0, 1]. The dead zone is applied by the poller,
 * before both the snapshot and the events, so a resting stick produces no events at all. Learn one from a resting
 * controller with the calibrator in calibration.hpp rather than tuning it by hand.
 *
 * Unlike a keyboard or a mouse, a gamepad has no window, so it is not fed by the platform layer: input::context polls it
 * from its own backend (XInput on Windows) when you call poll().
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/input/device.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace catalyst::input
{
    /** @brief Upper bound on gamepad slots across all backends. The active backend's real limit is usually lower. */
    inline constexpr std::size_t max_gamepads = 8;

    /**
     * @enum gamepad_button
     * @brief The digital inputs, named positionally after the Xbox layout: `a` is the bottom face button, `b` the right,
     * `x` the left, `y` the top. `back`/`start` are "view"/"menu" on newer pads; `guide` is the platform/home button.
     */
    enum class gamepad_button : std::uint8_t
    {
        a,
        b,
        x,
        y,
        back,
        guide,
        start,
        left_stick,
        right_stick,
        left_shoulder,
        right_shoulder,
        dpad_up,
        dpad_down,
        dpad_left,
        dpad_right
    };

    /** @brief Number of values in gamepad_button. */
    inline constexpr std::size_t gamepad_button_count = 15;

    /**
     * @enum gamepad_buttons
     * @brief Bit set of gamepad buttons; one bit per gamepad_button value, bit index == enumerator value.
     */
    enum class gamepad_buttons : std::uint16_t
    {
        none = 0,
        a = 1u << 0,
        b = 1u << 1,
        x = 1u << 2,
        y = 1u << 3,
        back = 1u << 4,
        guide = 1u << 5,
        start = 1u << 6,
        left_stick = 1u << 7,
        right_stick = 1u << 8,
        left_shoulder = 1u << 9,
        right_shoulder = 1u << 10,
        dpad_up = 1u << 11,
        dpad_down = 1u << 12,
        dpad_left = 1u << 13,
        dpad_right = 1u << 14
    };

    [[nodiscard]] inline constexpr gamepad_buttons operator|(gamepad_buttons a, gamepad_buttons b) noexcept
    {
        using u = std::underlying_type_t<gamepad_buttons>;
        return static_cast<gamepad_buttons>(static_cast<u>(a) | static_cast<u>(b));
    }
    [[nodiscard]] inline constexpr gamepad_buttons operator&(gamepad_buttons a, gamepad_buttons b) noexcept
    {
        using u = std::underlying_type_t<gamepad_buttons>;
        return static_cast<gamepad_buttons>(static_cast<u>(a) & static_cast<u>(b));
    }
    [[nodiscard]] inline constexpr gamepad_buttons operator^(gamepad_buttons a, gamepad_buttons b) noexcept
    {
        using u = std::underlying_type_t<gamepad_buttons>;
        return static_cast<gamepad_buttons>(static_cast<u>(a) ^ static_cast<u>(b));
    }
    [[nodiscard]] inline constexpr gamepad_buttons operator~(gamepad_buttons a) noexcept
    {
        using u = std::underlying_type_t<gamepad_buttons>;
        return static_cast<gamepad_buttons>(static_cast<u>(~static_cast<u>(a) & 0x7FFFu));
    }
    inline constexpr gamepad_buttons &operator|=(gamepad_buttons &a, gamepad_buttons b) noexcept { return a = (a | b); }
    inline constexpr gamepad_buttons &operator&=(gamepad_buttons &a, gamepad_buttons b) noexcept { return a = (a & b); }

    /** @brief A single button as a bit set. */
    [[nodiscard]] inline constexpr gamepad_buttons to_gamepad_buttons(gamepad_button b) noexcept
    {
        return static_cast<gamepad_buttons>(1u << static_cast<std::uint8_t>(b));
    }

    /** @brief True if @p set contains @p b. */
    [[nodiscard]] inline constexpr bool has_button(gamepad_buttons set, gamepad_button b) noexcept
    {
        return (set & to_gamepad_buttons(b)) != gamepad_buttons::none;
    }

    /**
     * @enum gamepad_axis
     * @brief The analog inputs. Sticks range over [-1, 1] with +x right and +y up; triggers over [0, 1].
     */
    enum class gamepad_axis : std::uint8_t
    {
        left_x,
        left_y,
        right_x,
        right_y,
        left_trigger,
        right_trigger
    };

    /** @brief Number of values in gamepad_axis. */
    inline constexpr std::size_t gamepad_axis_count = 6;

    /** @brief True for the four stick axes, which rest at 0 and swing both ways. */
    [[nodiscard]] inline constexpr bool is_stick_axis(gamepad_axis a) noexcept
    {
        return a <= gamepad_axis::right_y;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Controls
    // ------------------------------------------------------------------------------------------------------------------

    /** @brief Slot of the first button control on a gamepad; buttons run [0, gamepad_button_count). */
    inline constexpr std::size_t gamepad_button_control_base = 0;
    /** @brief Slot of the first axis control on a gamepad. */
    inline constexpr std::size_t gamepad_axis_control_base = gamepad_button_count;
    /** @brief Total number of controls a gamepad device has. */
    inline constexpr std::size_t gamepad_control_count = gamepad_axis_control_base + gamepad_axis_count;

    /** @brief The slot @p b occupies on a gamepad device. */
    [[nodiscard]] inline constexpr control_id control_of(gamepad_button b) noexcept
    {
        return control_at(gamepad_button_control_base + static_cast<std::size_t>(b));
    }

    /** @brief The slot @p a occupies on a gamepad device. */
    [[nodiscard]] inline constexpr control_id control_of(gamepad_axis a) noexcept
    {
        return control_at(gamepad_axis_control_base + static_cast<std::size_t>(a));
    }

    /** @brief The button a gamepad slot belongs to; empty-valued when the slot is an axis or out of range. */
    [[nodiscard]] inline constexpr bool gamepad_button_of(control_id c, gamepad_button &out) noexcept
    {
        if (!c.valid() || c.index >= gamepad_axis_control_base)
            return false;
        out = static_cast<gamepad_button>(c.index - gamepad_button_control_base);
        return true;
    }

    /** @brief The axis a gamepad slot belongs to; false when the slot is a button or out of range. */
    [[nodiscard]] inline constexpr bool gamepad_axis_of(control_id c, gamepad_axis &out) noexcept
    {
        if (!c.valid() || c.index < gamepad_axis_control_base || c.index >= gamepad_control_count)
            return false;
        out = static_cast<gamepad_axis>(c.index - gamepad_axis_control_base);
        return true;
    }

    /**
     * @fn gamepad_layout
     * @brief The layout every gamepad device shares: fifteen buttons then six axes, in the order above.
     */
    [[nodiscard]] const layout_ref &gamepad_layout();

    // ------------------------------------------------------------------------------------------------------------------
    // Snapshot and dead zone
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @struct gamepad_state
     * @brief One gamepad's controls as of the last poll, in the shape most game code wants to read them.
     * @note This is a *view* of the registry's control values, assembled on request. The registry is still the source of
     * truth; nothing writes back through this.
     */
    struct gamepad_state
    {
        /** @brief False if the slot is empty; the other fields are then at rest. */
        bool connected{false};
        /** @brief The buttons currently held. */
        gamepad_buttons buttons{gamepad_buttons::none};
        /** @brief Axis values indexed by gamepad_axis, dead zone already applied. */
        std::array<double, gamepad_axis_count> axes{};

        /** @brief True if @p b is held. */
        [[nodiscard]] constexpr bool is_down(gamepad_button b) const noexcept { return has_button(buttons, b); }
        /** @brief The value of @p a. */
        [[nodiscard]] constexpr double axis(gamepad_axis a) const noexcept { return axes[static_cast<std::size_t>(a)]; }
    };

    /**
     * @struct gamepad_deadzone
     * @brief The thresholds below which a resting control reads as zero.
     * @details Sticks use a *radial* dead zone - the 2D magnitude is compared against `stick` and the remainder rescaled
     * to [0, 1] - so a diagonal is not clipped into a square, which is what a per-axis threshold does. Triggers use a
     * linear one. The defaults are the values Microsoft publishes for XInput.
     */
    struct gamepad_deadzone
    {
        double stick{7849.0 / 32767.0};
        double trigger{30.0 / 255.0};
    };

    /**
     * @fn apply_deadzone
     * @brief A linear dead zone on a value in [-1, 1]: inside +/-@p threshold becomes 0, and the rest is rescaled so the
     * output still reaches +/-1 rather than jumping at the boundary.
     */
    [[nodiscard]] double apply_deadzone(double value, double threshold) noexcept;

    /**
     * @fn apply_radial_deadzone
     * @brief A radial dead zone on a 2D stick: below @p threshold both components become 0; above it the magnitude is
     * rescaled to [0, 1] and the direction is preserved.
     */
    void apply_radial_deadzone(double &x, double &y, double threshold) noexcept;

    /**
     * @struct rumble_state
     * @brief The two vibration motors most pads have, each in [0, 1]. Zeroes stop them.
     * @note `low_frequency` is the heavy motor and `high_frequency` the light one; a pad with one motor uses the larger.
     */
    struct rumble_state
    {
        double low_frequency{0.0};
        double high_frequency{0.0};
    };

    // ------------------------------------------------------------------------------------------------------------------
    // Events
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @struct gamepad_button_event
     * @brief A gamepad button changed state.
     * @note A pad never produces button_action::repeat; there is no auto-repeat in hardware.
     */
    struct gamepad_button_event : device_event<tags::gamepad_button>
    {
        /** @brief The pad's slot, i.e. device_info::slot. Kept alongside `device` because it is what game code names. */
        std::uint32_t slot{0};
        gamepad_button button{gamepad_button::a};
        button_action action{button_action::press};
        /** @brief How far the button travelled, for pads whose buttons are analog; 0 or 1 for the rest. */
        float value{0.0f};

        [[nodiscard]] constexpr control_id control() const noexcept { return control_of(button); }
        [[nodiscard]] constexpr bool down() const noexcept { return is_down_action(action); }
    };

    /**
     * @struct gamepad_axis_event
     * @brief A gamepad axis changed, after the dead zone - so a resting stick is silent.
     */
    struct gamepad_axis_event : device_event<tags::gamepad_axis>
    {
        std::uint32_t slot{0};
        gamepad_axis axis{gamepad_axis::left_x};
        /** @brief The new value. */
        double value{0.0};
        /** @brief What it was before. */
        double previous{0.0};

        [[nodiscard]] constexpr control_id control() const noexcept { return control_of(axis); }
    };

} // namespace catalyst::input
