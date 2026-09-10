/**
 * @file test_gamepad.cpp
 * @brief Gamepad vocabulary, the dead-zone maths, and the calibrator.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "../core/test_common.hpp"

#include <catalyst/events/bus.hpp>
#include <catalyst/input/calibration.hpp>
#include <catalyst/input/context.hpp>
#include <catalyst/input/gamepad.hpp>

#include <chrono>
#include <cmath>

using namespace catalyst::input;
namespace events = catalyst::events;

using clock_type = gamepad_deadzone_calibrator::clock;
using ms = std::chrono::milliseconds;

namespace
{
    [[nodiscard]] bool near(double a, double b, double eps = 1e-9) noexcept
    {
        return std::fabs(a - b) <= eps;
    }

    /** @brief A resting controller reporting @p stick on both sticks and @p trigger on both triggers. */
    [[nodiscard]] gamepad_state resting(double stick = 0.0, double trigger = 0.0) noexcept
    {
        gamepad_state s{};
        s.connected = true;
        s.axes[static_cast<std::size_t>(gamepad_axis::left_x)] = stick;
        s.axes[static_cast<std::size_t>(gamepad_axis::right_x)] = stick;
        s.axes[static_cast<std::size_t>(gamepad_axis::left_trigger)] = trigger;
        s.axes[static_cast<std::size_t>(gamepad_axis::right_trigger)] = trigger;
        return s;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Vocabulary
    // ------------------------------------------------------------------------------------------------------------------

    void test_button_sets()
    {
        gamepad_buttons set = gamepad_buttons::none;
        CT_REQUIRE(!has_button(set, gamepad_button::a));

        set |= to_gamepad_buttons(gamepad_button::a);
        set |= to_gamepad_buttons(gamepad_button::dpad_up);
        CT_REQUIRE(has_button(set, gamepad_button::a));
        CT_REQUIRE(has_button(set, gamepad_button::dpad_up));
        CT_REQUIRE(!has_button(set, gamepad_button::b));

        // Every enumerator has its own bit, and the bit index is the enumerator's value.
        for (std::size_t i = 0; i < gamepad_button_count; ++i)
        {
            const auto b = static_cast<gamepad_button>(i);
            CT_REQUIRE(static_cast<std::uint16_t>(to_gamepad_buttons(b)) == (1u << i));
        }

        set &= ~to_gamepad_buttons(gamepad_button::a);
        CT_REQUIRE(!has_button(set, gamepad_button::a));
        CT_REQUIRE(has_button(set, gamepad_button::dpad_up));
    }

    void test_controls()
    {
        CT_REQUIRE(control_of(gamepad_button::a).index == 0);
        CT_REQUIRE(control_of(gamepad_axis::left_x).index == gamepad_button_count);
        CT_REQUIRE(control_of(gamepad_axis::right_trigger).index == gamepad_control_count - 1);

        gamepad_button b{};
        CT_REQUIRE(gamepad_button_of(control_of(gamepad_button::y), b));
        CT_REQUIRE(b == gamepad_button::y);
        CT_REQUIRE(!gamepad_button_of(control_of(gamepad_axis::left_x), b));

        gamepad_axis a{};
        CT_REQUIRE(gamepad_axis_of(control_of(gamepad_axis::right_y), a));
        CT_REQUIRE(a == gamepad_axis::right_y);
        CT_REQUIRE(!gamepad_axis_of(control_of(gamepad_button::a), a));

        // A stick swings both ways; a trigger only one. The binding layer needs the difference to apply a dead zone.
        const layout_ref &layout = gamepad_layout();
        CT_REQUIRE(layout->size() == gamepad_control_count);
        CT_REQUIRE(layout->kind_of(control_of(gamepad_axis::left_x)) == control_kind::axis);
        CT_REQUIRE(layout->kind_of(control_of(gamepad_axis::left_trigger)) == control_kind::ratio);
        CT_REQUIRE(layout->kind_of(control_of(gamepad_button::a)) == control_kind::button);
        CT_REQUIRE(layout->name_of(control_of(gamepad_axis::left_x)) == "Left Stick X");
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Dead zones
    // ------------------------------------------------------------------------------------------------------------------

    void test_linear_deadzone()
    {
        CT_REQUIRE(near(apply_deadzone(0.0, 0.25), 0.0));
        CT_REQUIRE(near(apply_deadzone(0.25, 0.25), 0.0));
        CT_REQUIRE(near(apply_deadzone(-0.25, 0.25), 0.0));

        // Just past the threshold the output starts at 0 rather than jumping to 0.25.
        CT_REQUIRE(apply_deadzone(0.26, 0.25) > 0.0);
        CT_REQUIRE(apply_deadzone(0.26, 0.25) < 0.05);

        // Full deflection still reaches 1 either way.
        CT_REQUIRE(near(apply_deadzone(1.0, 0.25), 1.0));
        CT_REQUIRE(near(apply_deadzone(-1.0, 0.25), -1.0));

        // Halfway through the live range reads a half.
        CT_REQUIRE(near(apply_deadzone(0.625, 0.25), 0.5));

        // A zero dead zone is the identity, and a threshold of 1 is clamped rather than dividing by zero.
        CT_REQUIRE(near(apply_deadzone(0.3, 0.0), 0.3));
        CT_REQUIRE(std::isfinite(apply_deadzone(1.0, 1.0)));
    }

    void test_radial_deadzone()
    {
        double x = 0.0;
        double y = 0.0;
        apply_radial_deadzone(x, y, 0.25);
        CT_REQUIRE(near(x, 0.0) && near(y, 0.0));

        // Inside the circle both components go, not just the smaller one.
        x = 0.2;
        y = 0.1;
        apply_radial_deadzone(x, y, 0.25);
        CT_REQUIRE(near(x, 0.0) && near(y, 0.0));

        // Outside it, the direction survives - which is the whole reason for doing this radially.
        x = 0.8;
        y = 0.6; // magnitude 1
        apply_radial_deadzone(x, y, 0.25);
        CT_REQUIRE(near(std::sqrt(x * x + y * y), 1.0, 1e-9));
        CT_REQUIRE(near(y / x, 0.6 / 0.8, 1e-9));
    }

    void test_deadzone_defaults()
    {
        const gamepad_deadzone dz;
        // Microsoft's published XInput values, which are the right default for the hardware most players own.
        CT_REQUIRE(near(dz.stick, 7849.0 / 32767.0));
        CT_REQUIRE(near(dz.trigger, 30.0 / 255.0));
    }

    void test_deadzone_settings_are_clamped()
    {
        events::bus bus;
        context in(bus);

        in.set_deadzone({0.5, 2.0});
        const gamepad_deadzone after = in.deadzone();
        CT_REQUIRE(near(after.stick, 0.5));
        // A threshold of 1 or more would leave no live range at all, so it is clamped just short.
        CT_REQUIRE(after.trigger < 1.0);
        CT_REQUIRE(after.trigger > 0.9);
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Calibration
    // ------------------------------------------------------------------------------------------------------------------

    void test_deadzone_for_noise()
    {
        gamepad_deadzone_calibration_options opts;
        opts.headroom = 0.25;
        opts.margin = 0.02;

        const gamepad_deadzone dz = deadzone_for_noise(0.08, 0.04, opts);
        CT_REQUIRE(near(dz.stick, 0.08 * 1.25 + 0.02));
        CT_REQUIRE(near(dz.trigger, 0.04 * 1.25 + 0.02));

        // A controller with no measurable noise still gets the margin, not a zero threshold.
        const gamepad_deadzone clean = deadzone_for_noise(0.0, 0.0, opts);
        CT_REQUIRE(near(clean.stick, 0.02));

        // Nonsense in - a driver reporting NaN, or a peak past full deflection - stays inside a usable range.
        const gamepad_deadzone absurd = deadzone_for_noise(50.0, std::nan(""), opts);
        CT_REQUIRE(absurd.stick < 1.0 && absurd.stick >= 0.0);
        CT_REQUIRE(std::isfinite(absurd.trigger));
    }

    void test_calibrator_learns_noise()
    {
        gamepad_deadzone_calibration_options opts;
        opts.duration = ms{100};
        gamepad_deadzone_calibrator cal(0, opts);

        auto now = clock_type::now();
        cal.start(now);
        CT_REQUIRE(cal.is_sampling());
        CT_REQUIRE(cal.progress() == 0.0);

        // A resting controller that jitters a little; the largest jitter is the noise floor.
        for (int i = 0; i < 5; ++i)
        {
            now += ms{10};
            CT_REQUIRE(!cal.sample(resting(0.03 + i * 0.01, 0.01), now));
        }
        CT_REQUIRE(cal.progress() > 0.0 && cal.progress() < 1.0);

        now += ms{100};
        CT_REQUIRE(cal.sample(resting(0.03, 0.01), now));
        CT_REQUIRE(cal.is_complete());
        CT_REQUIRE(cal.progress() == 1.0);
        CT_REQUIRE(near(cal.peak_stick_noise(), 0.07, 1e-9));
        CT_REQUIRE(cal.restarts() == 0);

        const gamepad_deadzone result = cal.result();
        CT_REQUIRE(result.stick > 0.07);
        CT_REQUIRE(result.stick < 0.2);
    }

    void test_calibrator_restarts_when_disturbed()
    {
        gamepad_deadzone_calibration_options opts;
        opts.duration = ms{100};
        opts.disturbance_threshold = 0.5;
        gamepad_deadzone_calibrator cal(0, opts);

        auto now = clock_type::now();
        cal.start(now);

        now += ms{50};
        CT_REQUIRE(!cal.sample(resting(0.02), now));

        // The user grabbed the stick: the window starts over, and the peak they just caused is not kept.
        now += ms{10};
        CT_REQUIRE(!cal.sample(resting(0.9), now));
        CT_REQUIRE(cal.restarts() == 1);
        CT_REQUIRE(near(cal.peak_stick_noise(), 0.0));

        // A held button counts as a disturbance too, whatever the sticks say.
        gamepad_state pressed = resting(0.01);
        pressed.buttons = gamepad_buttons::a;
        now += ms{10};
        CT_REQUIRE(!cal.sample(pressed, now));
        CT_REQUIRE(cal.restarts() == 2);

        now += ms{200};
        CT_REQUIRE(cal.sample(resting(0.01), now));
        CT_REQUIRE(cal.is_complete());
    }

    void test_calibrator_handles_disconnect()
    {
        gamepad_deadzone_calibrator cal(0);
        auto now = clock_type::now();
        cal.start(now);

        gamepad_state gone{};
        gone.connected = false;
        now += ms{10};
        CT_REQUIRE(!cal.sample(gone, now));
        CT_REQUIRE(cal.status() == gamepad_calibration_status::disconnected);
        CT_REQUIRE(!cal.is_sampling());
        CT_REQUIRE(!cal.is_complete());

        // It can be restarted once the controller comes back.
        cal.start(now);
        CT_REQUIRE(cal.is_sampling());
    }

    void test_calibrator_lifecycle()
    {
        gamepad_deadzone_calibrator cal(2);
        CT_REQUIRE(cal.slot() == 2);
        CT_REQUIRE(cal.status() == gamepad_calibration_status::idle);
        CT_REQUIRE(cal.progress() == 0.0);

        // An idle calibrator ignores samples rather than quietly accumulating them.
        CT_REQUIRE(!cal.sample(resting(0.5), clock_type::now()));
        CT_REQUIRE(cal.status() == gamepad_calibration_status::idle);

        cal.start();
        CT_REQUIRE(cal.is_sampling());
        cal.cancel();
        CT_REQUIRE(cal.status() == gamepad_calibration_status::idle);
    }

    void test_calibrator_applies_to_a_context()
    {
        events::bus bus;
        context in(bus);

        gamepad_deadzone_calibration_options opts;
        opts.duration = ms{10};
        gamepad_deadzone_calibrator cal(0, opts);

        // Nothing is installed until the calibration finishes.
        const gamepad_deadzone before = in.deadzone();
        CT_REQUIRE(!cal.apply(in));
        CT_REQUIRE(near(in.deadzone().stick, before.stick));

        auto now = clock_type::now();
        cal.start(now);
        now += ms{50};
        CT_REQUIRE(cal.sample(resting(0.05, 0.02), now));
        CT_REQUIRE(cal.apply(in));
        CT_REQUIRE(near(in.deadzone().stick, cal.result().stick));
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Backend
    // ------------------------------------------------------------------------------------------------------------------

    void test_backend_queries_are_safe()
    {
        events::bus bus;
        context in(bus);

        CT_REQUIRE(in.gamepad_capacity() <= max_gamepads);
        CT_REQUIRE(in.backend_name() != nullptr);

        // Out-of-range slots are answered, not crashed on, whether or not hardware is attached.
        CT_REQUIRE(!in.raw_gamepad(static_cast<std::uint32_t>(max_gamepads)).connected);
        CT_REQUIRE(!in.raw_gamepad(9999).connected);
        CT_REQUIRE(!in.set_rumble(static_cast<std::uint32_t>(max_gamepads), {1.0, 1.0}));
        CT_REQUIRE(!in.set_rumble(no_device, {1.0, 1.0}));

        // Polling with nothing plugged in is a no-op, not a fault.
        in.new_frame();
        in.poll();
        in.update();
    }
} // namespace

int main()
{
    test_button_sets();
    test_controls();
    test_linear_deadzone();
    test_radial_deadzone();
    test_deadzone_defaults();
    test_deadzone_settings_are_clamped();
    test_deadzone_for_noise();
    test_calibrator_learns_noise();
    test_calibrator_restarts_when_disturbed();
    test_calibrator_handles_disconnect();
    test_calibrator_lifecycle();
    test_calibrator_applies_to_a_context();
    test_backend_queries_are_safe();
    return 0;
}
