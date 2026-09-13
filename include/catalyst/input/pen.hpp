/**
 * @file pen.hpp
 * @brief Stylus input: position, pressure, tilt, twist, and the barrel and eraser buttons.
 * @details A pen is not a mouse with extra fields. It hovers - it reports a position while it is *near* the surface and
 * not touching it, which is how a drawing application shows a brush preview - and it reports which end is down, because
 * flipping it over to erase is a different tool, not a different button. Both of those are lost if pen input is folded
 * into the mouse stream, which is why this is its own device kind.
 *
 * Pressure and the tilt angles are normalised, so a program written against one tablet works on another. Tilt is
 * reported as a pair in [-1, 1] rather than in degrees for the same reason a stick is: it is what a binding wants, and
 * `pen_tilt_degrees()` is there for the cases that genuinely need the angle.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/input/device.hpp>
#include <catalyst/math/vector.hpp>

#include <cstddef>
#include <cstdint>

namespace catalyst::input
{
    /**
     * @enum pen_button
     * @brief The buttons on a stylus. `tip` is the pen touching the surface, which is a button in HID's model and is
     * genuinely the one every drawing program cares about most.
     */
    enum class pen_button : std::uint8_t
    {
        /** @brief The pen is in contact with the surface. */
        tip,
        /** @brief The side ("barrel") button. */
        barrel,
        /** @brief The second side button, on styli that have one. */
        secondary,
        /** @brief The eraser end is in contact with the surface. */
        eraser
    };

    /** @brief Number of values in pen_button. */
    inline constexpr std::size_t pen_button_count = 4;

    /**
     * @enum pen_axis
     * @brief The stylus's analog controls.
     */
    enum class pen_axis : std::uint8_t
    {
        /** @brief Position in client-area pixels. Sub-pixel: a tablet reports far finer than the screen grid. */
        x,
        y,
        /** @brief Tip force in [0, 1]. 0 while hovering. */
        pressure,
        /** @brief Tilt away from vertical, in [-1, 1] per axis; +x tilted right, +y tilted away from the user. */
        tilt_x,
        tilt_y,
        /** @brief Barrel rotation in [0, 1) for one full turn, on styli that report it. */
        twist,
        /** @brief Distance from the surface in [0, 1] while hovering; 0 on contact, 1 at the edge of detection. */
        distance
    };

    /** @brief Number of values in pen_axis. */
    inline constexpr std::size_t pen_axis_count = 7;

    /** @brief A normalised tilt component as an angle from vertical, in degrees over [-90, 90]. */
    [[nodiscard]] inline constexpr float pen_tilt_degrees(float normalised) noexcept { return normalised * 90.0f; }

    // ------------------------------------------------------------------------------------------------------------------
    // Controls
    // ------------------------------------------------------------------------------------------------------------------

    /** @brief Slot of the first pen button control; buttons run [0, pen_button_count). */
    inline constexpr std::size_t pen_button_control_base = 0;
    /** @brief Slot of the first pen axis control. */
    inline constexpr std::size_t pen_axis_control_base = pen_button_count;
    /** @brief Total number of controls a pen device has. */
    inline constexpr std::size_t pen_control_count = pen_axis_control_base + pen_axis_count;

    /** @brief The slot @p b occupies on a pen device. */
    [[nodiscard]] inline constexpr control_id control_of(pen_button b) noexcept
    {
        return control_at(pen_button_control_base + static_cast<std::size_t>(b));
    }

    /** @brief The slot @p a occupies on a pen device. */
    [[nodiscard]] inline constexpr control_id control_of(pen_axis a) noexcept
    {
        return control_at(pen_axis_control_base + static_cast<std::size_t>(a));
    }

    /**
     * @fn pen_layout
     * @brief The layout every pen device shares: four buttons then seven axes, in the order above.
     */
    [[nodiscard]] const layout_ref &pen_layout();

    // ------------------------------------------------------------------------------------------------------------------
    // Events
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @struct pen_event
     * @brief The stylus moved, or one of its analog values changed.
     * @note Published while hovering as well as while touching. `contact` distinguishes the two; a program that draws on
     * every pen_event without checking it will paint whenever the user passes the pen over the tablet.
     */
    struct pen_event : device_event<tags::pen>
    {
        /** @brief The platform::window_id of the window the pen is over. */
        std::uint64_t window{0};
        /** @brief The platform's pointer id, stable while the pen stays in range. */
        std::uint32_t id{0};
        /** @brief Position in client-area pixels, with sub-pixel precision. */
        math::vec2<float> position_px{};
        /** @brief Motion since the previous pen event, in pixels. */
        math::vec2<float> delta_px{};
        /** @brief Tip force in [0, 1]; 0 while hovering. */
        float pressure{0.0f};
        /** @brief Tilt from vertical, each component in [-1, 1]. */
        math::vec2<float> tilt{};
        /** @brief Barrel rotation in [0, 1), or 0 on styli that do not report it. */
        float twist{0.0f};
        /** @brief Distance from the surface in [0, 1]; 0 on contact. */
        float distance{0.0f};
        /** @brief True while the pen is touching the surface, false while hovering. */
        bool contact{false};
        /** @brief True when the eraser end, rather than the tip, is the one in use. */
        bool inverted{false};
    };

    /**
     * @struct pen_button_event
     * @brief A stylus button, including the tip and the eraser, changed state.
     */
    struct pen_button_event : device_event<tags::pen_button>
    {
        std::uint64_t window{0};
        std::uint32_t id{0};
        pen_button button{pen_button::tip};
        button_action action{button_action::press};
        /** @brief Position in client-area pixels when the button changed. */
        math::vec2<float> position_px{};

        [[nodiscard]] constexpr control_id control() const noexcept { return control_of(button); }
        [[nodiscard]] constexpr bool down() const noexcept { return is_down_action(action); }
    };

} // namespace catalyst::input
