/**
 * @file input.hpp
 * @brief Umbrella header for the Catalyst input module.
 * @details The module has three layers, and you can work at whichever suits the code you are writing:
 *
 *   1. **Devices and events.** Every device is a flat set of float controls (device.hpp); every change is published to
 *      a `catalyst::events::bus` both generically (`control_changed_event`) and in a readable typed form
 *      (`key_event`, `gamepad_axis_event`, `touch_event`, ...).
 *   2. **Polled state.** `input_state` answers "is it down" and "was it pressed this frame" (state.hpp).
 *   3. **Actions.** Named things the player can do, bound to controls across any number of devices, with composites,
 *      processors and interactions (action.hpp, binding.hpp, action_map.hpp). This is the layer game code should
 *      usually live at.
 *
 * `input::context` owns all of it (context.hpp). There are no globals; construct one, give it a bus, and drive it:
 *
 *     events::bus bus;
 *     input::context in(bus);
 *     platform::set_input_feed(&in);
 *
 *     while (running) {
 *         in.new_frame();
 *         platform::pump_events();
 *         in.poll();
 *         in.update();
 *     }
 *
 * See docs/input.md for the design and the tier roadmap.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/input/action.hpp>
#include <catalyst/input/action_map.hpp>
#include <catalyst/input/binding.hpp>
#include <catalyst/input/calibration.hpp>
#include <catalyst/input/context.hpp>
#include <catalyst/input/device.hpp>
#include <catalyst/input/events.hpp>
#include <catalyst/input/feed.hpp>
#include <catalyst/input/registry.hpp>
#include <catalyst/input/state.hpp>

namespace catalyst::input
{

    /** @brief Name of the compiled-in backend ("win32" or "null"). */
    const char *module_name();

} // namespace catalyst::input
