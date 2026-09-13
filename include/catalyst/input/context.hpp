/**
 * @file context.hpp
 * @brief The object that owns the input module: devices, backends, frame state and actions.
 * @details Everything the old module did through file-scope globals is here instead. That is not tidiness for its own
 * sake - it is what makes the module testable without hardware, lets a tool run two independent input contexts, and
 * removes a class of static-initialisation-order bug that only ever showed up in release builds.
 *
 * A frame looks like this, and the order is not arbitrary:
 *
 *     in.new_frame();            // clear frame edges and delta controls
 *     platform::pump_events();   // window messages -> feed_*() -> registry -> bus
 *     in.poll();                 // gamepads, HID, MIDI -> registry -> bus
 *     in.update();               // evaluate actions -> action_event on the bus
 *
 * `new_frame()` comes first because deltas and edges describe *this* frame and must be empty before anything is added
 * to them. `update()` comes last because an action should see everything that happened, not most of it.
 *
 * The context implements `event_feed`, which is how the platform layer delivers window-sourced input; hand it over once
 * with `platform::set_input_feed(&in)`.
 *
 * Threading: not synchronised. Drive it from one thread. Listeners run on whichever thread publishes, which is that
 * one.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/input/action_map.hpp>
#include <catalyst/input/feed.hpp>
#include <catalyst/input/gamepad.hpp>
#include <catalyst/input/joystick.hpp>
#include <catalyst/input/midi.hpp>
#include <catalyst/input/registry.hpp>
#include <catalyst/input/state.hpp>

#include <array>
#include <cstddef>
#include <cstdint>

namespace catalyst::input
{

    /**
     * @class context
     * @brief The input module, as an object.
     */
    class context final : public event_feed
    {
    public:
        /**
         * @brief Constructs a context publishing to @p bus.
         * @param bus Must outlive the context.
         */
        explicit context(events::bus &bus);

        context(const context &) = delete;
        context &operator=(const context &) = delete;

        ~context() override;

        // --------------------------------------------------------------------------------------------------------------
        // Parts
        // --------------------------------------------------------------------------------------------------------------

        /** @brief The bus every input event is published to. */
        [[nodiscard]] events::bus &bus() const noexcept { return m_registry.bus(); }
        /** @brief The device store. Use it to enumerate hardware or to read a control the enums do not name. */
        [[nodiscard]] device_registry &devices() noexcept { return m_registry; }
        [[nodiscard]] const device_registry &devices() const noexcept { return m_registry; }
        /** @brief The polled frame state - is_key_down(), was_gamepad_button_pressed(), and the rest. */
        [[nodiscard]] input_state &state() noexcept { return m_state; }
        [[nodiscard]] const input_state &state() const noexcept { return m_state; }
        /** @brief The action maps. */
        [[nodiscard]] action_set &actions() noexcept { return m_actions; }
        [[nodiscard]] const action_set &actions() const noexcept { return m_actions; }

        /** @brief Name of the compiled-in backend ("win32" or "null"). */
        [[nodiscard]] const char *backend_name() const noexcept;

        // --------------------------------------------------------------------------------------------------------------
        // The frame
        // --------------------------------------------------------------------------------------------------------------

        /** @brief Clears frame edges and zeroes every delta control. Call first, before pumping events. */
        void new_frame() noexcept;

        /**
         * @brief Reads every device the module polls rather than receives - gamepads, generic HID, MIDI - and publishes
         * what changed.
         * @note Slots that were empty last time are re-probed only a few times a second, because probing an empty
         * XInput slot costs about a millisecond and doing it four times a frame is visible in a profile.
         */
        void poll();

        /** @brief Evaluates every enabled action map against the current device state, stamped with the current time. */
        void update();

        /**
         * @brief update() with an explicit timestamp.
         * @details Lets a replay drive interactions off its own clock and get bit-identical hold and tap timings.
         */
        void update(input_time now);

        // --------------------------------------------------------------------------------------------------------------
        // Gamepads
        // --------------------------------------------------------------------------------------------------------------

        /** @brief How many gamepad slots the backend can report (4 for XInput, 0 for the null backend). */
        [[nodiscard]] std::size_t gamepad_capacity() const noexcept;

        /** @brief The dead zone applied to every gamepad on poll(). */
        [[nodiscard]] gamepad_deadzone deadzone() const noexcept { return m_deadzone; }
        /** @brief Replaces the dead zone used for subsequent polls. Values are clamped to a usable range. */
        void set_deadzone(const gamepad_deadzone &dz) noexcept;

        /**
         * @brief A gamepad's axes exactly as the backend reported them, before the dead zone.
         * @details What a calibration UI needs: it is showing the user their controller's resting noise, which the dead
         * zone exists to hide. Buttons and `connected` match the processed state.
         */
        [[nodiscard]] gamepad_state raw_gamepad(std::uint32_t slot) const noexcept;

        /**
         * @brief Sets a pad's vibration motors. Intensities are clamped to [0, 1]; zeroes stop them.
         * @return False if the slot is empty or the backend has no rumble.
         */
        bool set_rumble(std::uint32_t slot, const rumble_state &r) noexcept;
        /** @brief set_rumble() by device handle. */
        bool set_rumble(device_id id, const rumble_state &r) noexcept;

        // --------------------------------------------------------------------------------------------------------------
        // Devices the application supplies
        // --------------------------------------------------------------------------------------------------------------

        /**
         * @brief Registers a device the application drives itself - a replay, a network peer, an on-screen gamepad.
         * @details It behaves exactly like hardware from every other layer's point of view: bindings match it, actions
         * read it, `input_state` reports it. Write its controls with `devices().set_value()`.
         * @param kind The kind it should impersonate. Pass device_kind::gamepad and a gamepad_layout() to have existing
         * gamepad bindings pick it up with no changes.
         */
        device_id add_simulated_device(device_kind kind, layout_ref layout, std::string name = {},
                                       std::uint32_t slot = any_slot);

        // --------------------------------------------------------------------------------------------------------------
        // event_feed - called by the platform layer
        // --------------------------------------------------------------------------------------------------------------

        void feed_key(const key_event &e) override;
        void feed_text(const text_input_event &e) override;
        void feed_composition(const text_composition_event &e) override;
        void feed_focus_lost(std::uint64_t window) override;
        void feed_mouse_move(const mouse_move_event &e) override;
        void feed_mouse_button(const mouse_button_event &e) override;
        void feed_mouse_wheel(const mouse_wheel_event &e) override;
        void feed_mouse_enter(const mouse_enter_event &e) override;
        void feed_mouse_leave(const mouse_leave_event &e) override;
        void feed_mouse_raw_move(const mouse_raw_move_event &e) override;
        void feed_touch(const touch_event &e) override;
        void feed_pen(const pen_event &e) override;
        void feed_pen_button(const pen_button_event &e) override;

        /** @brief The keyboard device, creating it if this is the first keyboard input of the session. */
        [[nodiscard]] device_id keyboard();
        /** @brief The mouse device, creating it on first use. */
        [[nodiscard]] device_id mouse();
        /** @brief The touchscreen device, creating it on first use. */
        [[nodiscard]] device_id touchscreen();
        /** @brief The pen device, creating it on first use. */
        [[nodiscard]] device_id pen_device();

    private:
        /** @brief One polled gamepad slot's bookkeeping. */
        struct gamepad_slot
        {
            device_id device{};
            /** @brief Axis values as the backend gave them, before the dead zone. For calibration UIs. */
            std::array<double, gamepad_axis_count> raw_axes{};
            bool connected{false};
            input_time next_probe{};
        };

        [[nodiscard]] device_id ensure_device(device_id &cache, device_kind kind, const layout_ref &layout,
                                              const char *name);

        void poll_gamepads(input_time now);
        void apply_deadzones(gamepad_state &s) const noexcept;
        /** @brief Writes a pad's controls and publishes the typed events for whatever changed. */
        void publish_gamepad_state(gamepad_slot &slot, std::uint32_t index, const gamepad_state &next, input_time now);

        /** @brief Sets a control and publishes @p e, with the device and time filled in. */
        template <typename Event>
        void feed_control(device_id device, control_id control, float value, Event e);

        device_registry m_registry;
        input_state m_state;
        action_set m_actions;

        device_id m_keyboard{};
        device_id m_mouse{};
        device_id m_touch{};
        device_id m_pen{};

        std::array<gamepad_slot, max_gamepads> m_gamepads{};
        gamepad_deadzone m_deadzone{};

        /** @brief Contact tracking id -> slot index, so a finger keeps the same control slots for its whole life. */
        std::array<std::uint32_t, max_touch_points> m_touch_ids{};
        std::array<bool, max_touch_points> m_touch_used{};
    };

} // namespace catalyst::input
