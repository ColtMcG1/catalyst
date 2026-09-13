/**
 * @file events.hpp
 * @brief Every input event type in one include, for code that listens but does not otherwise touch the module.
 * @details A UI layer or a debug overlay usually wants the events and nothing else; this saves it from including the
 * registry, the action layer and the system to get at them. The connect/disconnect events and the generic
 * `control_changed_event` live in device.hpp, which this pulls in with the rest.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/input/action.hpp>
#include <catalyst/input/device.hpp>
#include <catalyst/input/gamepad.hpp>
#include <catalyst/input/joystick.hpp>
#include <catalyst/input/keyboard.hpp>
#include <catalyst/input/midi.hpp>
#include <catalyst/input/mouse.hpp>
#include <catalyst/input/pen.hpp>
#include <catalyst/input/text.hpp>
#include <catalyst/input/touch.hpp>
