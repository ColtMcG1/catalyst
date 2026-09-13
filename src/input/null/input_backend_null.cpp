/**
 * @file input_backend_null.cpp
 * @brief The do-nothing backend: reports no devices of any kind, so a build on a platform with no implementation still
 * links and runs. Keyboard and mouse are unaffected - those come from the platform layer through input::event_feed.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "../detail_backend.hpp"

namespace catalyst::input::detail
{

    const char *backend_name()
    {
        return "null";
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Gamepads
    // ----------------------------------------------------------------------------------------------------------------

    std::size_t gamepad_capacity() noexcept
    {
        return 0;
    }

    bool read_gamepad(std::uint32_t, gamepad_state &) noexcept
    {
        return false;
    }

    bool set_gamepad_rumble(std::uint32_t, double, double) noexcept
    {
        return false;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Generic HID
    // ----------------------------------------------------------------------------------------------------------------

    std::size_t joystick_capacity() noexcept
    {
        return 0;
    }

    bool describe_joystick(std::size_t, device_info &, layout_ref &) noexcept
    {
        return false;
    }

    bool read_joystick(std::size_t, joystick_report &) noexcept
    {
        return false;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // MIDI
    // ----------------------------------------------------------------------------------------------------------------

    std::size_t midi_port_count() noexcept
    {
        return 0;
    }

    bool describe_midi_port(std::size_t, device_info &) noexcept
    {
        return false;
    }

    bool poll_midi(std::size_t, midi_message &, input_time &) noexcept
    {
        return false;
    }

} // namespace catalyst::input::detail
