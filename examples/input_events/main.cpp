/*
 * @file main.cpp
 * @brief Keyboard, text, mouse and gamepad input through a real window, with the Catalyst input module.
 * @details Creates a window, hands the platform layer the input context to feed, and shows all three ways of consuming
 * input at once: listening for typed events on the bus (the console echo), polling `input_state` (the hot keys), and
 * polling actions (movement and the camera). Tab toggles cursor capture, which switches the window to raw mouse motion;
 * H toggles a hidden cursor; Space rumbles the first gamepad; Escape quits.
 *
 * Everything it has to say goes through catalyst::logging, so the echo carries a timestamp and a category and can be
 * sent to a file or a panel by adding a sink rather than by changing any of the code below.
 * License: MIT (see LICENSE).
 */

#include <catalyst/events/bus.hpp>
#include <catalyst/input/input.hpp>
#include <catalyst/logging/logging.hpp>
#include <catalyst/platform/window.hpp>
#include <catalyst/text/utf8.hpp>

#include <chrono>
#include <string>
#include <string_view>
#include <thread>

namespace platform = catalyst::platform;
namespace input = catalyst::input;
namespace events = catalyst::events;
namespace logging = catalyst::logging;
namespace text = catalyst::text;

using namespace catalyst::input::bind;
using namespace std::chrono_literals;

namespace
{
    /** @brief Names this example in the log's category column. */
    struct example_log
    {
        static constexpr const char *name = "input_events";
    };

    std::string modifiers_to_string(input::key_modifiers m)
    {
        using input::has_modifier;
        using input::key_modifiers;

        std::string s;
        if (has_modifier(m, key_modifiers::shift)) s += "shift ";
        if (has_modifier(m, key_modifiers::control)) s += "ctrl ";
        if (has_modifier(m, key_modifiers::alt)) s += "alt ";
        if (has_modifier(m, key_modifiers::super)) s += "super ";
        if (has_modifier(m, key_modifiers::caps_lock)) s += "caps ";
        if (has_modifier(m, key_modifiers::num_lock)) s += "num ";
        if (!s.empty())
            s.pop_back();
        return s.empty() ? "-" : s;
    }

    const char *button_name(input::mouse_button b)
    {
        switch (b)
        {
        case input::mouse_button::left: return "left";
        case input::mouse_button::right: return "right";
        case input::mouse_button::middle: return "middle";
        case input::mouse_button::x1: return "x1";
        case input::mouse_button::x2: return "x2";
        default: return "unknown";
        }
    }

    // Unpadded: the format strings below line the columns up with a width, which is what "{:<7}" is for.
    const char *action_name(input::button_action a)
    {
        switch (a)
        {
        case input::button_action::press: return "press";
        case input::button_action::release: return "release";
        case input::button_action::repeat: return "repeat";
        }
        return "?";
    }
} // namespace

int main()
{
    // One console sink, and every line below reaches the terminal, coloured when the terminal
    // understands colour. Sending the same echo to a file is one more add_sink, and no change here.
    logging::default_logger().add_sink(logging::console_sink{});

    platform::window_desc desc;
    desc.title = "Catalyst - input_events";
    desc.width_px = catalyst::ui::px(800.0f);
    desc.height_px = catalyst::ui::px(450.0f);
    desc.visible = true;

    platform::window w = platform::create_window(desc);
    if (!w)
    {
        logging::critical<example_log>("Failed to create window");
        return 1;
    }

    // One bus carries everything. The input context owns the devices and is what the platform layer feeds - so every
    // input event, whatever produced it, reaches the bus through the same path and the registry can never disagree
    // with the event stream.
    events::bus bus;
    input::context in(bus);
    platform::set_input_feed(&in);
    platform::set_event_bus(&bus); // window events (close, focus) come straight from the platform

    // Actions: what the player can do, rather than which keys are down.
    input::action_map &play = in.actions().add_map("gameplay");
    input::action &move = play.add_axis2d("move");
    move.bind(compose2d().wasd());
    move.bind(left_stick().deadzone(0.2f));

    input::action &look = play.add_axis2d("look");
    look.bind(mouse_raw_delta().scale(0.05f)); // captured cursor, so it keeps reporting at the screen edge
    look.bind(right_stick().deadzone(0.2f));

    logging::info<example_log>("Input example (backend: {}, {} gamepad slots)", in.backend_name(),
                               in.gamepad_capacity());
    logging::info<example_log>("  Escape  quit");
    logging::info<example_log>("  Tab     toggle cursor capture (raw mouse motion)");
    logging::info<example_log>("  H       toggle hidden cursor");
    logging::info<example_log>("  Space   rumble gamepad 0 while held");
    logging::info<example_log>("  C       calibrate gamepad 0's dead zone (then leave the controller alone for a second)");
    logging::info<example_log>("  WASD / left stick moves; mouse / right stick looks.");
    logging::info<example_log>("  Type, click, scroll and move to see events.");

    // C learns a dead zone from gamepad 0: whatever noise the sticks and triggers report while it rests becomes the
    // new threshold, with a little headroom. It runs one frame at a time inside the normal loop.
    input::gamepad_deadzone_calibrator calibrator(0);

    bool running = true;
    const auto sub_close = bus.add_listener<platform::window_close_requested_event>(
        [&](const platform::window_close_requested_event &) { running = false; });

    const auto sub_focus = bus.add_listener<platform::window_focus_event>(
        [](const platform::window_focus_event &e)
        { logging::info<example_log>("focus   {}", e.focused ? "gained" : "lost"); });

    const auto sub_key = bus.add_listener<input::key_event>([](const input::key_event &e)
        {
            logging::info<example_log>("key     {:<7} {:<18} scancode=0x{:03X} mods={}", action_name(e.action),
                                       input::key_name(e.code), e.scancode, modifiers_to_string(e.modifiers));
        });

    const auto sub_text = bus.add_listener<input::text_input_event>([](const input::text_input_event &e)
        { logging::info<example_log>("text    \"{}\"", text::utf8::encode(e.text())); });

    const auto sub_click = bus.add_listener<input::mouse_button_event>([](const input::mouse_button_event &e)
        {
            logging::info<example_log>("mouse   {:<7} {:<7} clicks={} at ({}, {})", action_name(e.action),
                                       button_name(e.button), e.clicks, e.position_px[0], e.position_px[1]);
        });

    const auto sub_wheel = bus.add_listener<input::mouse_wheel_event>([](const input::mouse_wheel_event &e)
        { logging::info<example_log>("wheel   ({:+.2f}, {:+.2f})", e.delta[0], e.delta[1]); });

    // Devices announce themselves generically, so this one listener covers pads, sticks, MIDI - anything.
    const auto sub_device = bus.add_listener<input::device_connected_event>(
        [](const input::device_connected_event &e)
        { logging::info<example_log>("device  + {} ({})", e.info.name, input::device_kind_name(e.info.kind)); });

    const auto sub_gone = bus.add_listener<input::device_disconnected_event>(
        [](const input::device_disconnected_event &e)
        { logging::info<example_log>("device  - {}", e.info.name); });

    bool captured = false;
    bool hidden = false;

    while (running)
    {
        // Order matters: clear this frame's edges and deltas, let the window fill them, poll what has no window, then
        // evaluate the actions against everything that arrived.
        in.new_frame();
        platform::pump_events();
        in.poll();
        in.update();

        input::input_state &state = in.state();

        if (state.was_key_pressed(input::key_code::escape))
            running = false;

        if (state.was_key_pressed(input::key_code::tab))
        {
            captured = !captured;
            platform::set_cursor_mode(w, captured ? platform::cursor_mode::captured : platform::cursor_mode::normal);
            logging::info<example_log>("cursor  {}", captured ? "captured" : "normal");
        }

        if (state.was_key_pressed(input::key_code::h))
        {
            hidden = !hidden;
            platform::set_cursor_mode(w, hidden ? platform::cursor_mode::hidden : platform::cursor_mode::normal);
            logging::info<example_log>("cursor  {}", hidden ? "hidden" : "normal");
        }

        // Rumble follows the space bar, so it stops on its own when the key comes up.
        in.set_rumble(0, {state.is_key_down(input::key_code::space) ? 1.0 : 0.0,
                          state.is_key_down(input::key_code::space) ? 1.0 : 0.0});

        if (state.was_key_pressed(input::key_code::c))
        {
            calibrator.start();
            logging::info<example_log>("calib   started - leave the controller alone");
        }
        if (calibrator.is_sampling())
        {
            const unsigned restarts_before = calibrator.restarts();
            if (calibrator.update(in))
            {
                calibrator.apply(in);
                logging::info<example_log>("calib   done: stick={:.4f} trigger={:.4f}", in.deadzone().stick,
                                           in.deadzone().trigger);
            }
            else if (calibrator.restarts() != restarts_before)
            {
                logging::warn<example_log>("calib   disturbed - let go of the controller");
            }
        }

        // Once per frame while anything is off centre: trace, so the echo above stays readable.
        const auto m = move.vec2();
        const auto l = look.vec2();
        if (m[0] != 0.0f || m[1] != 0.0f || l[0] != 0.0f || l[1] != 0.0f)
            logging::trace<example_log>("actions move ({:+.2f}, {:+.2f})  look ({:+.2f}, {:+.2f})", m[0], m[1], l[0],
                                        l[1]);

        std::this_thread::sleep_for(8ms);
    }

    platform::set_event_bus(nullptr);
    platform::set_input_feed(nullptr);
    platform::destroy_window(w);
    return 0;
}
