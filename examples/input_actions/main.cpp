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
 *
 * Everything it reports goes through catalyst::logging, so the readout carries a level and a category and can be sent
 * somewhere else by adding a sink rather than by changing any of the code below.
 * License: MIT (see LICENSE).
 */

#include <catalyst/events/bus.hpp>
#include <catalyst/input/input.hpp>
#include <catalyst/logging/logging.hpp>

#include <chrono>
#include <string>
#include <thread>

namespace input = catalyst::input;
namespace events = catalyst::events;
namespace logging = catalyst::logging;

using namespace catalyst::input::bind;
using namespace std::chrono_literals;

namespace
{
    /** @brief Names this example in the log's category column. */
    struct example_log
    {
        static constexpr const char *name = "input_actions";
    };

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
    // One console sink, and every line below reaches the terminal, coloured when the terminal
    // understands colour. Sending the same readout to a file is one more add_sink, and no change here.
    logging::default_logger().add_sink(logging::console_sink{});

    events::bus bus;
    input::context in(bus);

    logging::info<example_log>("Catalyst input_actions (backend: {}, {} gamepad slots)", in.backend_name(),
                               in.gamepad_capacity());

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
            logging::info<example_log>("+ {} connected ({}, slot {}, {} controls)", e.info.name,
                                       input::device_kind_name(e.info.kind), e.info.slot,
                                       e.layout ? e.layout->size() : 0);
        });

    const auto on_disconnect = bus.add_listener<input::device_disconnected_event>(
        [](const input::device_disconnected_event &e)
        { logging::info<example_log>("- {} disconnected", e.info.name); });

    const auto on_action = bus.add_listener<input::action_event>(
        [](const input::action_event &e)
        {
            // Continuous actions fire every frame they are away from rest; only log the discrete ones.
            if (e.phase == input::action_phase::performed && e.value.kind == input::action_kind::button)
                logging::info<example_log>("  action {} {} ({} ms)", e.name, phase_name(e.phase),
                                           static_cast<long long>(e.elapsed.count()));
        });

    logging::info<example_log>("Plug in a gamepad and try it:");
    logging::info<example_log>("  left stick / d-pad   move");
    logging::info<example_log>("  right stick          look");
    logging::info<example_log>("  A or right trigger   fire");
    logging::info<example_log>("  hold L3 400ms        sprint");
    logging::info<example_log>("  double-tap B         dodge");
    logging::info<example_log>("  Start                quit");
    logging::info<example_log>("Running for 15 seconds with no pad attached, then demonstrating a simulated one.");

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
            logging::info<example_log>("quit");
            break;
        }

        // Once a second, report what the first pad is doing - straight from input_state, no actions involved. It is a
        // periodic readout rather than an event, so it goes out at trace.
        if (++frame % 60 == 0 && in.state().is_gamepad_connected(0))
        {
            const input::gamepad_state g = in.state().gamepad(0);
            logging::trace<example_log>("  pad0  LX {:+.2f} LY {:+.2f}  LT {} RT {}  move {}",
                                        g.axis(input::gamepad_axis::left_x), g.axis(input::gamepad_axis::left_y),
                                        bar(static_cast<float>(g.axis(input::gamepad_axis::left_trigger)), 10),
                                        bar(static_cast<float>(g.axis(input::gamepad_axis::right_trigger)), 10),
                                        bar(move.value().magnitude(), 10));
        }

        if (sprint.was_performed())
            logging::info<example_log>("  sprinting");
        if (dodge.was_performed())
            logging::info<example_log>("  dodge!");
        if (fire.is_held() && frame % 10 == 0)
            logging::trace<example_log>("  firing  {}", bar(fire.value().as_float(), 10));

        std::this_thread::sleep_for(16ms);
    }

    // ------------------------------------------------------------------------------------------------------------------
    // A device the application drives itself. Nothing above changes: the bindings match it, the actions read it, and
    // input_state reports it, because from every layer's point of view it is a gamepad.
    // ------------------------------------------------------------------------------------------------------------------

    logging::info<example_log>("Simulated pad:");
    const input::device_id fake =
        in.add_simulated_device(input::device_kind::gamepad, input::gamepad_layout(), "Scripted Pad", 3);

    const auto drive = [&](const char *what, auto &&script)
    {
        in.new_frame();
        script();
        in.poll();
        in.update();
        logging::info<example_log>("  {:<22} move {}  fire {}", what, bar(move.value().magnitude(), 10),
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
    logging::info<example_log>("Rebinding \"fire\" to the left trigger:");
    fire.rebind(0, pad(input::gamepad_axis::left_trigger).press_point(0.4f).as_override());

    drive("right trigger pulled", [&]
          { in.devices().set_value(fake, input::control_of(input::gamepad_axis::right_trigger), 0.8f); });
    drive("left trigger pulled", [&]
          { in.devices().set_value(fake, input::control_of(input::gamepad_axis::left_trigger), 0.8f); });

    logging::info<example_log>("{} device(s) connected at exit", in.devices().size());
    return 0;
}
