/**
 * @file feed.hpp
 * @brief The interface the platform layer pushes window-sourced input through.
 * @details Keyboards, mice, touchscreens and pens belong to a window, so their events come from the platform's message
 * loop rather than from a backend this module polls. In the old design the platform published those events to the event
 * sink itself, which meant input events could reach the bus without the input module ever seeing them - and the
 * tracker that folded them could, and did, drift from the poller.
 *
 * Now the platform calls a `event_feed` instead, `input::context` implements it, and *every* input event reaches the bus
 * from the registry. The platform's job shrinks to translating window messages, which is the job it should have had.
 *
 * A caller fills in everything it knows and leaves `device` null; the feed assigns the device, creating one on first
 * use. `time` is stamped if left at its default.
 *
 * The interface is abstract so `catalyst::platform` links against this header alone rather than the whole module, and
 * so a test can drive the platform side with a recording stub.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/input/mouse.hpp>
#include <catalyst/input/pen.hpp>
#include <catalyst/input/text.hpp>
#include <catalyst/input/touch.hpp>

#include <cstdint>

namespace catalyst::input
{

    /**
     * @class event_feed
     * @brief What the platform layer calls to deliver window-sourced input.
     */
    class event_feed
    {
    public:
        virtual ~event_feed() = default;

        // --------------------------------------------------------------------------------------------------------------
        // Keyboard and text
        // --------------------------------------------------------------------------------------------------------------

        /** @brief A physical key went down, auto-repeated, or came up. */
        virtual void feed_key(const key_event &e) = 0;

        /** @brief The user committed text. Control characters must not be delivered here; send those as keys. */
        virtual void feed_text(const text_input_event &e) = 0;

        /** @brief An input method's provisional text changed. Optional: a platform without IME support never calls it. */
        virtual void feed_composition(const text_composition_event &e) = 0;

        /**
         * @brief A window lost keyboard focus; release everything it still holds.
         * @details The platform used to track held keys per window purely so it could synthesise the releases. It does
         * not have to any more: the registry already knows what is down, so it can produce exactly the right releases
         * from one call. Pass 0 to release every device regardless of window.
         */
        virtual void feed_focus_lost(std::uint64_t window) = 0;

        // --------------------------------------------------------------------------------------------------------------
        // Mouse
        // --------------------------------------------------------------------------------------------------------------

        virtual void feed_mouse_move(const mouse_move_event &e) = 0;
        virtual void feed_mouse_button(const mouse_button_event &e) = 0;
        virtual void feed_mouse_wheel(const mouse_wheel_event &e) = 0;
        virtual void feed_mouse_enter(const mouse_enter_event &e) = 0;
        virtual void feed_mouse_leave(const mouse_leave_event &e) = 0;

        /** @brief Unaccelerated device motion. Only delivered while the window's cursor mode is captured. */
        virtual void feed_mouse_raw_move(const mouse_raw_move_event &e) = 0;

        // --------------------------------------------------------------------------------------------------------------
        // Touch and pen
        // --------------------------------------------------------------------------------------------------------------

        /** @brief One touch contact changed. `index` is assigned by the feed; leave it at 0 and set `id`. */
        virtual void feed_touch(const touch_event &e) = 0;

        virtual void feed_pen(const pen_event &e) = 0;
        virtual void feed_pen_button(const pen_button_event &e) = 0;
    };

} // namespace catalyst::input
