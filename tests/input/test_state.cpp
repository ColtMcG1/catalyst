/**
 * @file test_state.cpp
 * @brief input_state and input::context: the platform feed, frame edges, deltas and focus loss.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "../core/test_common.hpp"

#include <catalyst/events/bus.hpp>
#include <catalyst/input/context.hpp>

#include <string>
#include <vector>

using namespace catalyst::input;
namespace events = catalyst::events;

namespace
{
    constexpr std::uint64_t k_window = 42;

    /** @brief A context driven the way the platform layer drives it, with no hardware in sight. */
    struct fixture
    {
        events::bus bus;
        context in{bus};

        void key(key_code code, button_action action)
        {
            key_event e;
            e.window = k_window;
            e.code = code;
            e.action = action;
            e.modifiers = key_modifiers::none;
            in.feed_key(e);
        }

        void press(key_code code) { key(code, button_action::press); }
        void release(key_code code) { key(code, button_action::release); }
        void repeat(key_code code) { key(code, button_action::repeat); }

        void move(std::int32_t x, std::int32_t y, std::int32_t dx, std::int32_t dy)
        {
            mouse_move_event e;
            e.window = k_window;
            e.position_px = {x, y};
            e.delta_px = {dx, dy};
            in.feed_mouse_move(e);
        }

        void click(mouse_button b, button_action action, std::uint8_t clicks = 1)
        {
            mouse_button_event e;
            e.window = k_window;
            e.button = b;
            e.action = action;
            e.clicks = clicks;
            in.feed_mouse_button(e);
        }
    };

    // ------------------------------------------------------------------------------------------------------------------

    void test_keyboard_levels_and_edges()
    {
        fixture f;
        input_state &s = f.in.state();

        f.in.new_frame();
        CT_REQUIRE(!s.is_key_down(key_code::a));
        CT_REQUIRE(!s.any_key_down());

        f.press(key_code::a);
        CT_REQUIRE(s.is_key_down(key_code::a));
        CT_REQUIRE(s.was_key_pressed(key_code::a));
        CT_REQUIRE(!s.was_key_released(key_code::a));
        CT_REQUIRE(s.any_key_down());
        CT_REQUIRE(s.keys_down_count() == 1);

        // The level persists across the frame boundary; the edge does not.
        f.in.new_frame();
        CT_REQUIRE(s.is_key_down(key_code::a));
        CT_REQUIRE(!s.was_key_pressed(key_code::a));

        f.release(key_code::a);
        CT_REQUIRE(!s.is_key_down(key_code::a));
        CT_REQUIRE(s.was_key_released(key_code::a));
        CT_REQUIRE(s.keys_down_count() == 0);
    }

    void test_a_tap_inside_one_frame_is_not_lost()
    {
        fixture f;
        input_state &s = f.in.state();

        // Edges are accumulated, not diffed against a start-of-frame snapshot. A key tapped and released between two
        // new_frame() calls still registers - at 30fps that is a thing real players do.
        f.in.new_frame();
        f.press(key_code::space);
        f.release(key_code::space);

        CT_REQUIRE(!s.is_key_down(key_code::space));
        CT_REQUIRE(s.was_key_pressed(key_code::space));
        CT_REQUIRE(s.was_key_released(key_code::space));
    }

    void test_auto_repeat()
    {
        fixture f;
        input_state &s = f.in.state();

        f.in.new_frame();
        f.press(key_code::backspace);
        CT_REQUIRE(s.was_key_pressed(key_code::backspace));
        CT_REQUIRE(s.was_key_repeated(key_code::backspace));

        f.in.new_frame();
        f.repeat(key_code::backspace);
        // A repeat is not a press - holding a key must not re-trigger a jump - but it is what text navigation wants.
        CT_REQUIRE(!s.was_key_pressed(key_code::backspace));
        CT_REQUIRE(s.was_key_repeated(key_code::backspace));
        CT_REQUIRE(s.is_key_down(key_code::backspace));
    }

    void test_text_input()
    {
        fixture f;
        input_state &s = f.in.state();

        f.in.new_frame();
        f.in.feed_text(text_input_event(U'h'));
        f.in.feed_text(text_input_event(U"ello"));
        CT_REQUIRE(s.text() == U"hello");

        // Text belongs to a frame; it must not accumulate forever.
        f.in.new_frame();
        CT_REQUIRE(s.text().empty());
    }

    void test_mouse()
    {
        fixture f;
        input_state &s = f.in.state();

        f.in.new_frame();
        f.move(100, 200, 10, 5);
        CT_REQUIRE(s.mouse_position()[0] == 100);
        CT_REQUIRE(s.mouse_position()[1] == 200);
        CT_REQUIRE(s.mouse_delta()[0] == 10);
        CT_REQUIRE(s.mouse_window() == k_window);

        // Several messages in one frame sum, because a camera wants the total and not the last one.
        f.move(112, 208, 12, 8);
        CT_REQUIRE(s.mouse_delta()[0] == 22);
        CT_REQUIRE(s.mouse_delta()[1] == 13);
        CT_REQUIRE(s.mouse_position()[0] == 112);

        // The delta is a property of the frame; the position is not.
        f.in.new_frame();
        CT_REQUIRE(s.mouse_delta()[0] == 0);
        CT_REQUIRE(s.mouse_position()[0] == 112);
    }

    void test_mouse_buttons()
    {
        fixture f;
        input_state &s = f.in.state();

        f.in.new_frame();
        f.click(mouse_button::left, button_action::press);
        CT_REQUIRE(s.is_mouse_button_down(mouse_button::left));
        CT_REQUIRE(s.was_mouse_button_pressed(mouse_button::left));
        CT_REQUIRE(!s.was_mouse_button_double_clicked(mouse_button::left));
        CT_REQUIRE(has_button(s.mouse_buttons_down(), mouse_button::left));

        f.in.new_frame();
        f.click(mouse_button::left, button_action::release);
        f.click(mouse_button::left, button_action::press, 2);
        CT_REQUIRE(s.was_mouse_button_double_clicked(mouse_button::left));
        // A double-click is still a press, so every press stays paired with exactly one release.
        CT_REQUIRE(s.is_mouse_button_down(mouse_button::left));
    }

    void test_wheel_and_raw_motion()
    {
        fixture f;
        input_state &s = f.in.state();

        f.in.new_frame();
        mouse_wheel_event w;
        w.window = k_window;
        w.delta = {0.0f, 1.0f};
        f.in.feed_mouse_wheel(w);
        f.in.feed_mouse_wheel(w);
        CT_REQUIRE(s.wheel_delta()[1] == 2.0f);

        mouse_raw_move_event r;
        r.window = k_window;
        r.delta = {7, -3};
        f.in.feed_mouse_raw_move(r);
        CT_REQUIRE(s.raw_mouse_delta()[0] == 7);
        CT_REQUIRE(s.raw_mouse_delta()[1] == -3);

        f.in.new_frame();
        CT_REQUIRE(s.wheel_delta()[1] == 0.0f);
        CT_REQUIRE(s.raw_mouse_delta()[0] == 0);
    }

    void test_enter_and_leave()
    {
        fixture f;
        input_state &s = f.in.state();

        CT_REQUIRE(!s.mouse_inside());

        mouse_enter_event enter;
        enter.window = k_window;
        enter.position_px = {5, 5};
        f.in.feed_mouse_enter(enter);
        CT_REQUIRE(s.mouse_inside());

        mouse_leave_event leave;
        leave.window = k_window;
        f.in.feed_mouse_leave(leave);
        CT_REQUIRE(!s.mouse_inside());
    }

    void test_focus_loss_releases_everything()
    {
        fixture f;
        input_state &s = f.in.state();

        f.in.new_frame();
        f.press(key_code::w);
        f.press(key_code::left_shift);
        f.click(mouse_button::left, button_action::press);

        // The platform used to keep a per-window bitset just to synthesise these. It only has to say "focus went
        // away" now: the registry already knows what is down.
        std::vector<key_code> released;
        const auto token = f.bus.add_listener<key_event>([&](const key_event &e)
                                                         {
            if (e.action == button_action::release)
                released.push_back(e.code); });

        f.in.feed_focus_lost(k_window);

        CT_REQUIRE(released.size() == 2);
        CT_REQUIRE(!s.is_key_down(key_code::w));
        CT_REQUIRE(!s.is_key_down(key_code::left_shift));
        CT_REQUIRE(!s.is_mouse_button_down(mouse_button::left));
        // The releases are real releases, so edge-driven code unwinds properly rather than silently going quiet.
        CT_REQUIRE(s.was_key_released(key_code::w));
    }

    void test_devices_appear_on_first_use()
    {
        fixture f;

        // Nothing is registered until something actually arrives, so a headless build reports no devices rather than
        // a phantom keyboard.
        CT_REQUIRE(f.in.devices().size() == 0);

        f.press(key_code::a);
        CT_REQUIRE(f.in.devices().size() == 1);
        CT_REQUIRE(f.in.devices().info(f.in.keyboard())->kind == device_kind::keyboard);

        f.move(1, 1, 0, 0);
        CT_REQUIRE(f.in.devices().size() == 2);

        // Asking again returns the same device rather than making another.
        const device_id first = f.in.keyboard();
        f.press(key_code::b);
        CT_REQUIRE(f.in.keyboard() == first);
    }

    void test_touch_contacts_keep_their_slot()
    {
        fixture f;
        input_state &s = f.in.state();

        const auto contact = [&](std::uint32_t id, touch_phase phase, float x, float y)
        {
            touch_event e;
            e.window = k_window;
            e.id = id;
            e.phase = phase;
            e.position_px = {x, y};
            f.in.feed_touch(e);
        };

        f.in.new_frame();
        contact(1000, touch_phase::began, 10.0f, 20.0f);
        contact(1001, touch_phase::began, 30.0f, 40.0f);
        CT_REQUIRE(s.touch_count() == 2);
        CT_REQUIRE(s.is_touch_down(0));
        CT_REQUIRE(s.is_touch_down(1));
        CT_REQUIRE(s.touch_position(0)[0] == 10.0f);

        // A finger keeps the slot it was given, so gesture code following one contact reads the same controls.
        contact(1000, touch_phase::moved, 15.0f, 25.0f);
        CT_REQUIRE(s.touch_position(0)[0] == 15.0f);
        CT_REQUIRE(s.touch_position(1)[0] == 30.0f);

        contact(1000, touch_phase::ended, 15.0f, 25.0f);
        CT_REQUIRE(!s.is_touch_down(0));
        CT_REQUIRE(s.touch_count() == 1);

        // The freed slot is reused by the next finger down.
        contact(1002, touch_phase::began, 50.0f, 60.0f);
        CT_REQUIRE(s.is_touch_down(0));
        CT_REQUIRE(s.touch_position(0)[0] == 50.0f);
    }

    void test_simulated_device_behaves_like_hardware()
    {
        fixture f;
        input_state &s = f.in.state();

        // A replay or an on-screen pad registers a device and writes its controls; nothing above can tell.
        const device_id fake = f.in.add_simulated_device(device_kind::gamepad, gamepad_layout(), "Replay Pad", 0);
        CT_REQUIRE(fake.valid());

        f.in.new_frame();
        f.in.devices().set_value(fake, control_of(gamepad_button::a), 1.0f);
        CT_REQUIRE(s.is_gamepad_connected(0));
        CT_REQUIRE(s.is_gamepad_button_down(0, gamepad_button::a));
        CT_REQUIRE(s.was_gamepad_button_pressed(0, gamepad_button::a));

        f.in.devices().set_value(fake, control_of(gamepad_axis::left_x), 0.75f);
        const gamepad_state g = s.gamepad(0);
        CT_REQUIRE(g.connected);
        CT_REQUIRE(g.is_down(gamepad_button::a));
        CT_REQUIRE(g.axis(gamepad_axis::left_x) > 0.74 && g.axis(gamepad_axis::left_x) < 0.76);
    }

    void test_disconnect_is_reported_for_the_frame()
    {
        fixture f;
        input_state &s = f.in.state();

        // The edge belongs to the frame the device arrived in, so the frame has to be open before it connects.
        f.in.new_frame();
        const device_id fake = f.in.add_simulated_device(device_kind::gamepad, gamepad_layout(), "Replay Pad", 1);
        CT_REQUIRE(s.was_gamepad_connected(1));

        f.in.new_frame();
        CT_REQUIRE(!s.was_gamepad_connected(1));
        f.in.devices().remove_device(fake);
        // The device is gone, so it cannot be looked up by slot - but the frame still has to be able to say it left.
        CT_REQUIRE(s.was_gamepad_disconnected(1));
        CT_REQUIRE(!s.is_gamepad_connected(1));

        f.in.new_frame();
        CT_REQUIRE(!s.was_gamepad_disconnected(1));
    }

    void test_actions_run_through_the_system()
    {
        fixture f;
        using namespace catalyst::input::bind;

        action_map &play = f.in.actions().add_map("gameplay");
        action &move = play.add_axis2d("move");
        move.bind(compose2d().wasd());
        action &jump = play.add_button("jump");
        jump.bind(key(key_code::space));

        f.in.new_frame();
        f.press(key_code::d);
        f.press(key_code::space);
        f.in.poll();
        f.in.update();

        CT_REQUIRE(jump.was_performed());
        CT_REQUIRE(move.vec2()[0] > 0.9f);
    }
} // namespace

int main()
{
    test_keyboard_levels_and_edges();
    test_a_tap_inside_one_frame_is_not_lost();
    test_auto_repeat();
    test_text_input();
    test_mouse();
    test_mouse_buttons();
    test_wheel_and_raw_motion();
    test_enter_and_leave();
    test_focus_loss_releases_everything();
    test_devices_appear_on_first_use();
    test_touch_contacts_keep_their_slot();
    test_simulated_device_behaves_like_hardware();
    test_disconnect_is_reported_for_the_frame();
    test_actions_run_through_the_system();
    return 0;
}
