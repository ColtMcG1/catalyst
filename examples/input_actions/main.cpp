/*
 * @file main.cpp
 * @brief The Catalyst input module without a window: devices, actions and bindings, driven from the console.
 * @details Deliberately headless, so it exercises the parts of the module that do not depend on the platform layer -
 * gamepad polling, the device registry, the action layer - and runs on a machine with nothing plugged in.
 *
 * It shows the three ways to read input, all of which are live at once:
 *
 *   1. Listening for typed events on the bus (the device connect/disconnect log below).
 *   2. Polling input_state (the raw gamepad readout).
 *   3. Polling actions (everything under "actions"), which is where game code should usually live.
 *
 * The simulated device near the end is worth a look: it registers as a gamepad, and the same bindings pick it up with
 * no special case anywhere. That is what makes a replay, a network peer or an on-screen pad possible.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/events/bus.hpp>
#include <catalyst/input/input.hpp>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace input = catalyst::input;
namespace events = catalyst::events;

using namespace catalyst::input::bind;
using namespace std::chrono_literals;

namespace
{
    const char *phase_name(input::action_phase p)
    {
        switch (p)
        {
        case input::action_phase::started: return "started";
        case input::action_phase::performed: return "performed";
        case input::action_phase::cancelled: return "cancelled";
        case input::action_phase::idle: return "idle";
        }
        return "?";
    }

    /** @brief A little ASCII meter, for showing an axis without a window to draw in. */
    std::string bar(float value, int width = 20)
    {
        const int filled = static_cast<int>(value * static_cast<float>(width) + 0.5f);
        std::string s(1, '[');
        for (int i = 0; i < width; ++i)
            s += (i < filled) ? '#' : '.';
        s += ']';
        return s;
    }
} // namespace

int main()
{
    events::bus bus;
    input::context in(bus);

    std::printf("Catalyst input_actions (backend: %s, %zu gamepad slots)\n\n",
                in.backend_name(), in.gamepad_capacity());

    // ------------------------------------------------------------------------------------------------------------------
    // Actions. This is the layer game code should live at: it names what the player can do, and the bindings below say
    // how - across as many devices as you like, changeable at runtime without any of the code above knowing.
    // ------------------------------------------------------------------------------------------------------------------

    input::action_map &play = in.actions().add_map("gameplay");

    input::action &move = play.add_axis2d("move");
    move.bind(left_stick().deadzone(0.2f));
    move.bind(dpad());

    input::action &look = play.add_axis2d("look");
    look.bind(right_stick().deadzone(0.2f).curve(2.0f)); // a curve gives fine control near centre

    input::action &fire = play.add_button("fire");
    fire.bind(pad(input::gamepad_axis::right_trigger).press_point(0.4f));
    fire.bind(pad(input::gamepad_button::a));

    input::action &sprint = play.add_button("sprint");
    sprint.bind(pad(input::gamepad_button::left_stick).hold(400ms));

    input::action &dodge = play.add_button("dodge");
    dodge.bind(pad(input::gamepad_button::b).multi_tap(2, 300ms));

    input::action &quit = play.add_button("quit");
    quit.bind(pad(input::gamepad_button::start));

    // ------------------------------------------------------------------------------------------------------------------
    // Listeners. Everything the module produces reaches the bus, including devices coming and going.
    // ------------------------------------------------------------------------------------------------------------------

    const auto on_connect = bus.add_listener<input::device_connected_event>(
        [](const input::device_connected_event &e)
        {
            std::printf("+ %s connected (%s, slot %u, %zu controls)\n",
                        e.info.name.c_str(), std::string(input::device_kind_name(e.info.kind)).c_str(),
                        e.info.slot, e.layout ? e.layout->size() : 0);
        });

    const auto on_disconnect = bus.add_listener<input::device_disconnected_event>(
        [](const input::device_disconnected_event &e)
        { std::printf("- %s disconnected\n", e.info.name.c_str()); });

    const auto on_action = bus.add_listener<input::action_event>(
        [](const input::action_event &e)
        {
            // Continuous actions fire every frame they are away from rest; only log the discrete ones.
            if (e.phase == input::action_phase::performed && e.value.kind == input::action_kind::button)
                std::printf("  action %.*s %s (%lld ms)\n", static_cast<int>(e.name.size()), e.name.data(),
                            phase_name(e.phase), static_cast<long long>(e.elapsed.count()));
        });

    std::printf("Plug in a gamepad and try it:\n"
                "  left stick / d-pad   move\n"
                "  right stick          look\n"
                "  A or right trigger   fire\n"
                "  hold L3 400ms        sprint\n"
                "  double-tap B         dodge\n"
                "  Start                quit\n\n"
                "Running for 15 seconds with no pad attached, then demonstrating a simulated one.\n\n");

    // ------------------------------------------------------------------------------------------------------------------
    // The frame. The order is not arbitrary: new_frame() empties this frame's edges and deltas before anything is added
    // to them, and update() runs last so actions see everything that happened rather than most of it.
    // ------------------------------------------------------------------------------------------------------------------

    const auto started = std::chrono::steady_clock::now();
    int frame = 0;

    while (std::chrono::steady_clock::now() - started < 15s)
    {
        in.new_frame();
        in.poll();
        in.update();

        if (quit.was_performed())
        {
            std::printf("\nquit\n");
            break;
        }

        // Once a second, print what the first pad is doing - straight from input_state, no actions involved.
        if (++frame % 60 == 0 && in.state().is_gamepad_connected(0))
        {
            const input::gamepad_state g = in.state().gamepad(0);
            std::printf("  pad0  LX %+.2f LY %+.2f  LT %s RT %s  move %s\n",
                        g.axis(input::gamepad_axis::left_x), g.axis(input::gamepad_axis::left_y),
                        bar(static_cast<float>(g.axis(input::gamepad_axis::left_trigger)), 10).c_str(),
                        bar(static_cast<float>(g.axis(input::gamepad_axis::right_trigger)), 10).c_str(),
                        bar(move.value().magnitude(), 10).c_str());
        }

        if (sprint.was_performed())
            std::printf("  sprinting\n");
        if (dodge.was_performed())
            std::printf("  dodge!\n");
        if (fire.is_held() && frame % 10 == 0)
            std::printf("  firing  %s\n", bar(fire.value().as_float(), 10).c_str());

        std::this_thread::sleep_for(16ms);
    }

    // ------------------------------------------------------------------------------------------------------------------
    // A device the application drives itself. Nothing above changes: the bindings match it, the actions read it, and
    // input_state reports it, because from every layer's point of view it is a gamepad.
    // ------------------------------------------------------------------------------------------------------------------

    std::printf("\nSimulated pad:\n");
    const input::device_id fake =
        in.add_simulated_device(input::device_kind::gamepad, input::gamepad_layout(), "Scripted Pad", 3);

    const auto drive = [&](const char *what, auto &&script)
    {
        in.new_frame();
        script();
        in.poll();
        in.update();
        std::printf("  %-22s move %s  fire %s\n", what,
                    bar(move.value().magnitude(), 10).c_str(),
                    fire.is_held() ? "yes" : "no");
    };

    drive("stick pushed right", [&]
          { in.devices().set_value(fake, input::control_of(input::gamepad_axis::left_x), 1.0f); });
    drive("stick centred", [&]
          { in.devices().set_value(fake, input::control_of(input::gamepad_axis::left_x), 0.0f); });
    drive("trigger pulled", [&]
          { in.devices().set_value(fake, input::control_of(input::gamepad_axis::right_trigger), 0.8f); });
    drive("trigger released", [&]
          { in.devices().set_value(fake, input::control_of(input::gamepad_axis::right_trigger), 0.0f); });

    // Rebinding is the same operation a controls screen performs, and it takes effect on the next frame.
    std::printf("\nRebinding \"fire\" to the left trigger:\n");
    fire.rebind(0, pad(input::gamepad_axis::left_trigger).press_point(0.4f).as_override());

    drive("right trigger pulled", [&]
          { in.devices().set_value(fake, input::control_of(input::gamepad_axis::right_trigger), 0.8f); });
    drive("left trigger pulled", [&]
          { in.devices().set_value(fake, input::control_of(input::gamepad_axis::left_trigger), 0.8f); });

    std::printf("\n%zu device(s) connected at exit\n", in.devices().size());
    return 0;
}
