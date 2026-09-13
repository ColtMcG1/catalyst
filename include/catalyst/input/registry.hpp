/**
 * @file registry.hpp
 * @brief The device store: who is connected, what their controls currently read, and the generic events for both.
 * @details This is the module's source of truth. Backends and the platform layer write control values in; `input_state`
 * and the action layer read them out; typed events are published from the same writes. Nothing else keeps a parallel
 * copy of "what is held", which is the bug the old module had - a tracker folding the event stream could and did
 * disagree with the poller that produced it.
 *
 * The registry is deliberately ignorant of what any control *means*. It stores floats, notices when one changes, and
 * publishes `control_changed_event`. Turning that into a `key_event` or a `gamepad_axis_event` is `input::context`'s
 * job, because that is where the semantics live. The split is what keeps this class from growing a branch per device
 * kind, and what lets a device kind nobody has written yet work with no change here at all.
 *
 * Threading: not synchronised. Write from one thread - the one that pumps the platform's messages and calls poll().
 * Reads from other threads need external synchronisation, and a snapshot is usually the better answer anyway.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/events/bus.hpp>
#include <catalyst/input/device.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace catalyst::input
{

    /**
     * @class device_registry
     * @brief Owns every connected device and its control values, and publishes the generic events describing changes.
     */
    class device_registry
    {
    public:
        /**
         * @brief Constructs a registry that publishes to @p bus.
         * @param bus Must outlive the registry.
         */
        explicit device_registry(events::bus &bus) noexcept : m_bus(&bus) {}

        device_registry(const device_registry &) = delete;
        device_registry &operator=(const device_registry &) = delete;

        // --------------------------------------------------------------------------------------------------------------
        // Devices
        // --------------------------------------------------------------------------------------------------------------

        /**
         * @brief Registers a device and publishes device_connected_event.
         * @param info Its identity. `info.name` is filled with the kind's name if it is empty.
         * @param layout Its controls. Must be non-null; the device's value array is sized from it.
         * @return The new handle. Never null.
         */
        device_id add_device(device_info info, layout_ref layout);

        /**
         * @brief Resets a device's controls to rest, publishes device_disconnected_event, and frees the slot.
         * @details The reset happens *before* the event, and no per-control release events are published for what was
         * held - a consumer tracking held state clears it on the disconnect, which is one event rather than thirty.
         * A no-op for a handle that is already stale.
         */
        void remove_device(device_id id);

        /** @brief Removes every device, publishing a disconnect for each. */
        void clear();

        /** @brief True if @p id names a device that is still connected. False for a handle held across a disconnect. */
        [[nodiscard]] bool alive(device_id id) const noexcept;

        /** @brief A device's identity, or nullptr if @p id is stale. */
        [[nodiscard]] const device_info *info(device_id id) const noexcept;

        /** @brief A device's controls, or nullptr if @p id is stale. */
        [[nodiscard]] const device_layout *layout(device_id id) const noexcept;

        /** @brief A device's layout as a shared handle, or an empty one if @p id is stale. */
        [[nodiscard]] layout_ref layout_ref_of(device_id id) const noexcept;

        /** @brief Every connected device, in connection order. Invalidated by add_device()/remove_device(). */
        [[nodiscard]] std::span<const device_id> devices() const noexcept { return m_live; }

        /** @brief How many devices are connected. */
        [[nodiscard]] std::size_t size() const noexcept { return m_live.size(); }

        /**
         * @brief The first connected device @p sel picks, or no_device.
         * @details "First" is connection order, so `find(device_selector::of(device_kind::gamepad))` is the pad that has
         * been plugged in longest, which is the one a single-player game should follow.
         */
        [[nodiscard]] device_id find(const device_selector &sel) const noexcept;

        /** @brief The connected device with this kind and slot, or no_device. */
        [[nodiscard]] device_id find(device_kind kind, std::uint32_t slot) const noexcept;

        /** @brief Calls `fn(device_id)` for every connected device @p sel picks, in connection order. */
        template <typename F>
        void for_each(const device_selector &sel, F &&fn) const
        {
            for (const device_id id : m_live)
            {
                const slot_data *s = find_slot(id);
                if (s && sel.matches(id, s->info.kind, s->info.slot))
                    fn(id);
            }
        }

        // --------------------------------------------------------------------------------------------------------------
        // Control values
        // --------------------------------------------------------------------------------------------------------------

        /** @brief A control's current value, or 0 for a stale handle or a slot the device does not have. */
        [[nodiscard]] float value(device_id id, control_id control) const noexcept;

        /** @brief What a control reads, given a press point: `value >= point`. Useful for treating an axis as a button. */
        [[nodiscard]] bool is_down(device_id id, control_id control, float press_point = 0.5f) const noexcept;

        /**
         * @brief Writes a control value, publishing control_changed_event if it differs from what was there.
         * @return True if the value changed. False for a stale handle, an unknown slot, or a write of the same value -
         * which is what callers diff on to decide whether to publish their own typed event.
         */
        bool set_value(device_id id, control_id control, float value);

        /**
         * @brief Writes a control value without publishing anything.
         * @details For a backend that is about to publish its own typed event and wants exactly one generic event, or
         * none. Prefer set_value(); reach for this when you are constructing a device's initial state.
         * @return True if the value changed.
         */
        bool set_value_silent(device_id id, control_id control, float value) noexcept;

        /** @brief Every control value of a device, in slot order. Empty for a stale handle. */
        [[nodiscard]] std::span<const float> values(device_id id) const noexcept;

        /**
         * @brief Resets every `control_kind::delta` control on every device to zero, without publishing.
         * @details Deltas accumulate within a frame and mean nothing across one. Called by input::context::new_frame();
         * it is what lets a mouse delta be bound as a look axis without the application differencing positions itself.
         */
        void clear_deltas() noexcept;

        /**
         * @brief Resets every control of a device to rest, publishing a change for each one that was not already there.
         * @details What focus loss does to a keyboard: everything held comes up, properly, so nothing is left stuck.
         */
        void reset_device(device_id id);

        // --------------------------------------------------------------------------------------------------------------
        // Publishing
        // --------------------------------------------------------------------------------------------------------------

        /** @brief The bus this registry publishes to. */
        [[nodiscard]] events::bus &bus() const noexcept { return *m_bus; }

        /** @brief Stamps @p e with the current time if it carries none, then publishes it. */
        template <typename Event>
        void publish(Event e)
        {
            if (e.time == input_time{})
                e.time = input_clock::now();
            m_bus->dispatch(std::move(e));
        }

    private:
        struct slot_data
        {
            device_info info{};
            layout_ref layout{};
            std::vector<float> values{};
            /** @brief 0 while the slot is free; otherwise the generation of the device occupying it. */
            std::uint16_t generation{0};
        };

        [[nodiscard]] slot_data *find_slot(device_id id) noexcept;
        [[nodiscard]] const slot_data *find_slot(device_id id) const noexcept;

        events::bus *m_bus{nullptr};
        std::vector<slot_data> m_slots;
        std::vector<std::uint16_t> m_free;
        std::vector<device_id> m_live;
        /** @brief Bumped for every device, so a handle is stale the moment its device goes away. Never 0. */
        std::uint16_t m_next_generation{1};
    };

} // namespace catalyst::input
