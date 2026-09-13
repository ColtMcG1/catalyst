/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief What the rendering module tells the rest of the program about, published on a
 * `catalyst::events::bus`.
 * @details Rendering was the last subsystem that told the application nothing. That mattered more
 * here than it did in audio, because the two things a renderer must report - the device went away,
 * and the swapchain no longer matches its window - are not failures of any one call. They are
 * facts about the world that invalidate work already in flight, and a `bool` returned from
 * whichever call happened to notice first is the wrong shape for them entirely.
 *
 * So both are published *and* returned: `swapchain::acquire` still fails with
 * @ref error_code::swapchain_out_of_date so the caller who is mid-frame can act immediately, and
 * @ref swapchain_out_of_date_event still reaches the bus so the parts of the program that are not
 * in the frame loop - a screenshot task, a UI that caches render targets - hear about it too. The
 * error is for the caller; the event is for everyone else.
 *
 * **Which thread.** Nothing here is published from a driver thread. Backends push onto an internal
 * queue as they observe things, and `device::pump()` drains it onto the bus from whichever thread
 * the caller drives the device from, exactly as `audio::stream::pump()` does. A listener added with
 * `bus.add_listener<...>` therefore runs on the caller's thread and may do anything an ordinary
 * function may do, including destroying and rebuilding the device it just heard about.
 *
 * The cost of that choice is that events are as timely as `pump()` is frequent. A program that
 * calls it once a frame learns about a lost device within a frame; a program that never calls it
 * learns nothing, and its queue is bounded and drops the oldest rather than growing.
 */

#pragma once

#include <catalyst/events/tag.hpp>
#include <catalyst/rendering/error.hpp>
#include <catalyst/rendering/types.hpp>

#include <cstdint>

namespace catalyst::rendering
{

    /**
     * @namespace catalyst::rendering::tags
     * @brief Static event tags for this module, so the bus dispatches on a constant rather than on
     * `typeid`.
     * @details The rendering module owns the block 0x0003'0000 - 0x0003'FFFF, next after the audio
     * module's. Values are never reused or reordered.
     *
     * The block is subdivided so later tiers can append without renumbering: 0x00-0x0F device,
     * 0x10-0x1F swapchain, 0x20-0x2F submission and timelines.
     */
    namespace tags
    {
        /** @brief First tag in the rendering module's block. */
        inline constexpr events::event_type_t rendering_base = 0x0003'0000u;

        inline constexpr events::event_type_t device_lost = rendering_base + 0x00;

        inline constexpr events::event_type_t swapchain_out_of_date = rendering_base + 0x10;
        inline constexpr events::event_type_t swapchain_resized = rendering_base + 0x11;
    } // namespace tags

    /**
     * @struct rendering_event
     * @brief The field every rendering event carries, inherited rather than repeated.
     * @tparam Tag The event's static tag; see @ref catalyst::rendering::tags.
     */
    template <events::event_type_t Tag>
    struct rendering_event : events::tagged<Tag>
    {
        /** @brief The backend that observed it. Always the compiled-in one; carried so a listener
         * shared between builds does not have to ask. */
        backend_kind backend = backend_kind::null;
    };

    /**
     * @struct device_lost_event
     * @brief The device was removed or reset, and every handle created from it is dead.
     * @details A driver update, a TDR, an external GPU unplugged, or a hang the driver recovered
     * from by discarding the context. Nothing is salvageable in place: destroy the device and
     * rebuild from `create_device` down. Resources are not automatically recreated, because only
     * the application knows which of them still matter.
     *
     * Published once per device. Calls made after it fail with @ref error_code::device_lost rather
     * than crashing, so a program that ignores this event degrades instead of faulting - but it
     * renders nothing from here on.
     */
    struct device_lost_event : rendering_event<tags::device_lost>
    {
        /** @brief What the driver said, for the log. @ref error::code is
         * @ref error_code::device_lost. */
        error cause{};
    };

    /**
     * @struct swapchain_out_of_date_event
     * @brief A swapchain stopped matching its surface and must be recreated before it presents
     * again.
     * @details Almost always a window resize; also a monitor change, a DPI change, or a compositor
     * restart. The device is fine and so is everything not attached to that window.
     *
     * Published when the backend first notices, which may be inside `acquire` or inside `present`.
     * The caller that was mid-frame also gets @ref error_code::swapchain_out_of_date from the call
     * itself - see the file comment for why both.
     */
    struct swapchain_out_of_date_event : rendering_event<tags::swapchain_out_of_date>
    {
        /** @brief The swapchain that needs recreating. */
        resource_id swapchain = 0;
    };

    /**
     * @struct swapchain_resized_event
     * @brief A swapchain's back buffers were successfully recreated at a new size.
     * @details Published by `resize_swapchain` after the new images exist. Every texture previously
     * returned by `acquire_next_image` is invalid by the time this is delivered, which is what
     * makes it worth hearing: anything holding a back-buffer handle - a cached render target, a
     * recorded-and-not-yet-submitted command list - has to drop it here.
     */
    struct swapchain_resized_event : rendering_event<tags::swapchain_resized>
    {
        /** @brief The swapchain that was recreated. */
        resource_id swapchain = 0;

        /** @brief The size it had before. */
        extent2d previous{};

        /** @brief The size it has now. */
        extent2d current{};
    };

} // namespace catalyst::rendering
