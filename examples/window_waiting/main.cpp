/*
 * @file main.cpp
 * @brief Example of using the Catalyst platform library to create a window and wait for events.
 * @details This example demonstrates how to initialize the Catalyst platform library, create a window, install an event
 * bus, and enter a main loop that waits for events. The example handles window close requests and resize events, printing
 * relevant information to the console. It uses wait_events with an infinite timeout to block until events are available,
 * followed by pump_events to process them. This serves as a basic template for using the Catalyst platform library in
 * applications that require window management and event handling with an event-driven approach.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/catalyst.hpp>
#include <catalyst/events/bus.hpp>
#include <catalyst/platform/window.hpp>

#include <cstdio>

int main()
{
  catalyst::catalyst_version_anchor();

  using namespace catalyst::platform;

  window_desc desc;
  desc.title = "Catalyst - window_waiting";
  desc.width_px = catalyst::ui::px(800.0f);
  desc.height_px = catalyst::ui::px(450.0f);
  desc.visible = true;

  window w = create_window(desc);
  if (!w)
  {
    std::fprintf(stderr, "Failed to create window\n");
    return 1;
  }

  std::printf("Waiting example: wait_events(INFINITE) then pump_events().\n");

  // Window events are dispatched to this bus, synchronously from pump_events(). Keyboard and mouse events do not come
  // through here: those go to the input::event_feed installed with set_input_feed, which input::context implements.
  catalyst::events::bus bus;
  set_event_bus(&bus);

  // The listeners are held in scoped_tokens so they come off the bus before it goes out of scope.
  bool running = true;
  const catalyst::events::scoped_token sub_close =
      bus.add_listener<window_close_requested_event>([&](const window_close_requested_event &)
                                                    {
                                                      std::printf("Close requested\n");
                                                      running = false;
                                                    });

  const catalyst::events::scoped_token sub_resize =
      bus.add_listener<window_resized_event>([&](const window_resized_event &e)
                                            {
                                              // The event carries ui::length measurements, which resolve to pixels
                                              // against the window's DPI context.
                                              const auto ctx = resolve_context_for_window(w);
                                              std::printf("Resized: %.0f x %.0f\n",
                                                          catalyst::ui::resolve_or(e.width_px, catalyst::ui::axis::x, ctx),
                                                          catalyst::ui::resolve_or(e.height_px, catalyst::ui::axis::y, ctx));
                                            });

  const catalyst::events::scoped_token sub_enter =
      bus.add_listener<window_enter_size_move_event>([](const window_enter_size_move_event &)
                                                    { std::printf("Enter size/move (interactive resize begins)\n"); });

  const catalyst::events::scoped_token sub_exit =
      bus.add_listener<window_exit_size_move_event>([](const window_exit_size_move_event &)
                                                   { std::printf("Exit size/move (interactive resize ends)\n"); });

  while (running && is_valid(w))
  {
    (void)wait_events(0xFFFFFFFFu); // INFINITE
    pump_events();
    std::printf("Frame...\n");
  }

  set_event_bus(nullptr);
  destroy_window(w);
  return 0;
}
