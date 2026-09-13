/**
 * @file binding.cpp
 * @brief Processor maths and the binding builders.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/input/binding.hpp>

#include <algorithm>
#include <cmath>

namespace catalyst::input
{
    namespace
    {
        /**
         * @brief Maps a magnitude through the dead zone, saturation point and curve, yielding [0, 1].
         * @details One function for both the scalar and the radial case, which is what keeps a stick and a trigger
         * behaving consistently: the same numbers in the options menu mean the same thing to both.
         */
        [[nodiscard]] float shape_magnitude(float magnitude, const processor_chain &p) noexcept
        {
            const float dead = std::clamp(p.deadzone, 0.0f, 0.999f);
            const float sat = std::clamp(p.saturation, dead + 0.001f, 1.0f);

            if (magnitude <= dead)
                return 0.0f;
            if (magnitude >= sat)
                return 1.0f;

            float t = (magnitude - dead) / (sat - dead);
            if (p.curve != 1.0f && p.curve > 0.0f)
                t = std::pow(t, p.curve);
            return t;
        }
    } // namespace

    // ------------------------------------------------------------------------------------------------------------------
    // processor_chain
    // ------------------------------------------------------------------------------------------------------------------

    float processor_chain::apply(float value, bool bounded) const noexcept
    {
        if (!std::isfinite(value))
            return 0.0f;

        float out = value;
        if (bounded)
            out = std::copysign(shape_magnitude(std::fabs(value), *this), value);

        out *= scale;
        return invert ? -out : out;
    }

    void processor_chain::apply_radial(float &x, float &y, bool bounded) const noexcept
    {
        if (!std::isfinite(x) || !std::isfinite(y))
        {
            x = y = 0.0f;
            return;
        }

        const float magnitude = std::sqrt(x * x + y * y);
        if (magnitude <= 0.0f)
        {
            x = y = 0.0f;
            return;
        }

        const float shaped = bounded ? shape_magnitude(magnitude, *this) : magnitude;
        const float k = (shaped / magnitude) * scale * (invert ? -1.0f : 1.0f);
        x *= k;
        y *= k;
    }

    void processor_chain::apply_radial(float &x, float &y, float &z, bool bounded) const noexcept
    {
        if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z))
        {
            x = y = z = 0.0f;
            return;
        }

        const float magnitude = std::sqrt(x * x + y * y + z * z);
        if (magnitude <= 0.0f)
        {
            x = y = z = 0.0f;
            return;
        }

        const float shaped = bounded ? shape_magnitude(magnitude, *this) : magnitude;
        const float k = (shaped / magnitude) * scale * (invert ? -1.0f : 1.0f);
        x *= k;
        y *= k;
        z *= k;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // binding
    // ------------------------------------------------------------------------------------------------------------------

    std::size_t binding::expected_parts() const noexcept
    {
        const std::size_t dims = binding_dimensions(shape);
        if (shape == binding_shape::control)
            return 1;
        // A composite spends one part per direction; an analog binding spends one per dimension.
        return composite ? dims * 2 : dims;
    }

    bool binding::valid() const noexcept
    {
        if (parts.size() < expected_parts())
            return false;
        return std::any_of(parts.begin(), parts.end(), [](const binding_part &p) { return p.control.valid(); });
    }

    // ------------------------------------------------------------------------------------------------------------------
    // binding_builder
    // ------------------------------------------------------------------------------------------------------------------

    binding_builder &binding_builder::slot(std::uint32_t slot) noexcept
    {
        for (binding_part &p : m_binding.parts)
            p.device.slot = slot;
        for (binding_part &p : m_binding.modifiers)
            p.device.slot = slot;
        return *this;
    }

    binding_builder &binding_builder::device(device_id id) noexcept
    {
        for (binding_part &p : m_binding.parts)
            p.device.id = id;
        for (binding_part &p : m_binding.modifiers)
            p.device.id = id;
        return *this;
    }

    binding_builder &binding_builder::deadzone(float threshold) noexcept
    {
        for (binding_part &p : m_binding.parts)
            p.processors.deadzone = threshold;
        return *this;
    }

    binding_builder &binding_builder::saturation(float threshold) noexcept
    {
        for (binding_part &p : m_binding.parts)
            p.processors.saturation = threshold;
        return *this;
    }

    binding_builder &binding_builder::curve(float exponent) noexcept
    {
        for (binding_part &p : m_binding.parts)
            p.processors.curve = exponent;
        return *this;
    }

    binding_builder &binding_builder::scale(float factor) noexcept
    {
        for (binding_part &p : m_binding.parts)
            p.processors.scale = factor;
        return *this;
    }

    binding_builder &binding_builder::invert(bool on) noexcept
    {
        for (binding_part &p : m_binding.parts)
            p.processors.invert = on;
        return *this;
    }

    binding_builder &binding_builder::press_point(float point) noexcept
    {
        for (binding_part &p : m_binding.parts)
            p.processors.press_point = point;
        return *this;
    }

    binding_builder &binding_builder::on_press() noexcept
    {
        m_binding.interact = {interaction_kind::press, {}, 0};
        return *this;
    }

    binding_builder &binding_builder::on_release() noexcept
    {
        m_binding.interact = {interaction_kind::release, {}, 0};
        return *this;
    }

    binding_builder &binding_builder::hold(std::chrono::milliseconds duration) noexcept
    {
        m_binding.interact = {interaction_kind::hold, duration, 0};
        return *this;
    }

    binding_builder &binding_builder::tap(std::chrono::milliseconds max_duration) noexcept
    {
        m_binding.interact = {interaction_kind::tap, max_duration, 0};
        return *this;
    }

    binding_builder &binding_builder::multi_tap(std::uint8_t count, std::chrono::milliseconds window) noexcept
    {
        m_binding.interact = {interaction_kind::multi_tap, window, count};
        return *this;
    }

    binding_builder &binding_builder::with(key_code code)
    {
        return with(device_selector::of(device_kind::keyboard), control_of(code));
    }

    binding_builder &binding_builder::with(gamepad_button button)
    {
        return with(device_selector::of(device_kind::gamepad), control_of(button));
    }

    binding_builder &binding_builder::with(device_selector device, control_id control)
    {
        m_binding.modifiers.push_back(binding_part{device, control, {}});
        return *this;
    }

    binding_builder &binding_builder::as_override(bool on) noexcept
    {
        m_binding.user_override = on;
        return *this;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Builders
    // ------------------------------------------------------------------------------------------------------------------

    namespace bind
    {
        namespace
        {
            /** @brief A one-control binding. */
            [[nodiscard]] binding one(device_kind kind, control_id control)
            {
                binding b;
                b.shape = binding_shape::control;
                b.parts.push_back(binding_part{device_selector::of(kind), control, {}});
                return b;
            }

            /** @brief A two-axis binding from a pair of analog controls. */
            [[nodiscard]] binding two_axis(device_kind kind, control_id x, control_id y)
            {
                binding b;
                b.shape = binding_shape::axis2d;
                b.composite = false;
                b.parts.push_back(binding_part{device_selector::of(kind), x, {}});
                b.parts.push_back(binding_part{device_selector::of(kind), y, {}});
                return b;
            }
        } // namespace

        binding_builder control(device_selector device, control_id control)
        {
            binding b;
            b.shape = binding_shape::control;
            b.parts.push_back(binding_part{device, control, {}});
            return binding_builder{std::move(b)};
        }

        binding_builder key(key_code code)
        {
            return binding_builder{one(device_kind::keyboard, control_of(code))};
        }

        binding_builder mouse(mouse_button button)
        {
            return binding_builder{one(device_kind::mouse, control_of(button))};
        }

        binding_builder pointer(mouse_axis axis)
        {
            return binding_builder{one(device_kind::mouse, control_of(axis))};
        }

        binding_builder mouse_delta()
        {
            return binding_builder{
                two_axis(device_kind::mouse, control_of(mouse_axis::delta_x), control_of(mouse_axis::delta_y))};
        }

        binding_builder mouse_raw_delta()
        {
            return binding_builder{
                two_axis(device_kind::mouse, control_of(mouse_axis::raw_x), control_of(mouse_axis::raw_y))};
        }

        binding_builder wheel()
        {
            return binding_builder{
                two_axis(device_kind::mouse, control_of(mouse_axis::wheel_x), control_of(mouse_axis::wheel_y))};
        }

        binding_builder pad(gamepad_button button)
        {
            return binding_builder{one(device_kind::gamepad, control_of(button))};
        }

        binding_builder pad(gamepad_axis axis)
        {
            return binding_builder{one(device_kind::gamepad, control_of(axis))};
        }

        binding_builder stick(gamepad_axis x, gamepad_axis y)
        {
            return binding_builder{two_axis(device_kind::gamepad, control_of(x), control_of(y))};
        }

        binding_builder left_stick()
        {
            return stick(gamepad_axis::left_x, gamepad_axis::left_y);
        }

        binding_builder right_stick()
        {
            return stick(gamepad_axis::right_x, gamepad_axis::right_y);
        }

        binding_builder dpad()
        {
            return axis2d_builder{}
                .up(gamepad_button::dpad_up)
                .down(gamepad_button::dpad_down)
                .left(gamepad_button::dpad_left)
                .right(gamepad_button::dpad_right)
                .done();
        }

        binding_builder joystick_button(std::size_t n)
        {
            return binding_builder{one(device_kind::joystick, joystick_button_control(n))};
        }

        binding_builder joystick_axis(std::size_t n)
        {
            return binding_builder{one(device_kind::joystick, joystick_axis_control(n))};
        }

        binding_builder joystick_hat(std::size_t n)
        {
            return binding_builder{
                two_axis(device_kind::joystick, joystick_hat_x_control(n), joystick_hat_y_control(n))};
        }

        binding_builder touch(std::size_t index)
        {
            return binding_builder{one(device_kind::touchscreen, touch_control_of(index, touch_control::down))};
        }

        binding_builder touch_position(std::size_t index)
        {
            return binding_builder{two_axis(device_kind::touchscreen,
                                            touch_control_of(index, touch_control::x),
                                            touch_control_of(index, touch_control::y))};
        }

        binding_builder pen(pen_button button)
        {
            return binding_builder{one(device_kind::pen, control_of(button))};
        }

        binding_builder pen(pen_axis axis)
        {
            return binding_builder{one(device_kind::pen, control_of(axis))};
        }

        binding_builder note(std::uint8_t note)
        {
            return binding_builder{one(device_kind::midi, midi_note_control(note))};
        }

        binding_builder cc(std::uint8_t controller)
        {
            return binding_builder{one(device_kind::midi, midi_controller_control(controller))};
        }

        // ---- composites ----

        axis1d_builder::axis1d_builder()
        {
            m_binding.shape = binding_shape::axis1d;
            m_binding.composite = true;
            m_binding.parts.resize(2);
        }

        void axis1d_builder::set(std::size_t index, device_selector device, control_id control)
        {
            m_binding.parts[index] = binding_part{device, control, {}};
        }

        axis1d_builder &axis1d_builder::negative(key_code code)
        {
            set(0, device_selector::of(device_kind::keyboard), control_of(code));
            return *this;
        }

        axis1d_builder &axis1d_builder::negative(gamepad_button button)
        {
            set(0, device_selector::of(device_kind::gamepad), control_of(button));
            return *this;
        }

        axis1d_builder &axis1d_builder::negative(device_selector device, control_id control)
        {
            set(0, device, control);
            return *this;
        }

        axis1d_builder &axis1d_builder::positive(key_code code)
        {
            set(1, device_selector::of(device_kind::keyboard), control_of(code));
            return *this;
        }

        axis1d_builder &axis1d_builder::positive(gamepad_button button)
        {
            set(1, device_selector::of(device_kind::gamepad), control_of(button));
            return *this;
        }

        axis1d_builder &axis1d_builder::positive(device_selector device, control_id control)
        {
            set(1, device, control);
            return *this;
        }

        axis1d_builder::operator binding() const { return m_binding; }
        binding_builder axis1d_builder::done() const { return binding_builder{m_binding}; }

        // Part order for a composite is (negative, positive) per dimension, which read_binding() relies on to compute
        // each axis as positive - negative. For 2D that is x's left and right first, then y's down and up.
        axis2d_builder::axis2d_builder()
        {
            m_binding.shape = binding_shape::axis2d;
            m_binding.composite = true;
            m_binding.parts.resize(4);
        }

        void axis2d_builder::set(std::size_t index, device_selector device, control_id control)
        {
            m_binding.parts[index] = binding_part{device, control, {}};
        }

        axis2d_builder &axis2d_builder::up(key_code code)
        {
            set(3, device_selector::of(device_kind::keyboard), control_of(code));
            return *this;
        }
        axis2d_builder &axis2d_builder::down(key_code code)
        {
            set(2, device_selector::of(device_kind::keyboard), control_of(code));
            return *this;
        }
        axis2d_builder &axis2d_builder::left(key_code code)
        {
            set(0, device_selector::of(device_kind::keyboard), control_of(code));
            return *this;
        }
        axis2d_builder &axis2d_builder::right(key_code code)
        {
            set(1, device_selector::of(device_kind::keyboard), control_of(code));
            return *this;
        }

        axis2d_builder &axis2d_builder::up(gamepad_button button)
        {
            set(3, device_selector::of(device_kind::gamepad), control_of(button));
            return *this;
        }
        axis2d_builder &axis2d_builder::down(gamepad_button button)
        {
            set(2, device_selector::of(device_kind::gamepad), control_of(button));
            return *this;
        }
        axis2d_builder &axis2d_builder::left(gamepad_button button)
        {
            set(0, device_selector::of(device_kind::gamepad), control_of(button));
            return *this;
        }
        axis2d_builder &axis2d_builder::right(gamepad_button button)
        {
            set(1, device_selector::of(device_kind::gamepad), control_of(button));
            return *this;
        }

        axis2d_builder &axis2d_builder::up(device_selector device, control_id control)
        {
            set(3, device, control);
            return *this;
        }
        axis2d_builder &axis2d_builder::down(device_selector device, control_id control)
        {
            set(2, device, control);
            return *this;
        }
        axis2d_builder &axis2d_builder::left(device_selector device, control_id control)
        {
            set(0, device, control);
            return *this;
        }
        axis2d_builder &axis2d_builder::right(device_selector device, control_id control)
        {
            set(1, device, control);
            return *this;
        }

        axis2d_builder &axis2d_builder::arrows()
        {
            return up(key_code::up_arrow).down(key_code::down_arrow).left(key_code::left_arrow).right(key_code::right_arrow);
        }

        axis2d_builder &axis2d_builder::wasd()
        {
            return up(key_code::w).down(key_code::s).left(key_code::a).right(key_code::d);
        }

        axis2d_builder::operator binding() const { return m_binding; }
        binding_builder axis2d_builder::done() const { return binding_builder{m_binding}; }

        axis1d_builder compose1d() { return axis1d_builder{}; }
        axis2d_builder compose2d() { return axis2d_builder{}; }

    } // namespace bind

} // namespace catalyst::input
