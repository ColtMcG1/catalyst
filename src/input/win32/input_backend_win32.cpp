#include "../detail_backend.hpp"

#include <win32/windows_lean.hpp>

#include <Xinput.h>

#include <algorithm>
#include <cstdint>

// Gamepad backend built on XInput 1.4. XInput exposes up to four Xbox-compatible controllers with a fixed layout that
// maps 1:1 onto input::gamepad_button/gamepad_axis. The guide button is not reported by the public API, so it never
// actuates here.
//
// The generic-HID, MIDI, touch and pen seams below are Tier 4 of docs/input.md. The device model above them is finished,
// so each is a self-contained fill-in: Raw Input plus HidP_* for joysticks, WinMM midiIn* for MIDI. They report nothing
// until then rather than pretending, so poll() simply finds no devices of those kinds.
namespace catalyst::input::detail
{
    namespace
    {
        [[nodiscard]] gamepad_buttons map_buttons(WORD w) noexcept
        {
            gamepad_buttons b = gamepad_buttons::none;
            if (w & XINPUT_GAMEPAD_A) b |= gamepad_buttons::a;
            if (w & XINPUT_GAMEPAD_B) b |= gamepad_buttons::b;
            if (w & XINPUT_GAMEPAD_X) b |= gamepad_buttons::x;
            if (w & XINPUT_GAMEPAD_Y) b |= gamepad_buttons::y;
            if (w & XINPUT_GAMEPAD_BACK) b |= gamepad_buttons::back;
            if (w & XINPUT_GAMEPAD_START) b |= gamepad_buttons::start;
            if (w & XINPUT_GAMEPAD_LEFT_THUMB) b |= gamepad_buttons::left_stick;
            if (w & XINPUT_GAMEPAD_RIGHT_THUMB) b |= gamepad_buttons::right_stick;
            if (w & XINPUT_GAMEPAD_LEFT_SHOULDER) b |= gamepad_buttons::left_shoulder;
            if (w & XINPUT_GAMEPAD_RIGHT_SHOULDER) b |= gamepad_buttons::right_shoulder;
            if (w & XINPUT_GAMEPAD_DPAD_UP) b |= gamepad_buttons::dpad_up;
            if (w & XINPUT_GAMEPAD_DPAD_DOWN) b |= gamepad_buttons::dpad_down;
            if (w & XINPUT_GAMEPAD_DPAD_LEFT) b |= gamepad_buttons::dpad_left;
            if (w & XINPUT_GAMEPAD_DPAD_RIGHT) b |= gamepad_buttons::dpad_right;
            return b;
        }

        [[nodiscard]] double normalise_stick(SHORT v) noexcept
        {
            // SHORT_MIN would give -1.00003; clamp so the range is exactly [-1, 1].
            return std::clamp(static_cast<double>(v) / 32767.0, -1.0, 1.0);
        }

        [[nodiscard]] double normalise_trigger(BYTE v) noexcept
        {
            return static_cast<double>(v) / 255.0;
        }
    } // namespace

    const char *backend_name()
    {
        return "win32";
    }

    std::size_t gamepad_capacity() noexcept
    {
        return XUSER_MAX_COUNT;
    }

    bool read_gamepad(std::uint32_t id, gamepad_state &out) noexcept
    {
        if (id >= XUSER_MAX_COUNT)
            return false;

        XINPUT_STATE st{};
        if (XInputGetState(static_cast<DWORD>(id), &st) != ERROR_SUCCESS)
            return false;

        const XINPUT_GAMEPAD &g = st.Gamepad;

        out.connected = true;
        out.buttons = map_buttons(g.wButtons);
        out.axes[static_cast<std::size_t>(gamepad_axis::left_x)] = normalise_stick(g.sThumbLX);
        out.axes[static_cast<std::size_t>(gamepad_axis::left_y)] = normalise_stick(g.sThumbLY);
        out.axes[static_cast<std::size_t>(gamepad_axis::right_x)] = normalise_stick(g.sThumbRX);
        out.axes[static_cast<std::size_t>(gamepad_axis::right_y)] = normalise_stick(g.sThumbRY);
        out.axes[static_cast<std::size_t>(gamepad_axis::left_trigger)] = normalise_trigger(g.bLeftTrigger);
        out.axes[static_cast<std::size_t>(gamepad_axis::right_trigger)] = normalise_trigger(g.bRightTrigger);
        return true;
    }

    bool set_gamepad_rumble(std::uint32_t id, double low_frequency, double high_frequency) noexcept
    {
        if (id >= XUSER_MAX_COUNT)
            return false;

        XINPUT_VIBRATION v{};
        v.wLeftMotorSpeed = static_cast<WORD>(low_frequency * 65535.0);
        v.wRightMotorSpeed = static_cast<WORD>(high_frequency * 65535.0);
        return XInputSetState(static_cast<DWORD>(id), &v) == ERROR_SUCCESS;
    }

    // ----------------------------------------------------------------------------------------------------------------
    // Generic HID - Tier 4 (Raw Input + HidP_GetCaps / HidP_GetUsages)
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
    // MIDI - Tier 4 (WinMM midiIn*)
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
