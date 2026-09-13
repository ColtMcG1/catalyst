/**
 * @file detail_backend.hpp
 * @brief The seam every input backend (win32/, null/) implements.
 * @details Only devices that are *not* tied to a window are behind here. Keyboards, mice, touchscreens and pens belong
 * to a window, so the platform layer produces them and delivers them through `input::event_feed`; this seam covers
 * what `input::context::poll()` has to go and ask for.
 *
 * Gamepads are implemented. Generic HID, MIDI and the rest are declared and return "nothing here" from both backends,
 * which is Tier 4 of docs/input.md: the device model above them is complete, so filling one in is local to one file.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/input/device.hpp>
#include <catalyst/input/gamepad.hpp>
#include <catalyst/input/joystick.hpp>
#include <catalyst/input/midi.hpp>

#include <cstddef>
#include <cstdint>

namespace catalyst::input::detail
{

    /** @brief Name of the compiled-in backend, for module_name() and for logs. */
    const char *backend_name();

    // ----------------------------------------------------------------------------------------------------------------
    // Gamepads
    // ----------------------------------------------------------------------------------------------------------------

    /** @brief Slots the backend can report. Never more than input::max_gamepads. */
    [[nodiscard]] std::size_t gamepad_capacity() noexcept;

    /**
     * @brief Reads a slot's raw state into @p out.
     * @details Axes are already normalised - sticks over [-1, 1] with +y up, triggers over [0, 1] - but no dead zone is
     * applied; that is the context's job, because the threshold is the application's to choose.
     * @return False, leaving @p out untouched, if the slot is empty.
     */
    [[nodiscard]] bool read_gamepad(std::uint32_t slot, gamepad_state &out) noexcept;

    /**
     * @brief Sets a slot's vibration motors. Intensities arrive already clamped to [0, 1].
     * @return False if the slot is empty or the backend has no rumble.
     */
    bool set_gamepad_rumble(std::uint32_t slot, double low_frequency, double high_frequency) noexcept;

    // ----------------------------------------------------------------------------------------------------------------
    // Generic HID - Tier 4
    // ----------------------------------------------------------------------------------------------------------------

    /**
     * @struct joystick_report
     * @brief One generic HID device's current state, in the slot order joystick.hpp defines.
     */
    struct joystick_report
    {
        std::uint16_t axis_count{0};
        std::uint16_t button_count{0};
        std::uint16_t hat_count{0};
        std::array<float, max_joystick_axes> axes{};
        std::array<bool, max_joystick_buttons> buttons{};
        std::array<hat_direction, max_joystick_hats> hats{};
    };

    /** @brief How many generic HID devices are attached. */
    [[nodiscard]] std::size_t joystick_capacity() noexcept;

    /** @brief Identity and control layout of joystick @p index, for the device the context will register. */
    [[nodiscard]] bool describe_joystick(std::size_t index, device_info &info_out, layout_ref &layout_out) noexcept;

    /** @brief Reads joystick @p index. False if it is gone. */
    [[nodiscard]] bool read_joystick(std::size_t index, joystick_report &out) noexcept;

    // ----------------------------------------------------------------------------------------------------------------
    // MIDI - Tier 4
    // ----------------------------------------------------------------------------------------------------------------

    /** @brief How many MIDI input ports are open. */
    [[nodiscard]] std::size_t midi_port_count() noexcept;

    /** @brief Identity of MIDI input port @p index. */
    [[nodiscard]] bool describe_midi_port(std::size_t index, device_info &info_out) noexcept;

    /**
     * @brief Takes the next queued message from port @p index.
     * @return False when the port's queue is empty, which is how the caller knows to stop draining it.
     */
    [[nodiscard]] bool poll_midi(std::size_t index, midi_message &out, input_time &time_out) noexcept;

} // namespace catalyst::input::detail
