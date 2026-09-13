/**
 * @file touch.hpp
 * @brief Multi-touch: id-tracked contacts, their phases, and the events that report them.
 * @details A touchscreen is a device whose slots are contacts. Each contact occupies five consecutive slots - x, y,
 * pressure, size, and an "is down" flag - so a pinch gesture is two contacts' worth of ordinary controls and needs
 * nothing special from the binding layer.
 *
 * The `id` on an event is the platform's tracking id and is what identifies *a finger over time*; the `index` is which
 * slot the module assigned it, and is what identifies it in the control model. Slots are recycled once a contact ends,
 * ids are not, so gesture code should follow `id` and only ever read control values by `index`.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/input/device.hpp>
#include <catalyst/math/vector.hpp>

#include <cstddef>
#include <cstdint>

namespace catalyst::input
{
    /** @brief Simultaneous contacts a touchscreen device tracks. Ten is what the Windows touch stack guarantees. */
    inline constexpr std::size_t max_touch_points = 10;

    /**
     * @enum touch_phase
     * @brief Where a contact is in its life.
     */
    enum class touch_phase : std::uint8_t
    {
        /** @brief The finger went down. The contact's slots become live with this event. */
        began,
        /** @brief It moved. */
        moved,
        /** @brief It is still down and has not moved since the last frame. */
        stationary,
        /** @brief It lifted normally. The contact's slots return to rest after this event. */
        ended,
        /** @brief The system took the contact away - a gesture was recognised, the window lost focus. Treat as a
         *  cancellation, not a tap: whatever the contact was doing should be undone rather than committed. */
        cancelled
    };

    /** @brief True for the phases in which the finger is still on the glass. */
    [[nodiscard]] inline constexpr bool is_active_phase(touch_phase p) noexcept
    {
        return p == touch_phase::began || p == touch_phase::moved || p == touch_phase::stationary;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Controls
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @enum touch_control
     * @brief The five slots each contact occupies, in order.
     */
    enum class touch_control : std::uint8_t
    {
        /** @brief Position in client-area pixels. */
        x,
        y,
        /** @brief Normalised contact pressure, or 1 while down on hardware that does not report it. */
        pressure,
        /** @brief Normalised contact area, or 0 on hardware that does not report it. */
        size,
        /** @brief 1 while the contact is active, 0 otherwise. A button, so it can be bound like one. */
        down
    };

    /** @brief Slots per contact. */
    inline constexpr std::size_t touch_controls_per_point = 5;
    /** @brief Total number of controls a touchscreen device has. */
    inline constexpr std::size_t touch_control_count = max_touch_points * touch_controls_per_point;

    /** @brief The slot @p c of contact @p index, or no_control if @p index is out of range. */
    [[nodiscard]] inline constexpr control_id touch_control_of(std::size_t index, touch_control c) noexcept
    {
        if (index >= max_touch_points)
            return no_control;
        return control_at(index * touch_controls_per_point + static_cast<std::size_t>(c));
    }

    /**
     * @fn touch_layout
     * @brief The layout every touchscreen device shares: ten contacts of five controls each.
     */
    [[nodiscard]] const layout_ref &touch_layout();

    // ------------------------------------------------------------------------------------------------------------------
    // Events
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @struct touch_event
     * @brief One contact changed. A frame in which three fingers move produces three of these.
     */
    struct touch_event : device_event<tags::touch>
    {
        /** @brief The platform::window_id of the window the contact is over. */
        std::uint64_t window{0};
        /**
         * @brief The platform's tracking id: stable for the life of one contact, never reused while it is live. Follow
         * this to know that the finger at (100, 200) is the same finger that was at (90, 190).
         */
        std::uint32_t id{0};
        /** @brief Which contact slot the module assigned; this is what indexes the control model. */
        std::uint8_t index{0};
        touch_phase phase{touch_phase::began};
        /** @brief Position in client-area pixels. */
        math::vec2<float> position_px{};
        /** @brief Motion since this contact's previous event, in pixels. Zero for touch_phase::began. */
        math::vec2<float> delta_px{};
        /** @brief Normalised pressure in [0, 1], or 1 while down on hardware that does not report it. */
        float pressure{1.0f};
        /** @brief Normalised contact area in [0, 1], or 0 on hardware that does not report it. */
        float size{0.0f};
        /** @brief True if this is the contact that began the gesture, i.e. the only one down when it went down. */
        bool primary{false};

        /** @brief The slot @p c of this event's contact. */
        [[nodiscard]] constexpr control_id control(touch_control c) const noexcept { return touch_control_of(index, c); }
        /** @brief True while the finger is still on the glass. */
        [[nodiscard]] constexpr bool active() const noexcept { return is_active_phase(phase); }
    };

} // namespace catalyst::input
