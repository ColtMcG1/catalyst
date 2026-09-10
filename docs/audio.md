# Catalyst audio subsystem

Status: Tier 1 (devices, streams, the real-time seam) reshaped 2026-09-09 to match the conventions
of `catalyst::events`, `catalyst::logging` and `catalyst::input`. Tier 5 (offline rendering + tests)
carried across. Tier 2 opened 2026-09-10 with the lock-free hand-off every voice and bus API is
built on — `spsc_ring`, `command`, `command_ring` — and the mixer core that is its first consumer:
`sound_buffer`, `voice` handles, and a flat `mixer` driven entirely through the ring. The bus tree,
streaming voices and decoders are still open, as are Tiers 3–4 (spatial, DSP), unchanged in scope.

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
   game thread ─────────┼──► command_ring ──► execute(), on that thread   │
   (voices, parameters) │        no lock, no allocation, in order         │
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
| `audio/ring.hpp` | `cache_line_bytes`, `ring_element`, `spsc_ring<T>` — the lock-free hand-off between two threads |
| `audio/command.hpp` | `commandable`, `command`, `command_ring` — a change posted on one thread and applied on the render thread |
| `audio/sound.hpp` | `sound_id`, `sound_buffer` — decoded audio in memory, and the handle a mixer knows it by |
| `audio/mixer.hpp` | `voice_id`, `voice_params`, `mixer_config`, `mixer_stats`, `class mixer` |
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

Everything above. Backends: WASAPI (shared and exclusive), ASIO, null. Reshaped 2026-09-09;
the backends were factored onto a shared base on 2026-09-10.

#### What a backend is, and what it is not

A backend contains its platform API and nothing else. Everything a backend has *because it is a
backend* lives in `src/audio/detail_backend_base.hpp`:

- `backend_base` — the request, the `render_dispatcher` and its `stats_block`, the identity of the
  device that was opened, `is_running`/`stats`/`reset_stats`/`take_xruns`/`take_failure`, and
  `failure(code)` so every error names the backend that produced it. A backend supplies only
  `enumerate_devices`, `open`, `start`, `stop`, `close` and `info`.
- `notice_publisher` — the hand-off from a platform notification thread to `stream::pump()`. Held by
  `shared_ptr` on both sides and retired at teardown, so a notification already in flight cannot
  reach freed state, and it owns its own copy of the device identifier so it never reads the
  backend's. Everything it does is `noexcept`, allocation included.

Sample conversion is `src/audio/detail_convert.hpp`: `sample_format` × `std::endian` resolved once,
at configuration time, to a `pack_fn`/`unpack_fn`. The device side of a conversion is packed and the
float side is strided, which is what lets one table serve ASIO's planar per-channel buffers
(`stride` = channels) and WASAPI's interleaved ones (`stride` = 1). Fixed-point stores clamp; float
stores deliberately do not.

`src/audio/win32/detail_win32.hpp` is the Windows vocabulary both backends share: `com_ptr`,
`com_apartment`, `co_task_ptr`, GUID text, and the HRESULT-to-`error_code` mapping. `com_ptr` is
written against `AddRef`/`Release` alone rather than against `IUnknown`, which is what lets the same
type hold a WASAPI endpoint and the ASIO loader's hand-declared `IASIO` — the two differ in how an
interface is *obtained*, not in what one is.

#### Formats

WASAPI prefers float32 and, when it gets it, the renderer writes straight into the driver's buffer
with no copy. Any other layout the conversion table understands is accepted through a scratch
buffer, which is what makes exclusive mode usable on interfaces that offer only 24- or 32-bit PCM.
In exclusive mode the device hands over the whole buffer once per period, so the buffer is the
block and `GetCurrentPadding` must not be consulted — it reports an exclusive render buffer as
permanently full.

### Tier 5 — Offline rendering and tests ✅ implemented

`offline_stream`, the float WAV writer, and `tests/audio/` — `stream`, `offline`, `events`,
`convert`, `command`, `mixer`, one executable each, CTest names `catalyst.audio.<name>`. `convert` covers the
shared conversion table and the GUID round trip; it reaches into `src/` because neither is public.
`command` and `mixer` are the two that start a thread — ordering and wrap-around in a ring only
mean anything once a real producer and a real consumer are looking at the indices at the same time,
and the mixer's whole contract is about which thread may touch what. Everything else about the mixer
is asserted sample-for-sample through an `offline_stream`, because it renders on demand.

### Tier 2 — Mixer and voices (in progress)

#### The command ring ✅ implemented

The lock-free SPSC hand-off landed first, on 2026-09-10, for the reason the plan gave: every voice
and bus API depends on it, and retrofitting it later forces a mutex into the render callback. It is
three types across two headers.

`spsc_ring<T>` is a bounded queue with one producer thread and one consumer thread. That restriction
is the feature — one writer and one reader need no compare-exchange and no retry loop, so a push and
a pop are each a bounded run of instructions with nothing to spin on, which is the only definition of
"real-time safe" worth having on a thread with a millisecond to spend. Capacity is a power of two
fixed at construction, so the index wraps with a mask and the buffer is allocated exactly once, by
whoever built the ring. Indices are monotonic 64-bit counters masked only when addressing a slot,
which is what makes every slot usable instead of the customary one wasted to tell full from empty.

`command` is what usually crosses it: a `noexcept` callable with its arguments copied into a
fixed 56-byte payload, one cache line all told. It is the exact inverse of `renderer`, and
deliberately so — `renderer` *refers* to a callable and refuses to bind a temporary, because it runs
for the life of the stream; a command is posted, waits, and runs later, so it *owns* its captures and
a temporary lambda is the intended argument. The `commandable` concept rejects at compile time the
four captures that would break the render thread: one that can throw, one that is not trivially
copyable, one that is not trivially destructible (a `shared_ptr` capture would call `free` at 48 kHz
when the command is discarded), and one too large for the preallocated slot.

`command_ring` is the channel plus its counters — `posted`, `executed`, `refused`. `execute(budget)`
caps how much work one block will do, because an unbounded drain makes a block's cost depend on how
busy the game thread was. A full ring refuses the *newest* command, the opposite of `notice_queue`:
for device topology the newest state is the only one worth having, but applying a "set gain" whose
"start voice" was thrown away produces state nobody asked for. `refused` counts refusals rather than
losses, because only the caller knows which it was — a game thread that gives up has lost a command,
one that posts again has not.

Ownership travels back the same way. When a command hands the render thread something to replace,
the old object must not be freed there; it goes into a second ring pointing the other way, which the
game thread drains and destroys. There is no separate type for the return path because it is the
same mechanism with the threads swapped.

```cpp
audio::command_ring to_audio;                       // game thread → render thread
audio::spsc_ring<std::unique_ptr<sound>> retired(64); // and back, for what it replaces

// Game thread. A temporary lambda is right here; captures are copied.
to_audio.post([&mixer, voice, gain]() noexcept { mixer.set_gain(voice, gain); });

// Render thread, at the top of the block.
auto render = [&](audio::render_block &block) noexcept {
    to_audio.execute(64);       // bounded, so a burst cannot become an xrun
    mixer.fill(block);
};
```

#### The mixer core ✅ implemented

`sound_buffer`, `voice` handles and a flat `mixer`, landed the same day on top of the ring. A game
thread calls `add_sound`, `play`, `set_gain`, `stop`; the render thread calls `render(block)`, which
applies the queue and sums whatever is playing. Handles follow the project rule — index plus
generation, so a stale `voice` is detected rather than turning down whatever took its slot, as
`input::device_id` and `ui::node` already do.

Four decisions are worth keeping:

**The mixer takes its format from the block, not from a config.** `mixer_config` has sizes and
nothing else: no `sample_rate`, no `output_channels`, because `render_block` already carries both
and a device is free to negotiate something other than what was asked for. A separately configured
mixer could disagree with the stream feeding it, and the disagreement would be inaudible right up
until it was a wrong pitch.

**A handle is answered immediately; the work happens later.** `play()` returns a `voice_id` before
the render thread has seen the command, because a caller that has to wait a block to learn what it
just started cannot use the answer. The slot is allocated on the game thread and the command carries
it, so the two cannot disagree — and a `play` whose command is refused rolls the slot back rather
than leaking it.

**Nothing is freed on the render thread, and no return ring is needed to arrange that.**
command.hpp describes handing an object back through a second ring, which is right when the render
thread holds the only reference. The mixer is arranged so that it never does: the game thread owns
every `sound_buffer` for as long as the mixer knows about it, and the render thread only holds a
pointer. So retiring one is a pointer dropped and an atomic word cleared — which, unlike a message,
cannot be refused by a full ring — and `collect()` frees it on the game thread. The same atomic
trick reclaims finished voice slots, which is why a voice that ends needs to tell nobody.

**Gain and pan ramp across a block; they do not jump.** A gain applied as a step is a discontinuity,
and a discontinuity is a click — which is what separates a mixer from a loop that adds. A gain given
to `play` applies at once (a fade-in nobody asked for is just as wrong), and a gain *changed* later
interpolates over exactly one block.

`collect()` is the counterpart to `stream::pump()`: it reclaims finished voices, frees released
sounds, and re-posts anything a full ring refused. Refusals are not all equal, and the mixer says
which are which — a lost "quieter" is inaudible and the next value supersedes it, so `set_*` is not
retried; a lost "stop" is a sound that never stops, so `stop`, `stop_all` and `release_sound` are.

```cpp
audio::mixer mix;
const auto footstep = mix.add_sound(std::move(buffer));

auto stream = audio::stream::open(cfg, mix);      // a mixer *is* a renderable
stream->start();

mix.play(footstep, {.gain = 0.8f, .pan = -0.3f}); // game thread, any time

while (running) {
    stream->pump();
    mix.collect();                                 // once a frame, beside pump()
    frame();
}
```

#### Still open

A `mixer_bus` tree — groups, per-group effects, sends — which slots in between the voice loop and
the block without changing the surface above. Then streaming voices and decoders (WAV → Ogg/Opus →
FLAC) into the `resource::IProvider` / `registry` pattern.

A streaming voice will also want a *bulk* ring — many samples per operation rather than one element
per push — which `spsc_ring` deliberately is not. That is a second type built on the same indices,
and it lands with the decoders that need it rather than in advance of them.

Playing a sound whose rate differs from the device's is handled by a linearly interpolated read
position, which is right in pitch and cheap, and audibly imperfect on large ratios. The Tier 4
resampler replaces that read and nothing else.

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
- Nothing in a renderer may allocate, lock, block, do I/O, or log. A renderer that needs to hear
  from the rest of the program does it through a `command_ring`, and hands anything it is finished
  with back through a second ring rather than destroying it.
- Audio owns event tag block `0x0002'0000`–`0x0002'FFFF`. Values are never reused or reordered.
