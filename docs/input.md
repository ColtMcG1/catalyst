# Catalyst input subsystem

Status: Tiers 1-3 implemented (2026-09-09). Tier 4 (per-platform backends beyond XInput) and Tier 5
(persistence) are planned. Supersedes the pre-`events::bus` module.

## Why this is being rewritten

The previous module was built on `catalyst::core::dispatcher`/`event_sink`/`event<T>`, all of
which were removed when the event system became `catalyst::events::bus`. Porting the old
shape forward would have carried four problems with it:

- **Process-global state.** `set_event_sink()`, `poll_gamepads()`, `get_gamepad_state()` were
  free functions over file-scope statics. Two contexts were impossible, tests had to reach
  through the same globals, and there was no place to hang per-device configuration.
- **Three hard-wired device types.** Keyboard, mouse and gamepad each had their own parallel
  API with gratuitously different spellings — `key_action`, `mouse_button_action`, and a bare
  `bool pressed`. A joystick with eleven axes could not be described at all.
- **No action layer.** Every consumer hand-wrote
  `was_key_pressed(space) || was_gamepad_button_pressed(0, a)`. Rebindable controls, "hold to
  sprint", and "WASD *or* left stick" were each the application's problem.
- **Mixed concerns.** A blocking, `sleep`-ing calibration routine lived in the core gamepad
  header; a USB HID packing helper sat in the public umbrella; `midi.hpp` was a comment with
  no code in it.

## Shape of the subsystem

```
                       ┌───────────────────── catalyst::input::context ─────────────────────┐
  platform window      │                                                                   │
  messages ───────────▶│  device_registry ──▶ typed events ──▶ catalyst::events::bus       │
                       │   (flat control       (key_event,            │                    │
  poll backends        │    values, one         gamepad_axis_event…)  │                    │
  (XInput, HID, ───────▶│    slot per control)                        ▼                    │
   MIDI) ──────────────▶│        │                            application listeners        │
                       │        ├──▶ input_state ──────────▶  polling: is_key_down(…)      │
                       │        │                                                          │
                       │        └──▶ action_map ───────────▶  polling: move.vec2()         │
                       │              (bindings,                events: action_event       │
                       │               composites,                                         │
                       │               processors)                                         │
                       └───────────────────────────────────────────────────────────────────┘
```

### The four choices that define it

**1. State-first, not event-first.** Backends write into a `device_registry` — a flat array of
control values per device. Everything else is derived from that store: typed events are
published *from* the diff, `input_state` folds frame edges over it, and actions are evaluated
against it. The old design evaluated nothing and let each consumer re-derive state from the
event stream, which is how the tracker and the event listeners could disagree about what was
held. There is now exactly one source of truth, and one publish path.

**2. Every control is a float.** A device is `n` float slots plus metadata naming them. A
button is 0 or 1 (or analog, so an Xbox trigger is a button *and* an axis without special
casing). A stick is two adjacent slots. A pen's pressure, a sensor's quaternion, a HOTAS's
eleventh axis — all the same. This is what makes "a large range of input devices" possible
without an enum per device family. The familiar enums (`key_code`, `gamepad_axis`, …) survive
as *names for slot indices*, so the common case stays as readable as it was.

**3. Named enums are views, not the model.** `gamepad_axis::left_x` is a `control_id`; so is
`key_code::space`; so is joystick axis 11. The binding layer only ever sees `control_id`, so
it works with hardware nobody wrote an enum for. Typed events (`key_event`, `gamepad_button_event`)
remain the readable public surface for direct listeners.

**4. An explicit object.** `input::context` owns the registry, the backends, and the action
maps, and publishes to an `events::bus` you hand it. No globals, no static-init ordering, two
instances in one process if you want them (a test harness usually does). It is `context` rather
than the more obvious `system` because `<process.h>` declares a global `system()`, and
`using namespace catalyst::input` would then make the name ambiguous - which the tests hit
immediately.

## What moved, and why

| Was | Is | Reason |
| --- | --- | --- |
| `input/usb.hpp` (public umbrella) | `input/hid.hpp` | It is a USB HID *usage* packing helper, not a device API. It stays public because `key_code` values are HID usages, but it is no longer something you meet on the way in. |
| `gamepad_deadzone_calibrator`, `deadzone_for_noise` in `gamepad.hpp` | `input/calibration.hpp` | Calibration is a tool built *on* the gamepad API, not part of it. `gamepad.hpp` shrinks to vocabulary + events. |
| `calibrate_gamepad_deadzone()` (blocking, `sleep_for` in a loop) | `examples/input_calibration` | A library core should not sleep. The frame-driven calibrator does the same job inside a normal loop; the blocking convenience belongs to the program that wants it. |
| `text_input_event` in `keyboard.hpp` | `input/text.hpp` | Text is not a physical key. Separating it makes room for IME composition events, which real i18n needs and which have nothing to do with `key_code`. |
| `platform::set_event_sink()` publishing input events | `platform::set_input_feed()` feeding `input::context` | Platform translates window messages; it should not also be an input event publisher. Now *all* input events reach the bus through the registry, so the registry can never disagree with the event stream (see choice 1). |
| `input/midi.hpp` (empty stub) | `input/midi.hpp` (real vocabulary + events) | — |
| MIDI *synthesis* | stays out; belongs to `catalyst::audio` | A MIDI keyboard is an input device. A MIDI synthesiser is an audio one. |

Cursor capture and hidden-cursor modes stay in `catalyst::platform` — they are window
properties, not device state.

## Headers

Each header is one concern; `input/input.hpp` pulls in the module.

### Vocabulary and events

| Header | Contents |
| --- | --- |
| `input/hid.hpp` | `usb_hid` packing, usage pages (was `usb.hpp`) |
| `input/device.hpp` | `device_id`, `device_kind`, `device_info`, `control_kind`, `control_id`, `control_info`, `device_layout`, `device_selector` |
| `input/keyboard.hpp` | `key_code`, `key_modifiers`, `button_action`, `key_event` |
| `input/text.hpp` | `character_code`, `text_input_event`, `text_composition_event` |
| `input/mouse.hpp` | `mouse_button(s)`, `mouse_axis`, move/button/wheel/enter/leave/raw events |
| `input/gamepad.hpp` | `gamepad_button(s)`, `gamepad_axis`, `gamepad_state`, `gamepad_deadzone`, connect/button/axis events, rumble |
| `input/joystick.hpp` | `hat_direction`, generic-HID button/axis/hat events for sticks, wheels and HOTAS |
| `input/touch.hpp` | `touch_phase`, `touch_point`, touch events (multi-contact, id-tracked) |
| `input/pen.hpp` | `pen_button`, pressure/tilt/twist/eraser, pen events |
| `input/midi.hpp` | `midi_status`, `midi_message`, note/CC/pitch-bend events |
| `input/events.hpp` | `device_connected_event`/`device_disconnected_event` + every header above |

### Runtime

| Header | Contents |
| --- | --- |
| `input/registry.hpp` | `device_registry`: create/remove devices, write control values, diff and publish |
| `input/state.hpp` | `input_state`: held/pressed/released/repeated, deltas, typed text, per frame |
| `input/action.hpp` | `action_kind`, `action_value`, `action_phase`, `action_event`, `action` handle |
| `input/binding.hpp` | `binding`, `binding_source`, composites (`axis1d`/`axis2d` from buttons), processors (deadzone, invert, scale, curve, press point) |
| `input/action_map.hpp` | `action_map` (named actions + bindings), `action_set` (maps you enable and disable as contexts) |
| `input/calibration.hpp` | `gamepad_deadzone_calibrator`, `deadzone_for_noise` |
| `input/feed.hpp` | `event_feed` — the interface the platform layer pushes window-sourced input through |
| `input/context.hpp` | `input::context` — owns everything, `new_frame()` / `poll()` / `update()` |

## What it reads like

```cpp
events::bus bus;
input::context in(bus);
platform::set_input_feed(&in);

using namespace catalyst::input::bind;
input::action_map &play = in.actions().add_map("gameplay");

input::action &move = play.add_axis2d("move")
    .bind(left_stick().deadzone(0.2f))
    .bind(compose2d().wasd());
input::action &fire = play.add_button("fire")
    .bind(mouse(mouse_button::left))
    .bind(pad(gamepad_axis::right_trigger).press_point(0.4f));
input::action &sprint = play.add_button("sprint")
    .bind(key(key_code::left_shift).hold(200ms));

while (running) {
    in.new_frame();          // clear frame edges and delta controls
    platform::pump_events(); // window messages -> feed -> registry -> bus
    in.poll();               // gamepads, HID, MIDI -> registry -> bus
    in.update();             // evaluate actions -> action_event on the bus

    camera.move(move.vec2() * dt);
    if (fire.was_performed()) shoot();
    if (sprint.is_held())     speed *= 2.0f;
}
```

The same information is available three ways, and which you use is a matter of taste rather
than capability: subscribe to typed events on the bus, poll `in.state()`, or poll actions.

Pausing is a context switch rather than a flag threaded through every consumer:

```cpp
in.actions().enable_only("menu");      // gameplay stops hearing anything
in.actions().enable_only("gameplay");  // and starts again
```

## Tiers

### Tier 1 — Device and control model, events ✅ implemented

`hid.hpp`, `device.hpp`, and the per-device vocabulary headers. Every event struct carries a
`device_id` and a timestamp, and is tagged (`events::tagged<…>`) so the bus dispatches on a
constant rather than `typeid`. Input owns tag block `0x0001'0000`–`0x0001'FFFF`.

### Tier 2 — Registry, state, context ✅ implemented

`registry.hpp`, `state.hpp`, `context.hpp`, `feed.hpp`. The registry diffs control values and publishes;
`input_state` folds frame edges; `system` wires them together and drives the backends. The
`platform` seam is rewired here: platform feeds `system`, and stops publishing input events.

### Tier 3 — Actions and bindings ✅ implemented

`action.hpp`, `binding.hpp`, `action_map.hpp`. Sources, composites, processors, interactions
(`press`, `hold`, `tap`, `multi_tap`, `chord`), phases, and enable/disable contexts.

### Tier 4 — Backends (partly done)

The device model is complete from Tier 1; this tier is per-platform plumbing behind the
`detail::backend` seam.

- **win32/XInput** — gamepads. ✅ ported from the existing backend.
- **null** — every seam, reporting nothing, so a non-Windows build links. ✅
- **win32/Raw Input HID** — `joystick.hpp` devices: `HidP_GetCaps`/`HidP_GetValueCaps` to build
  a `device_layout` at connect time, `HidP_GetUsages` per report. This is what makes a wheel or
  HOTAS work without a bespoke enum.
- **win32/WM_POINTER** — `touch.hpp` and `pen.hpp`.
- **win32/MIDI** — WinMM `midiIn*` to start; WinRT MIDI if timestamps prove insufficient.

### Tier 5 — Rebinding and persistence

- Serialize an `action_set` to and from `catalyst::resource::json`, so a game's default
  bindings and a player's overrides are the same document.
- Interactive rebinding: `rebind_operation` listens for the next control any device actuates,
  with exclusions (Escape cancels, mouse motion ignored unless asked for).
- Control display names, per device and per layout, for the bindings screen.

### Tier 6 — Beyond

Haptics beyond rumble (trigger effects, per-motor envelopes), input recording and replay over
the registry, a `bench/input` for action evaluation cost, sensors (accelerometer, gyro,
`quaternion` controls are already in the model), and player-device pairing for local
multiplayer.

## The platform seam

The platform module has been ported to match. It now has two separate installers, and which one an
event goes to is decided by what produced it, not by what it is:

- `platform::set_event_bus(events::bus*)` takes the window events declared in `platform/window.hpp`
  and `platform/monitor.hpp`. They are plain structs now; the bus keys on the static type, so they
  need no base class.
- `platform::set_input_feed(input::event_feed*)` takes everything a keyboard, mouse, touchscreen or
  pen produced. The backend no longer builds input events and publishes them itself - it calls the
  feed, `input::context` implements it, and the registry publishes onward. That is what makes it
  impossible for the event stream and the device state to disagree.

The win32 backend's per-window `std::bitset<key_code_count> keys_down` is gone with it: it existed
only to synthesise releases on focus loss, and `feed_focus_lost()` now does that from the state the
registry already owns. The backend still tracks held mouse *buttons*, but only for the
SetCapture/ReleaseCapture lifecycle, which is genuinely its own business.

The old `poll_event()` queue went too, along with its bound and its coalescing policy. The one
problem it solved - the OS holding the thread inside a modal size/move loop while resizes pile up -
is solved better by the frame callback, which hands control back mid-loop so the application renders
the current size instead of catching up on a queue of stale ones. See `platform::set_frame_callback`.

Both `examples/input_events` (windowed) and `examples/input_actions` (headless) build and run.

## Conventions

- Headers under `include/catalyst/input/`, implementation under `src/input/`, backends under
  `src/input/<backend>/` behind `src/input/detail_backend.hpp`.
- Tests under `tests/input/`, one executable per file, CTest name `catalyst.input.<name>`.
- `#include <catalyst/input/input.hpp>` pulls in the module.
- Handles (`device_id`, `action`) are index + generation, so a stale handle is detected rather
  than aliasing a recycled slot — the same rule `catalyst::ui` uses for `node`.
- Axis conventions, unchanged from the old module: sticks `[-1, 1]` with **+y up**, triggers
  `[0, 1]`, wheel in notches, positions in client-area pixels.
