/**
 * @file test_action.cpp
 * @brief The action layer: binding shapes, composites, processors, interactions, phases and contexts.
 * License: MIT (see LICENSE).
 */

#include "../test_common.hpp"

#include <catalyst/events/bus.hpp>
#include <catalyst/input/action_map.hpp>
#include <catalyst/input/gamepad.hpp>
#include <catalyst/input/keyboard.hpp>
#include <catalyst/input/mouse.hpp>
#include <catalyst/input/registry.hpp>

#include <cmath>
#include <string>
#include <vector>

using namespace catalyst::input;
using namespace catalyst::input::bind;
namespace events = catalyst::events;

using ms = std::chrono::milliseconds;

namespace
{
    [[nodiscard]] bool near(float a, float b, float eps = 1e-4f) noexcept
    {
        return std::fabs(a - b) <= eps;
    }

    /**
     * @brief A registry with a keyboard, a mouse and one pad, plus a clock the test advances by hand.
     * @details Interactions are the only part of the module with memory of time, so every test that touches one drives
     * the clock explicitly rather than sleeping. That makes hold and tap timings exactly reproducible.
     */
    struct fixture
    {
        events::bus bus;
        device_registry reg{bus};
        device_id keyboard;
        device_id mouse;
        device_id pad;
        input_time now{input_clock::now()};

        fixture()
        {
            device_info kb;
            kb.kind = device_kind::keyboard;
            keyboard = reg.add_device(std::move(kb), keyboard_layout());

            device_info m;
            m.kind = device_kind::mouse;
            mouse = reg.add_device(std::move(m), mouse_layout());

            device_info p;
            p.kind = device_kind::gamepad;
            p.slot = 0;
            pad = reg.add_device(std::move(p), gamepad_layout());
        }

        void press(key_code code) { reg.set_value(keyboard, control_of(code), 1.0f); }
        void release(key_code code) { reg.set_value(keyboard, control_of(code), 0.0f); }
        void press(gamepad_button b) { reg.set_value(pad, control_of(b), 1.0f); }
        void release(gamepad_button b) { reg.set_value(pad, control_of(b), 0.0f); }
        void axis(gamepad_axis a, float v) { reg.set_value(pad, control_of(a), v); }

        void advance(ms d) { now += d; }

        void tick(action_map &map)
        {
            map.new_frame();
            map.update(reg, now, &bus);
        }
    };

    // ------------------------------------------------------------------------------------------------------------------

    void test_button_action()
    {
        fixture f;
        action_map map("test");
        action &jump = map.add_button("jump");
        jump.bind(key(key_code::space));

        f.tick(map);
        CT_REQUIRE(!jump.is_held());
        CT_REQUIRE(!jump.was_performed());

        f.press(key_code::space);
        f.tick(map);
        CT_REQUIRE(jump.is_held());
        CT_REQUIRE(jump.was_started());
        CT_REQUIRE(jump.was_performed());

        // A held button performs once, not every frame: "jump" must not fire sixty times a second.
        f.tick(map);
        CT_REQUIRE(jump.is_held());
        CT_REQUIRE(!jump.was_performed());

        f.release(key_code::space);
        f.tick(map);
        CT_REQUIRE(!jump.is_held());
        CT_REQUIRE(!jump.was_performed());
        CT_REQUIRE(jump.was_cancelled());
    }

    void test_several_bindings_on_one_action()
    {
        fixture f;
        action_map map("test");
        action &jump = map.add_button("jump");
        jump.bind(key(key_code::space));
        jump.bind(pad(gamepad_button::a));

        // Either device drives it, which is the whole point of the layer.
        f.press(gamepad_button::a);
        f.tick(map);
        CT_REQUIRE(jump.was_performed());
        CT_REQUIRE(jump.source_device() == f.pad);

        f.release(gamepad_button::a);
        f.tick(map);

        f.press(key_code::space);
        f.tick(map);
        CT_REQUIRE(jump.was_performed());
        CT_REQUIRE(jump.source_device() == f.keyboard);
    }

    void test_axis2d_from_a_stick()
    {
        fixture f;
        action_map map("test");
        action &move = map.add_axis2d("move");
        move.bind(stick(gamepad_axis::left_x, gamepad_axis::left_y));

        f.axis(gamepad_axis::left_x, 1.0f);
        f.tick(map);
        CT_REQUIRE(near(move.vec2()[0], 1.0f));
        CT_REQUIRE(near(move.vec2()[1], 0.0f));
        CT_REQUIRE(move.is_held());

        f.axis(gamepad_axis::left_x, 0.0f);
        f.axis(gamepad_axis::left_y, -0.5f);
        f.tick(map);
        CT_REQUIRE(near(move.vec2()[1], -0.5f));
    }

    void test_axis2d_from_wasd()
    {
        fixture f;
        action_map map("test");
        action &move = map.add_axis2d("move");
        move.bind(compose2d().wasd());

        f.press(key_code::d);
        f.tick(map);
        CT_REQUIRE(near(move.vec2()[0], 1.0f));
        CT_REQUIRE(near(move.vec2()[1], 0.0f));

        f.press(key_code::w);
        f.tick(map);
        // Diagonals are normalised radially, so moving north-east is not faster than moving east.
        CT_REQUIRE(near(move.value().magnitude(), 1.0f, 1e-3f));

        // Opposite keys cancel rather than fighting.
        f.press(key_code::a);
        f.release(key_code::w);
        f.tick(map);
        CT_REQUIRE(near(move.vec2()[0], 0.0f));
    }

    void test_axis1d_composite()
    {
        fixture f;
        action_map map("test");
        action &turn = map.add_axis1d("turn");
        turn.bind(compose1d().negative(key_code::q).positive(key_code::e));

        f.tick(map);
        CT_REQUIRE(near(turn.as_float(), 0.0f));

        f.press(key_code::e);
        f.tick(map);
        CT_REQUIRE(near(turn.as_float(), 1.0f));

        f.press(key_code::q);
        f.tick(map);
        CT_REQUIRE(near(turn.as_float(), 0.0f));

        f.release(key_code::e);
        f.tick(map);
        CT_REQUIRE(near(turn.as_float(), -1.0f));
    }

    void test_processors()
    {
        fixture f;
        action_map map("test");

        action &scaled = map.add_axis1d("scaled");
        scaled.bind(pad(gamepad_axis::left_x).scale(2.0f));

        action &inverted = map.add_axis1d("inverted");
        inverted.bind(pad(gamepad_axis::left_x).invert());

        action &dead = map.add_axis1d("dead");
        dead.bind(pad(gamepad_axis::left_x).deadzone(0.5f));

        f.axis(gamepad_axis::left_x, 0.4f);
        f.tick(map);
        CT_REQUIRE(near(scaled.as_float(), 0.8f));
        CT_REQUIRE(near(inverted.as_float(), -0.4f));
        // Inside the dead zone, so nothing at all - not a small value the player did not ask for.
        CT_REQUIRE(near(dead.as_float(), 0.0f));

        f.axis(gamepad_axis::left_x, 1.0f);
        f.tick(map);
        // Past the dead zone the remainder is rescaled, so full deflection still reads 1.
        CT_REQUIRE(near(dead.as_float(), 1.0f));
    }

    void test_trigger_as_a_button()
    {
        fixture f;
        action_map map("test");
        action &fire = map.add_button("fire");
        fire.bind(pad(gamepad_axis::right_trigger).press_point(0.4f));

        f.axis(gamepad_axis::right_trigger, 0.3f);
        f.tick(map);
        CT_REQUIRE(!fire.is_held());

        f.axis(gamepad_axis::right_trigger, 0.5f);
        f.tick(map);
        CT_REQUIRE(fire.is_held());
        CT_REQUIRE(fire.was_performed());
    }

    void test_hold_interaction()
    {
        fixture f;
        action_map map("test");
        action &revive = map.add_button("revive");
        revive.bind(key(key_code::f).hold(ms{500}));

        f.press(key_code::f);
        f.tick(map);
        CT_REQUIRE(revive.was_started());
        CT_REQUIRE(!revive.was_performed()); // not yet - that is the point of a hold

        f.advance(ms{200});
        f.tick(map);
        CT_REQUIRE(!revive.was_performed());

        f.advance(ms{400}); // 600ms total
        f.tick(map);
        CT_REQUIRE(revive.was_performed());

        // It performs once, not every frame it stays held.
        f.advance(ms{100});
        f.tick(map);
        CT_REQUIRE(!revive.was_performed());
    }

    void test_hold_cancelled_when_released_early()
    {
        fixture f;
        action_map map("test");
        action &revive = map.add_button("revive");
        revive.bind(key(key_code::f).hold(ms{500}));

        f.press(key_code::f);
        f.tick(map);
        f.advance(ms{100});
        f.release(key_code::f);
        f.tick(map);

        // Letting go early is the player changing their mind, which is a cancellation and not a very short hold.
        CT_REQUIRE(revive.was_cancelled());
        CT_REQUIRE(!revive.was_performed());
    }

    void test_tap_interaction()
    {
        fixture f;
        action_map map("test");
        action &dodge = map.add_button("dodge");
        dodge.bind(key(key_code::space).tap(ms{200}));

        f.press(key_code::space);
        f.tick(map);
        f.advance(ms{50});
        f.release(key_code::space);
        f.tick(map);
        CT_REQUIRE(dodge.was_performed());

        // Held too long: a tap it is not.
        f.press(key_code::space);
        f.tick(map);
        f.advance(ms{400});
        f.release(key_code::space);
        f.tick(map);
        CT_REQUIRE(!dodge.was_performed());
        CT_REQUIRE(dodge.was_cancelled());
    }

    void test_multi_tap_interaction()
    {
        fixture f;
        action_map map("test");
        action &dash = map.add_button("dash");
        dash.bind(key(key_code::w).multi_tap(2, ms{300}));

        f.press(key_code::w);
        f.tick(map);
        CT_REQUIRE(!dash.was_performed());
        f.release(key_code::w);
        f.tick(map);

        f.advance(ms{100});
        f.press(key_code::w);
        f.tick(map);
        CT_REQUIRE(dash.was_performed());

        f.release(key_code::w);
        f.tick(map);

        // Too slow the second time: that is two separate taps, not a double.
        f.press(key_code::w);
        f.tick(map);
        f.release(key_code::w);
        f.tick(map);
        f.advance(ms{1000});
        f.press(key_code::w);
        f.tick(map);
        CT_REQUIRE(!dash.was_performed());
    }

    void test_release_interaction()
    {
        fixture f;
        action_map map("test");
        action &throw_grenade = map.add_button("throw");
        throw_grenade.bind(key(key_code::g).on_release());

        f.press(key_code::g);
        f.tick(map);
        CT_REQUIRE(throw_grenade.was_started());
        CT_REQUIRE(!throw_grenade.was_performed());

        f.release(key_code::g);
        f.tick(map);
        CT_REQUIRE(throw_grenade.was_performed());
    }

    void test_chords_suppress_the_plain_binding()
    {
        fixture f;
        action_map map("test");
        action &back = map.add_button("move_back");
        back.bind(key(key_code::s));
        action &save = map.add_button("quicksave");
        save.bind(key(key_code::s).with(key_code::left_control));

        // S alone walks backwards.
        f.press(key_code::s);
        f.tick(map);
        CT_REQUIRE(back.was_performed());
        CT_REQUIRE(!save.was_performed());

        f.release(key_code::s);
        f.tick(map);

        // Ctrl+S saves, and does *not* also walk backwards - which is what the player meant.
        f.press(key_code::left_control);
        f.press(key_code::s);
        f.tick(map);
        CT_REQUIRE(save.was_performed());
        CT_REQUIRE(!back.was_performed());
        CT_REQUIRE(!back.is_held());
    }

    void test_disabled_map_is_silent()
    {
        fixture f;
        action_map map("test");
        action &jump = map.add_button("jump");
        jump.bind(key(key_code::space));

        map.disable();
        f.press(key_code::space);
        f.tick(map);
        CT_REQUIRE(!jump.was_performed());
        CT_REQUIRE(!jump.is_held());

        map.enable();
        f.tick(map);
        CT_REQUIRE(jump.is_held());
    }

    void test_contexts()
    {
        fixture f;
        action_set set;
        action_map &play = set.add_map("gameplay");
        action_map &menu = set.add_map("menu");

        action &fire = play.add_button("fire");
        fire.bind(mouse(mouse_button::left));
        action &confirm = menu.add_button("confirm");
        confirm.bind(mouse(mouse_button::left));

        set.enable_only("gameplay");
        f.reg.set_value(f.mouse, control_of(mouse_button::left), 1.0f);
        set.new_frame();
        set.update(f.reg, f.now, &f.bus);
        CT_REQUIRE(fire.was_performed());
        CT_REQUIRE(!confirm.was_performed());

        // Pausing: the same click now means confirm, and gameplay hears nothing. No `if (paused)` anywhere.
        f.reg.set_value(f.mouse, control_of(mouse_button::left), 0.0f);
        set.new_frame();
        set.update(f.reg, f.now, &f.bus);

        set.enable_only("menu");
        f.reg.set_value(f.mouse, control_of(mouse_button::left), 1.0f);
        set.new_frame();
        set.update(f.reg, f.now, &f.bus);
        CT_REQUIRE(confirm.was_performed());
        CT_REQUIRE(!fire.was_performed());

        CT_REQUIRE(set.find_action("gameplay/fire") == &fire);
        CT_REQUIRE(set.find_action("confirm") == &confirm);
        CT_REQUIRE(set.find_action("gameplay/nope") == nullptr);
    }

    void test_action_events()
    {
        fixture f;
        action_map map("gameplay");
        action &jump = map.add_button("jump");
        jump.bind(key(key_code::space));

        std::vector<std::pair<std::string, action_phase>> seen;
        const auto token = f.bus.add_listener<action_event>([&](const action_event &e)
                                                            { seen.emplace_back(std::string(e.name), e.phase); });

        f.press(key_code::space);
        f.tick(map);

        CT_REQUIRE(seen.size() == 2);
        CT_REQUIRE(seen[0].first == "jump");
        CT_REQUIRE(seen[0].second == action_phase::started);
        CT_REQUIRE(seen[1].second == action_phase::performed);

        seen.clear();
        f.release(key_code::space);
        f.tick(map);
        CT_REQUIRE(seen.size() == 1);
        CT_REQUIRE(seen[0].second == action_phase::cancelled);
    }

    void test_rebinding()
    {
        fixture f;
        action_map map("test");
        action &jump = map.add_button("jump");
        jump.bind(key(key_code::space));
        CT_REQUIRE(jump.bindings().size() == 1);

        // What a bindings screen does: swap one binding out and leave everything else about the action alone.
        CT_REQUIRE(jump.rebind(0, key(key_code::j).as_override()));
        CT_REQUIRE(jump.bindings()[0].user_override);
        CT_REQUIRE(!jump.rebind(5, key(key_code::k)));

        f.press(key_code::space);
        f.tick(map);
        CT_REQUIRE(!jump.was_performed());

        f.press(key_code::j);
        f.tick(map);
        CT_REQUIRE(jump.was_performed());

        CT_REQUIRE(jump.unbind(0));
        CT_REQUIRE(jump.bindings().empty());
    }

    void test_slot_narrows_to_one_device()
    {
        fixture f;
        device_info second;
        second.kind = device_kind::gamepad;
        second.slot = 1;
        const device_id pad1 = f.reg.add_device(std::move(second), gamepad_layout());

        action_map map("test");
        action &p2_jump = map.add_button("p2_jump");
        p2_jump.bind(pad(gamepad_button::a).slot(1));

        f.press(gamepad_button::a); // slot 0
        f.tick(map);
        CT_REQUIRE(!p2_jump.was_performed());

        f.reg.set_value(pad1, control_of(gamepad_button::a), 1.0f);
        f.tick(map);
        CT_REQUIRE(p2_jump.was_performed());
    }

    void test_binding_shape_adapts_to_action_kind()
    {
        fixture f;
        action_map map("test");

        // A 2D binding on a button action reads as its magnitude, rather than silently producing nothing - a config
        // file should not be able to disable an action by naming the wrong shape.
        action &moving = map.add_button("moving");
        moving.bind(stick(gamepad_axis::left_x, gamepad_axis::left_y));

        f.axis(gamepad_axis::left_y, 1.0f);
        f.tick(map);
        CT_REQUIRE(moving.is_held());
        CT_REQUIRE(near(moving.value().as_float(), 1.0f));
    }

    void test_add_is_idempotent_and_typed()
    {
        action_map map("test");
        action &a = map.add_button("jump");
        action &b = map.add_button("jump");
        CT_REQUIRE(&a == &b);

        bool threw = false;
        try
        {
            map.add_axis2d("jump");
        }
        catch (const std::invalid_argument &)
        {
            threw = true;
        }
        // Reusing a name with a different kind is a mistake, and a silent reshape would be found much later.
        CT_REQUIRE(threw);

        CT_REQUIRE(map.find("jump") == &a);
        CT_REQUIRE(map.find("nope") == nullptr);
    }
} // namespace

int main()
{
    test_button_action();
    test_several_bindings_on_one_action();
    test_axis2d_from_a_stick();
    test_axis2d_from_wasd();
    test_axis1d_composite();
    test_processors();
    test_trigger_as_a_button();
    test_hold_interaction();
    test_hold_cancelled_when_released_early();
    test_tap_interaction();
    test_multi_tap_interaction();
    test_release_interaction();
    test_chords_suppress_the_plain_binding();
    test_disabled_map_is_silent();
    test_contexts();
    test_action_events();
    test_rebinding();
    test_slot_narrows_to_one_device();
    test_binding_shape_adapts_to_action_kind();
    test_add_is_idempotent_and_typed();
    return 0;
}
