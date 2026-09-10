# Catalyst audio subsystem

Status: Tier 1 (devices, streams, the real-time seam) reshaped 2026-09-09 to match the conventions
of `catalyst::events`, `catalyst::logging` and `catalyst::input`. Tier 5 (offline rendering + tests)
carried across. Tiers 2–4 — mixer/voices, spatial, DSP — are still open and unchanged in scope.

## Why this was reshaped

The audio module was the oldest part of the project, written before `catalyst::events::bus` existed
and before the module conventions settled. Nothing about it was broken; it simply spoke a different
dialect from everything around it, and the differences were the kind that get copied forward once a
mixer and a voice API are built on top.

- **One header held the module.** `engine.hpp` carried the error enum, backends, device info, stream
  info, stats, config, offline options, two callback typedefs and the class. Every other module is
  one concern per header behind an umbrella.
- **Two-phase construction.** `initialize()` / `shutdown()` meant an `engine` could exist without
  meaning anything, which cost three error codes (`not_initialized`, `already_initialized`,
  `not_running`), a query (`is_initialized()`), and a rule the caller had to remember. Elsewhere in
  the project an object that exists is usable — `input::context in(bus);`.
- **One class doing two jobs.** `render()` and `captured_output()` lived on `engine` and returned
  `unsupported_operation` on four backends out of six, because only the offline backend owns a clock
  the caller can turn. The type's signature promised what most of its instances could not do.
- **C callbacks with `void *`.** `render_callback` plus `void *user`, and `device_change_callback`
  plus a second `void *device_change_user`. Every call site cast a `void *` back to its own type by
  hand — the exact pattern `docs/input.md` called out and removed — and device changes arrived on a
  platform notification thread with a comment telling the caller to post them to a queue of their
  own.
- **Vocabulary that did not match.** `audio::audio_error` stutters where `input::device_kind` does
  not; `to_string()` where logging uses `name()` plus a `std::formatter`; `double
  output_latency_seconds` where the rest of the project uses `std::chrono`; a `preferred_device`
  string documented as "an id, or a friendly name as a fallback for convenience, but is ambiguous".
- **No events, no tag block.** Audio was the only subsystem that told the application nothing
  through the bus.

## Shape of the subsystem

```
                        ┌──────────────── catalyst::audio ────────────────┐
                        │                                                 │
   free functions ──────┤  is_available()  available_backends()           │
   (no stream needed)   │  devices()  default_device()  find_device()     │
                        │                                                 │
                        │        stream_config + renderer                 │
                        │                  │                              │
                        │                  ▼                              │
                        │  stream::open() ──► std::expected<stream,error> │
                        │        │                                        │
   driver thread ◄──────┼────────┤ renderer(render_block&) noexcept       │
                        │        │                                        │
   OS notify thread ────┼──► notice_queue ──► pump() ──► events::bus      │
                        │                       (caller's thread)         │
                        │                                                 │
   CI / tests ──────────┤  offline_stream::open() ──► render(frames)      │
                        │        same renderer, no device, no thread      │
                        └─────────────────────────────────────────────────┘
```

### The five choices that define it

**1. An open stream is the only kind there is.** `stream::open()` returns
`std::expected<stream, error>`; the destructor closes. There is no state in which a stream exists
but is not negotiated, so `not_initialized`, `already_initialized` and `is_initialized()` are gone
rather than renamed. What remains observable is what matters: open, and open and running.

**2. The caller-clocked stream is a different type.** `offline_stream` has `render(frames)`,
`captured()` and `write_wav()`; `stream` has `start()`, `stop()` and `pump()`. Neither has a method
that fails because of which backend it is. `unsupported_operation` survives only for its honest
case — a backend that understands a configuration and cannot serve it, which today means WASAPI and
duplex. Both take the same `renderer`, so what CI exercises is the code that will play.

**3. The renderer is a typed, non-owning reference.** `audio::renderer` is two pointers, allocates
nothing, and accepts any `noexcept`-invocable lvalue. `noexcept` is enforced by the `renderable`
concept rather than requested in a comment — there is no safe way to unwind out of a driver
callback, so it is a compile error. Binding a *temporary* is also a compile error, because a lambda
that dies at the end of the full-expression is a dangling pointer discovered at 48 kHz. Name the
callable and pass it by name; a stateless lambda or a plain function converts to a function pointer
and is unaffected.

**4. Device changes are bus events, published from `pump()`.** The backend's notification thread
pushes onto a small bounded queue; `stream::pump()` drains it onto the `events::bus` the caller
supplied, on the caller's thread. So a listener is an ordinary function that may log, open a device
or touch a widget. The cost is that events are as timely as `pump()` is frequent — once a frame is
soon enough for every use they have — and a program that never pumps grows nothing, because the
queue drops the oldest rather than the newest.

**5. An error is worth reading.** `error` is a code plus what makes the code actionable: the
backend that failed, and for `format_unsupported` the rate and channel count the device *would*
have taken. It allocates nothing, formats as a whole sentence, and follows `json::parse_error`.

## What moved, and why

| Was | Is | Reason |
| --- | --- | --- |
| `audio/engine.hpp` (one header, 347 lines) | `types` / `error` / `backend` / `device` / `block` / `stream` / `offline` / `events` | One concern per header, as everywhere else. Code that only renders includes `block.hpp`. |
| `class engine` | `class stream` + `class offline_stream` + free functions | Three jobs — enumerate, drive a device, drive a clock — that shared a class and not a set of operations. |
| `initialize()` / `shutdown()` / `is_initialized()` | `stream::open()` → `expected`, destructor | An object that exists is open. Deletes two error codes and a query. |
| `engine::devices()`, `engine::is_backend_available()` (statics) | `audio::devices()`, `audio::is_available()` | Asking what a machine can do should not require an object to ask it of. |
| `render_callback` + `void *user` | `renderer` (function_ref) + `renderable` concept | Type-safe, still allocation-free, and `noexcept` is checked rather than hoped for. |
| `render_context` (raw pointers + counts) | `render_block` (spans + frame views) | A pointer and a count can disagree; a span cannot. `output_frame(f)` removes the interleaving arithmetic. |
| `device_change_callback` + `void *` on a driver thread | `device_added/removed/lost_event`, `default_device_changed_event` on the bus, from `pump()` | The old comment said "post this to your own queue and act on it from a thread you control". That comment is now the implementation. |
| `audio_error` enum | `error_code` + `error` struct | No module stutter, and a failure that says what the device offered. |
| `to_string(x)` | `name(x)` + `std::formatter<x>` | Matches `logging::name` / `formatter<log_level>`. `log::info("{}", backend)` needs no conversion call. |
| `engine_backend` | `backend_kind` | `input::device_kind`, not `input::input_device_kind`. |
| `double output_latency_seconds` | `seconds output_latency` | `std::chrono`, as `input_time` is. A bare double reads the same whether the author meant seconds or milliseconds. |
| `std::string preferred_device` ("id, or maybe a name") | `device_selector` | One field asking two questions, ambiguously. The selector says which question, and `find_device` resolves it against a list a picker already has. |
| `frames_per_buffer`, `buffer_frames` | `block_frames` throughout | The same quantity had two names on the request and the result. |
| `stream_stats::callback_count` | `stream_stats::blocks` | It counts blocks; there is no longer a "callback". |
| `offline_options::wav_path`, written during `shutdown()` | `offline_stream::write_wav(path)` | Best-effort I/O in a `noexcept` teardown, with no way to report failure, conditional on a second flag. Now a call that says what it did. |
| offline as a `detail::backend` | plain implementation in `offline.cpp` | It has no device to abstract. Removing the pretence removed a file. |
| `tests/audio/test_engine.cpp` | `tests/audio/test_stream.cpp` + `test_events.cpp` | Split with the type it tests; events got coverage they never had. |

## Headers

Each header is one concern; `audio/audio.hpp` pulls in the module.

| Header | Contents |
| --- | --- |
| `audio/types.hpp` | `sample`, `frame_count`, `channel_count`, `sample_rate_t`, `audio_clock`, `seconds`, `stream_direction`, `channel_layout`, frames↔time and dB↔gain conversions |
| `audio/error.hpp` | `error_code`, `error`, `make_error`, `name()`, formatters |
| `audio/backend.hpp` | `backend_kind`, `is_available()`, `available_backends()`, `default_backend()` |
| `audio/device.hpp` | `device_info`, `device_selector`, `devices()`, `default_device()`, `find_device()` |
| `audio/block.hpp` | `render_block`, `renderable`, `renderer` — and the real-time contract, stated in one place |
| `audio/stream.hpp` | `stream_config`, `stream_info`, `stream_stats`, `class stream` |
| `audio/offline.hpp` | `offline_config`, `class offline_stream` |
| `audio/events.hpp` | tag block `0x0002'0000`, `audio_event<Tag>`, the eight event structs |

Implementation is `src/audio/`, backends under `src/audio/<backend>/` behind
`src/audio/detail_backend.hpp`. The offline renderer is deliberately not behind that seam.

## What it reads like

```cpp
namespace audio = catalyst::audio;

for (const auto backend : audio::available_backends())
    log::info("Available backend: {}", backend);          // no to_string()

// The renderer is named, because `renderer` refers to it rather than owning it.
auto render = [&synth](audio::render_block &block) noexcept {
    for (std::uint32_t f = 0; f < block.frames; ++f)
        for (audio::sample &channel : block.output_frame(f))
            channel = synth.next();
};

events::bus bus;

audio::stream_config cfg;
cfg.device      = audio::device_selector::by_id(settings.audio_device_id);
cfg.sample_rate = 48000;
cfg.bus         = &bus;

auto stream = audio::stream::open(cfg, render);
if (!stream) {
    // "WASAPI: device is in use by another process"
    log::critical("{}", stream.error());
    return;
}

stream->start();

while (running) {
    stream->pump();     // device events reach listeners here, on this thread
    frame();
}
```

And the same renderer, in a test, with no sound card in the machine:

```cpp
auto offline = audio::offline_stream::open({.sample_rate = 48000}, render);
offline->render(48000);                       // exactly one second, on this thread
CT_REQUIRE(offline->captured_frames() == 48000);
```

## Tiers

### Tier 1 — Devices, streams, the real-time seam ✅ implemented

Everything above. Backends: WASAPI (shared and exclusive), ASIO, null. Reshaped 2026-09-09.

### Tier 5 — Offline rendering and tests ✅ implemented

`offline_stream`, the float WAV writer, and `tests/audio/` — `stream`, `offline`, `events`, one
executable each, CTest names `catalyst.audio.<name>`.

### Tier 2 — Mixer and voices (next)

Land the **lock-free SPSC command ring** (game thread → audio thread) *first*: every voice and bus
API depends on it, and retrofitting it later forces a mutex into the render callback. Then
`sound_buffer` + `voice` handles, a `mixer_bus` tree, streaming voices, and decoders (WAV → Ogg/Opus
→ FLAC) slotted into the `resource::IProvider` / `registry` pattern.

Handles follow the project rule: index + generation, so a stale `voice` is detected rather than
aliasing a recycled slot — as `input::device_id` and `ui::node` already do.

### Tier 3 — Spatial

`listener` bound to `catalyst::scene` transforms, 3D emitters, VBAP/HRTF panning, reverb zones.
Positions are `math::vec3f`; `channel_layout` already exists for the panner to target.

### Tier 4 — DSP

Biquads, delay, FDN reverb, master limiter, resampler.

## Deliberately deferred, with reasons

- **WASAPI duplex** returns `error_code::unsupported_operation`. Render and capture are independent
  `IAudioClient`s with independent clocks; correct duplex needs an async ring plus drift
  compensation. `offline_stream` supports duplex, so the API shape is exercised by tests.
- **ASIO input** is implemented but unverified — no ASIO driver installed on this machine.
- **Following the default device.** `default_device_changed_event` is published; moving a running
  stream to the new endpoint is not done. It is a policy decision, and a game and a DAW want
  opposite answers.
- **ALSA and CoreAudio** report `is_available() == false` and fail to open with
  `backend_unavailable`. The enum entries exist so that persisted configuration naming them keeps
  meaning what it meant.

## Conventions

- Headers under `include/catalyst/audio/`, implementation under `src/audio/`, backends under
  `src/audio/<backend>/`.
- `#include <catalyst/audio/audio.hpp>` pulls in the module.
- Buffers are 32-bit float, interleaved, nominally [-1, 1]. Use `double` inside a renderer for phase
  accumulators, filter state and resampler positions; use `frame_count` for absolute time, never a
  float counter.
- `render_block::frames` is the only truth about a block's size. `stream_info::block_frames` is
  nominal, and a device may hand over a short block at any time.
- Nothing in a renderer may allocate, lock, block, do I/O, or log.
- Audio owns event tag block `0x0002'0000`–`0x0002'FFFF`. Values are never reused or reordered.
