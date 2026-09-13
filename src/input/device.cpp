/**
 * @file device.cpp
 * @brief device_layout lookups, device kind names, and the built-in layout for each device kind.
 * @details Every built-in kind's layout is built once, on first use, and shared by reference, so a hundred connected
 * devices of one kind cost one control list between them. Names are `string_view`s into static storage - either string
 * literals here or the tables key_name() and midi_note_name() already own.
 * License: MIT (see LICENSE).
 */

#include <catalyst/input/device.hpp>
#include <catalyst/input/gamepad.hpp>
#include <catalyst/input/joystick.hpp>
#include <catalyst/input/keyboard.hpp>
#include <catalyst/input/midi.hpp>
#include <catalyst/input/mouse.hpp>
#include <catalyst/input/pen.hpp>
#include <catalyst/input/touch.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace catalyst::input
{
    namespace
    {
        /**
         * @brief Interns a generated name ("Button 37", "C#4") so a control_info can hold a view of it.
         * @details The pool is never cleared and never rehashed away: these are a few thousand short strings, built
         * once, that have to outlive every layout that points into them. Using a deque of strings rather than a vector
         * keeps existing views valid as it grows.
         */
        std::string_view intern(std::string s)
        {
            static std::deque<std::string> pool;
            pool.push_back(std::move(s));
            return pool.back();
        }

        /** @brief `prefix` followed by `n`, interned. */
        std::string_view numbered(std::string_view prefix, std::size_t n)
        {
            return intern(std::string(prefix) + std::to_string(n));
        }

        /** @brief Wraps a freshly built control list in a shared layout. */
        layout_ref make_layout(device_kind kind, std::vector<control_info> controls)
        {
            return std::make_shared<const device_layout>(kind, std::move(controls));
        }
    } // namespace

    // ------------------------------------------------------------------------------------------------------------------
    // device_kind
    // ------------------------------------------------------------------------------------------------------------------

    std::string_view device_kind_name(device_kind kind) noexcept
    {
        switch (kind)
        {
        case device_kind::any: return "any";
        case device_kind::keyboard: return "keyboard";
        case device_kind::mouse: return "mouse";
        case device_kind::gamepad: return "gamepad";
        case device_kind::joystick: return "joystick";
        case device_kind::touchscreen: return "touchscreen";
        case device_kind::pen: return "pen";
        case device_kind::midi: return "midi";
        case device_kind::sensor: return "sensor";
        case device_kind::simulated: return "simulated";
        }
        return "unknown";
    }

    // ------------------------------------------------------------------------------------------------------------------
    // device_layout
    // ------------------------------------------------------------------------------------------------------------------

    control_id device_layout::find_usage(usb_hid usage) const noexcept
    {
        if (usage == usb_hid_unknown)
            return no_control;

        const auto list = controls();
        for (std::size_t i = 0; i < list.size(); ++i)
        {
            if (list[i].usage == usage)
                return control_at(i);
        }
        return no_control;
    }

    control_id device_layout::find_name(std::string_view name) const noexcept
    {
        if (name.empty())
            return no_control;

        const auto list = controls();
        for (std::size_t i = 0; i < list.size(); ++i)
        {
            if (list[i].name == name)
                return control_at(i);
        }
        return no_control;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Keyboard
    // ------------------------------------------------------------------------------------------------------------------

    const layout_ref &keyboard_layout()
    {
        static const layout_ref layout = []
        {
            std::vector<control_info> controls(key_code_count);
            for (std::size_t i = 0; i < key_code_count; ++i)
            {
                const auto code = static_cast<key_code>(i);
                controls[i].kind = control_kind::button;
                // key_name() returns "Unknown" for gaps in the HID page; naming them all that way is right, because
                // that is exactly what a binding screen should show for a key it cannot name.
                controls[i].name = key_name(code);
                controls[i].usage = to_usb_hid(code);
            }
            // Slot 0 is key_code::unknown and is never actuated; naming it keeps the array dense and the indexing trivial.
            return make_layout(device_kind::keyboard, std::move(controls));
        }();
        return layout;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Mouse
    // ------------------------------------------------------------------------------------------------------------------

    const layout_ref &mouse_layout()
    {
        static const layout_ref layout = []
        {
            std::vector<control_info> controls(mouse_control_count);

            static constexpr std::array<std::string_view, mouse_button_count> button_names{
                "Left Button", "Right Button", "Middle Button", "Button 4", "Button 5"};
            for (std::size_t i = 0; i < mouse_button_count; ++i)
                controls[mouse_button_control_base + i] = {control_kind::button, button_names[i], usb_hid_unknown};

            struct axis_desc
            {
                mouse_axis axis;
                control_kind kind;
                std::string_view name;
            };
            static constexpr std::array<axis_desc, mouse_axis_count> axes{{
                {mouse_axis::x, control_kind::absolute, "Position X"},
                {mouse_axis::y, control_kind::absolute, "Position Y"},
                {mouse_axis::delta_x, control_kind::delta, "Delta X"},
                {mouse_axis::delta_y, control_kind::delta, "Delta Y"},
                {mouse_axis::wheel_x, control_kind::delta, "Wheel X"},
                {mouse_axis::wheel_y, control_kind::delta, "Wheel Y"},
                {mouse_axis::raw_x, control_kind::delta, "Raw Delta X"},
                {mouse_axis::raw_y, control_kind::delta, "Raw Delta Y"},
            }};
            for (const auto &a : axes)
                controls[control_of(a.axis).index] = {a.kind, a.name, usb_hid_unknown};

            return make_layout(device_kind::mouse, std::move(controls));
        }();
        return layout;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Gamepad
    // ------------------------------------------------------------------------------------------------------------------

    const layout_ref &gamepad_layout()
    {
        static const layout_ref layout = []
        {
            std::vector<control_info> controls(gamepad_control_count);

            static constexpr std::array<std::string_view, gamepad_button_count> button_names{
                "A", "B", "X", "Y", "Back", "Guide", "Start",
                "Left Stick", "Right Stick", "Left Shoulder", "Right Shoulder",
                "D-Pad Up", "D-Pad Down", "D-Pad Left", "D-Pad Right"};
            for (std::size_t i = 0; i < gamepad_button_count; ++i)
                controls[gamepad_button_control_base + i] = {control_kind::button, button_names[i], usb_hid_unknown};

            static constexpr std::array<std::string_view, gamepad_axis_count> axis_names{
                "Left Stick X", "Left Stick Y", "Right Stick X", "Right Stick Y",
                "Left Trigger", "Right Trigger"};
            for (std::size_t i = 0; i < gamepad_axis_count; ++i)
            {
                const auto a = static_cast<gamepad_axis>(i);
                controls[gamepad_axis_control_base + i] = {
                    is_stick_axis(a) ? control_kind::axis : control_kind::ratio, axis_names[i], usb_hid_unknown};
            }

            return make_layout(device_kind::gamepad, std::move(controls));
        }();
        return layout;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Joystick
    // ------------------------------------------------------------------------------------------------------------------

    namespace
    {
        /** @brief The generic joystick control list: every slot present, generically named. */
        std::vector<control_info> build_joystick_controls()
        {
            std::vector<control_info> controls(joystick_control_count);

            for (std::size_t i = 0; i < max_joystick_buttons; ++i)
                controls[joystick_button_control_base + i] = {control_kind::button, numbered("Button ", i + 1), usb_hid_unknown};

            // Centred by default: an axis that rests at an end is re-kinded by the backend when the descriptor says so.
            for (std::size_t i = 0; i < max_joystick_axes; ++i)
                controls[joystick_axis_control_base + i] = {control_kind::axis, numbered("Axis ", i + 1), usb_hid_unknown};

            for (std::size_t i = 0; i < max_joystick_hats; ++i)
            {
                const std::string label = "Hat " + std::to_string(i + 1);
                controls[joystick_hat_control_base + i * 2] = {control_kind::axis, intern(label + " X"), usb_hid_unknown};
                controls[joystick_hat_control_base + i * 2 + 1] = {control_kind::axis, intern(label + " Y"), usb_hid_unknown};
            }

            return controls;
        }
    } // namespace

    hat_direction hat_from_axes(float x, float y) noexcept
    {
        const int ix = (x > 0.5f) ? 1 : (x < -0.5f ? -1 : 0);
        const int iy = (y > 0.5f) ? 1 : (y < -0.5f ? -1 : 0);

        if (ix == 0 && iy == 0)
            return hat_direction::centred;
        if (ix == 0)
            return iy > 0 ? hat_direction::up : hat_direction::down;
        if (iy == 0)
            return ix > 0 ? hat_direction::right : hat_direction::left;
        if (iy > 0)
            return ix > 0 ? hat_direction::up_right : hat_direction::up_left;
        return ix > 0 ? hat_direction::down_right : hat_direction::down_left;
    }

    const layout_ref &default_joystick_layout()
    {
        static const layout_ref layout = make_layout(device_kind::joystick, build_joystick_controls());
        return layout;
    }

    layout_ref make_joystick_layout(std::span<const std::pair<control_id, std::string_view>> named,
                                    std::span<const std::pair<control_id, usb_hid>> usages)
    {
        std::vector<control_info> controls = build_joystick_controls();

        for (const auto &[control, name] : named)
        {
            if (control.valid() && control.index < controls.size() && !name.empty())
                controls[control.index].name = name;
        }
        for (const auto &[control, usage] : usages)
        {
            if (control.valid() && control.index < controls.size())
                controls[control.index].usage = usage;
        }

        return make_layout(device_kind::joystick, std::move(controls));
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Touch
    // ------------------------------------------------------------------------------------------------------------------

    const layout_ref &touch_layout()
    {
        static const layout_ref layout = []
        {
            std::vector<control_info> controls(touch_control_count);

            struct part
            {
                touch_control control;
                control_kind kind;
                std::string_view suffix;
            };
            static constexpr std::array<part, touch_controls_per_point> parts{{
                {touch_control::x, control_kind::absolute, " X"},
                {touch_control::y, control_kind::absolute, " Y"},
                {touch_control::pressure, control_kind::ratio, " Pressure"},
                {touch_control::size, control_kind::ratio, " Size"},
                {touch_control::down, control_kind::button, " Down"},
            }};

            for (std::size_t i = 0; i < max_touch_points; ++i)
            {
                const std::string label = "Touch " + std::to_string(i + 1);
                for (const auto &p : parts)
                {
                    const control_id c = touch_control_of(i, p.control);
                    controls[c.index] = {p.kind, intern(label + std::string(p.suffix)), usb_hid_unknown};
                }
            }

            return make_layout(device_kind::touchscreen, std::move(controls));
        }();
        return layout;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Pen
    // ------------------------------------------------------------------------------------------------------------------

    const layout_ref &pen_layout()
    {
        static const layout_ref layout = []
        {
            std::vector<control_info> controls(pen_control_count);

            static constexpr std::array<std::string_view, pen_button_count> button_names{
                "Tip", "Barrel Button", "Secondary Button", "Eraser"};
            for (std::size_t i = 0; i < pen_button_count; ++i)
                controls[pen_button_control_base + i] = {control_kind::button, button_names[i], usb_hid_unknown};

            struct axis_desc
            {
                pen_axis axis;
                control_kind kind;
                std::string_view name;
            };
            static constexpr std::array<axis_desc, pen_axis_count> axes{{
                {pen_axis::x, control_kind::absolute, "Position X"},
                {pen_axis::y, control_kind::absolute, "Position Y"},
                {pen_axis::pressure, control_kind::ratio, "Pressure"},
                {pen_axis::tilt_x, control_kind::axis, "Tilt X"},
                {pen_axis::tilt_y, control_kind::axis, "Tilt Y"},
                {pen_axis::twist, control_kind::ratio, "Twist"},
                {pen_axis::distance, control_kind::ratio, "Distance"},
            }};
            for (const auto &a : axes)
                controls[control_of(a.axis).index] = {a.kind, a.name, usb_hid_unknown};

            return make_layout(device_kind::pen, std::move(controls));
        }();
        return layout;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // MIDI
    // ------------------------------------------------------------------------------------------------------------------

    std::string_view midi_status_name(midi_status status) noexcept
    {
        switch (status)
        {
        case midi_status::note_off: return "note off";
        case midi_status::note_on: return "note on";
        case midi_status::poly_pressure: return "poly pressure";
        case midi_status::control_change: return "control change";
        case midi_status::program_change: return "program change";
        case midi_status::channel_pressure: return "channel pressure";
        case midi_status::pitch_bend: return "pitch bend";
        case midi_status::system: return "system";
        }
        return "unknown";
    }

    std::string_view midi_note_name(std::uint8_t note) noexcept
    {
        // Built once into a table rather than formatted per call, so the result can be a view with static lifetime and
        // the layout below can point straight at it.
        static const std::array<std::string, midi_note_count> names = []
        {
            static constexpr std::array<const char *, 12> pitches{
                "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
            std::array<std::string, midi_note_count> out{};
            for (std::size_t i = 0; i < midi_note_count; ++i)
            {
                // Middle C (note 60) is C4 in scientific pitch notation, which puts note 0 in octave -1.
                const int octave = static_cast<int>(i) / 12 - 1;
                out[i] = std::string(pitches[i % 12]) + std::to_string(octave);
            }
            return out;
        }();

        return note < midi_note_count ? std::string_view(names[note]) : std::string_view("");
    }

    const layout_ref &midi_layout()
    {
        static const layout_ref layout = []
        {
            std::vector<control_info> controls(midi_control_count);

            for (std::size_t i = 0; i < midi_note_count; ++i)
                controls[midi_note_control_base + i] = {control_kind::ratio, midi_note_name(static_cast<std::uint8_t>(i)), usb_hid_unknown};

            for (std::size_t i = 0; i < midi_controller_count; ++i)
                controls[midi_controller_control_base + i] = {control_kind::ratio, numbered("CC ", i), usb_hid_unknown};

            controls[midi_bend_control] = {control_kind::axis, "Pitch Bend", usb_hid_unknown};
            controls[midi_pressure_control] = {control_kind::ratio, "Channel Pressure", usb_hid_unknown};

            return make_layout(device_kind::midi, std::move(controls));
        }();
        return layout;
    }

} // namespace catalyst::input
