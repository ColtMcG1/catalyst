# Catalyst rendering subsystem

Status: reviewed 2026-09-10; Tiers 1 (diagnostic vocabulary), 2 (timelines and queues), 3 (parallel
recording and frames) and 4 (transfers) implemented the same day. The module
works and is the last one still speaking the dialect it was written in, before `catalyst::events`,
`catalyst::logging` and the module conventions settled. This document is the review, the plan that
follows from it, and the record of what has landed.

Decided when the plan was approved: the `device` and `swapchain` become RAII roots in the shape of
`audio::stream`, while resource handles stay leaves owned by the device, as in `input::context` and
`ui::tree`. Tiers are implemented in order.

The short version: the rendering module has no vocabulary for *time*. Every operation that costs GPU
milliseconds either blocks the calling thread without saying so, or completes invisibly with no way
to ask when. That single gap is what makes the API unable to express parallelism, and closing it is
most of the rewrite.

## Why this is being rewritten

### 1. Every expensive operation is synchronous, and the API cannot say so

Four separate hidden stalls, all reachable from an ordinary frame loop:

- **Re-recording a command list waits for the GPU.** `begin_recording` calls
  `wait_for_serial(cl->last_submit_serial)` (`src/rendering/vulkan/vulkan_command.cpp:362`). The
  comment is honest — a list cannot be reset while it is executing — but the consequence is that
  `examples/rendering_basics` is fully serialised: the CPU records, the GPU executes, the CPU waits,
  repeat. The obvious fix, rotating N command lists, is not available either, because `submit`
  returns `bool` and there is no way to learn when list *i* became free again.

- **Writing to a GPU-only buffer drains the entire queue.** `write_buffer` on
  `memory_access::gpu_only` goes to `stage_upload` → `begin_immediate` / `end_immediate`, which is
  `vkQueueSubmit` followed by `vkWaitForFences(..., UINT64_MAX)`
  (`src/rendering/vulkan/vulkan_device.cpp:723-744`). Because fences on one queue signal in
  submission order, waiting on the immediate fence waits for *every submission issued before it*.
  One buffer write in the middle of a frame stalls the whole frame.

- **Creating a resource with initial data takes the same path.** `create_buffer` and
  `create_texture` with `initial_data` are each a full CPU↔GPU round trip. Loading a level is N of
  them, in series.

- **Acquiring a back buffer blocks twice, without a timeout.** `acquire_next_image` calls
  `wait_for_serial` on the acquire slot and then `vkAcquireNextImageKHR(..., UINT64_MAX, ...)`
  (`src/rendering/vulkan/vulkan_swapchain.cpp:533-537`). The caller cannot do anything else while
  the compositor decides, and cannot choose to skip the frame instead.

The single shared staging buffer states the constraint outright: *"the immediate command buffer is
submitted and waited on before control returns to the caller, so successive transfers cannot
overlap"* (`src/rendering/vulkan/vulkan_backend.hpp:426`). That is a correct description of a
design that has decided transfers are free. They are not; they are the second most expensive thing
the module does.

### 2. There is no vocabulary for "asked for, not finished"

The public headers contain no fence, no semaphore, no timeline, no completion query, no timeout.
`submit` returns `bool` — did the driver accept it, nothing more. The only wait is `wait_idle`,
which stalls everything on the device.

The backend already has exactly the right concept and keeps it private: a monotonically increasing
`serial` per submission, `wait_for_serial`, `completed_serial`, and `defer_release` for destroying a
resource that is still in flight (`vulkan_backend.hpp:74-84, 376-390`). The rewrite is, more than
anything else, promoting that serial into the public API and giving it a type worth having.

### 3. The module is not thread-safe at all, and does not say so

All backend state lives in one `static registry` of eight `unordered_map`s behind `reg()`
(`src/rendering/vulkan/rendering_backend_vulkan.cpp:30-39`), and `allocate_id` is a plain
`next_id++`. There is not one `std::mutex`, `std::atomic` or `std::thread` anywhere under
`src/rendering/` or `include/catalyst/rendering/`. Two threads calling any two rendering functions
is a data race.

So recording command lists on several threads — the largest CPU win available to any renderer, and
the reason `VkCommandPool` and `ID3D12CommandAllocator` are per-thread objects in the first place —
is not merely unsupported. It is undefined, and nothing in the headers warns anyone off. Compare
`audio/stream.hpp`, which names all three threads involved and says which calls belong to which, or
`input::context`, which says who owns it.

### 4. `queue_type` is a promise the backend does not keep

`command_list_desc::queue` accepts `graphics`, `compute` and `transfer`, and the Vulkan backend runs
all of them on one `VkQueue` from one family — *"Every queue type runs on the single device queue for
now"* (`src/rendering/vulkan/vulkan_command.cpp:307`). The enum is enforced restrictively (a
`transfer` list refuses `dispatch`, `vulkan_command.cpp:685`) while buying no parallelism at all.
The caller pays the restriction and gets nothing back.

### 5. Failures are `false`, `{}` and `stderr`

`create_*` returns an invalid handle; `submit`, `present`, `resize_swapchain`, `write_buffer` and
`read_buffer` return `bool`; the reason is printed to `stderr` by a varargs `report()`
(`vulkan_device.cpp:27-35`). `acquire_next_image` returns the same invalid texture for "the window
is minimised", "the swapchain is out of date, resize it" and "the device is gone" — three outcomes
that need three different responses from the caller.

Every other module in the project returns `std::expected<T, error>` where `error` is a code plus the
little that makes the code actionable, and `resource::json`, `audio` and `input` all agree on the
shape.

### 6. No logging, no events

There is not one reference to `catalyst/logging` in the module. Device loss, swapchain invalidation
and a GPU that stopped answering are precisely the things an application must react to, and there is
no bus to hear them on. This is the same gap that reshaped audio: *"Audio was the only subsystem
that told the application nothing through the bus."* Rendering is now the only one.

### 7. The handle model is fine, and is staying

Recorded here because it is the obvious thing to "fix" and should not be. `resource_handle<Tag>`
wraps a bare 64-bit `resource_id` where `ui::node` and `input::device_id` are index + generation,
which looks like an inconsistency until you check where the ids come from: both backends allocate
from a global monotonic counter with no free list (`vulkan_backend.hpp:303`,
`null_resources.cpp:109-123`), so an id is never handed out twice. A stale handle resolves to
nothing and `is_valid` rejects it — the same guarantee a generation buys, without the packing. The
`Tag` template parameter already keeps a `buffer` from being passed where a `texture` is expected.

Handles stay as they are. What changes is who owns them: the `device` becomes an RAII root, and
resource handles remain leaves owned by it, matching `input::context` and `ui::tree`.

### 8. No RAII

`create_device` / `destroy_device(device &)` on a handle the caller must remember to release, for
every resource kind. `audio::stream` already settled what the project prefers: a factory that
returns an object only on success, and a destructor that closes it.

---

## Shape of the subsystem

```
                    ┌──────────────────── catalyst::rendering ────────────────────┐
                    │                                                             │
  adapters ─────────┤  adapters()  default_adapter()                              │
  (no device yet)   │                                                             │
                    │  device::open() ──► std::expected<device, error>            │
                    │        │                                                    │
                    │        ├─ graphics()  compute()  copy()   ── queue objects, │
                    │        │        │                            each a timeline│
                    │        │        ▼                                           │
  N worker threads ─┼──► command_pool (one per thread per frame)                   │
  record in parallel│        │  └─ command_list ── open() … close()                │
                    │        ▼                                                    │
                    │  queue::submit(submit_info) ──► timeline_point               │
                    │        │        wait: {points…}   signal: {points…}          │
                    │        │        cross-queue ordering with no CPU wait        │
                    │        ▼                                                    │
  streaming /  ─────┼──► copy().begin_batch() … submit() ──► timeline_point         │
  level load        │        staging ring, never an inline stall                   │
                    │                                                             │
  game thread ──────┼──► co_await timeline_point                                   │
                    │        resumed by device::pump(), on the caller's thread     │
                    │                                                             │
  driver / OS ──────┼──► notice queue ──► pump() ──► events::bus                    │
                    │        device_lost, swapchain_out_of_date, gpu_timeout       │
                    │                                                             │
  frame pacing ─────┤  frame_ring{dev, 3} ──► begin() ──► frame ──► end(point)      │
                    │        the one call in the loop that is allowed to block     │
                    └─────────────────────────────────────────────────────────────┘
```

### The five choices that define it

**1. A `timeline_point` is the return value of everything that costs GPU time.** Not a fence, not a
semaphore, not a handle to either — a *value* naming a moment on a queue's timeline. It is 16 bytes,
trivially copyable, comparable, and it is the same object on every backend: a Vulkan timeline
semaphore, a D3D12 fence and an `MTLSharedEvent` are all a monotonic 64-bit counter. Binary
semaphores exist only in swapchain interop and stay inside the backend where they belong.

A point can be asked (`is_complete()`), waited on by the CPU (`wait()`, `wait_for(duration)`),
waited on by the GPU (hand it to another `submit` as a dependency — no CPU involvement at all), or
awaited (`co_await`, composing with `catalyst::events::task<T>`). One type, four uses, and it
replaces every `bool` return that today means "accepted, good luck".

**2. Queues are objects, and they tell the truth.** `device::graphics()`, `device::compute()` and
`device::copy()` return `queue &`, each with its own timeline. `queue::info()` reports whether it is
distinct hardware or an alias of the graphics queue on an adapter that has nothing better, so a
caller who wants to know can find out instead of guessing. `queue_type` on a command list description
goes away: a list is opened from a pool that belongs to a queue, which is where the restriction
actually lives.

**3. Recording is per-thread by construction, not by convention.** A `command_pool` is thread-affine
and move-only; a `command_list` is opened from exactly one pool and touches nothing else. N threads
each hold their own pool, record, close, and hand back a list; one thread submits them together.
There is no shared mutable state on the recording path to lock, because the type system does not
offer any. The backend registry gets per-device locking so that *creation* and *destruction* from
several threads are defined too — but the hot path never reaches it.

**4. Transfers are a queue, a ring and a batch — never an inline stall.** `copy().upload(...)`
copies into a staging ring and returns a `timeline_point`; it does not submit, wait, or allocate.
`copy().begin_batch()` accumulates many copies and submits once. `copy().download(...)` returns
something you `co_await`. `write_buffer`'s hidden `vkWaitForFences` disappears, and with it the
device-wide drain it caused. The one shared grow-on-demand staging buffer becomes a ring sized in
the device description, with back-pressure reported as `error_code::staging_exhausted` rather than a
silent stall.

**5. Continuations resume on the thread that calls `pump()`.** The module ships no thread pool and
starts no threads of its own. `device::pump()` polls the timelines, resumes the coroutines whose
points have completed, publishes queued events onto the bus, and collects deferred releases — all on
the caller's thread, exactly as `audio::stream::pump()` does. Rendering composes with whatever
scheduler the application already has rather than importing one; `events::task` plus `pump()` is the
whole seam.

---

## What moved, and why

| Today | After | Why |
|---|---|---|
| `bool submit(dev, lists)` | `std::expected<timeline_point, error> queue::submit(submit_info)` | The caller needs to know *when*, not just *whether*. |
| `wait_idle(dev)` | `timeline_point::wait()`, `queue::wait_idle()`, `device::wait_idle()` | Three granularities; today only the biggest hammer exists. |
| `begin_recording` blocks | `frame_ring::begin()` blocks, visibly | One blocking call in the loop, named for what it is. |
| `write_buffer` stalls the queue | `copy().upload(...) -> timeline_point` | Transfers stop being pretend-free. |
| `command_list_desc::queue` | `command_pool` belongs to a queue | The restriction lives where the parallelism does. |
| `report()` to `stderr` | `catalyst::logging`, category `rendering` | Same reason audio stopped printing. |
| invalid handle / `false` | `std::expected<T, error>` + `error_code` | Three different swapchain failures need three responses. |
| nothing on the bus | `device_lost_event`, `swapchain_out_of_date_event`, `gpu_timeout_event` | Device loss is not an error return, it is an event. |
| `create_device` / `destroy_device` | `device::open()`, destructor closes | Matches `audio::stream`. |

---

## Headers

```
include/catalyst/rendering/
    types.hpp       handles (index + generation), formats, flag helpers, geometry
    error.hpp       error_code, error, name(), std::formatter                     [new]
    events.hpp      device_lost, swapchain_out_of_date, gpu_timeout               [new]
    timeline.hpp    timeline_point, awaiter, ordering                             [new]
    queue.hpp       queue, queue_kind, queue_info, submit_info                    [new]
    device.hpp      adapters, device (RAII, move-only), pump()
    buffer.hpp      buffers, structured_buffer<T>
    shader.hpp      shader modules from bytecode
    texture.hpp     textures and samplers
    pipeline.hpp    graphics / compute pipeline state objects
    command.hpp     command_pool, command_list, render passes                     [pools new]
    frame.hpp       frame_ring, frame                                             [new]
    transfer.hpp    transfer_batch, upload, download, staging ring                [new]
    swapchain.hpp   swapchain (RAII), acquired_image
    rendering.hpp   umbrella
```

---

## What it reads like

**A frame, with three frames in flight.** The only call that blocks is the one whose name says so.

```cpp
using namespace catalyst::rendering;

auto dev = device::open({.application_name = "demo", .validation = validation_mode::on, .bus = &bus});
if (!dev)
{
    logging::error<app>("rendering: {}", dev.error().message());
    return 1;
}

auto sc = swapchain::open(*dev, {.window = platform::get_native_handle(w), .extent = {1280, 720}});
frame_ring frames{*dev, 3};

while (running)
{
    dev->pump();                        // events, continuations, garbage — caller's thread

    frame f = frames.begin();           // waits for frame N-3; the one honest stall
    auto image = sc->acquire(f);
    if (!image)
    {
        if (image.error().code == error_code::swapchain_out_of_date)
            sc->resize(client_size(w));
        continue;                       // minimised, or a resize is pending: skip the frame
    }

    command_list cl = f.pool().open();
    cl.begin_render_pass({.color = {{.target = image->target, .clear = {0.1f, 0.1f, 0.1f, 1.0f}}}});
    cl.set_pipeline(triangle);
    cl.draw(3);
    cl.end_render_pass();
    cl.close();

    const timeline_point done = submit(graphics, {
        .lists = {&cl, 1},
        .wait  = {&image->ready, 1},    // GPU-side; the CPU never touches this
    }).value();

    sc->present(done);
    f.end(done);                        // this frame's pools retire when `done` completes
}
```

**Recording on N threads.** Each worker holds its own pool; nothing is shared, so nothing is locked.

```cpp
frame f = frames.begin();
std::vector<command_list> recorded(worker_count);

{
    std::vector<std::jthread> workers;
    for (std::uint32_t i = 0; i < worker_count; ++i)
        workers.emplace_back([&, i] {
            command_list cl = f.pool(i).open();     // pool i is this thread's, and only this thread's
            record_slice(cl, scene, i, worker_count);
            cl.close();
            recorded[i] = std::move(cl);
        });
}                                                    // joined

dev->graphics().submit({.lists = recorded});
```

**Streaming a level without stalling the frame.** The upload runs on the copy queue while the
graphics queue keeps drawing; the coroutine resumes in a later `pump()`.

```cpp
events::task<void> load_level(device &dev, level_data data)
{
    transfer_batch batch = dev.copy().begin_batch();
    for (const mesh_data &m : data.meshes)
        batch.upload(m.target, 0, m.bytes);

    const timeline_point resident = batch.submit().value();
    co_await resident;                              // resumes on the thread that calls pump()

    logging::info<app>("level resident: {} meshes", data.meshes.size());
    scene.publish(std::move(data));
}
```

**Cross-queue dependency, with no CPU wait anywhere.** Culling on the compute queue, drawing on the
graphics queue, ordered by the GPU.

```cpp
const timeline_point culled = dev->compute().submit({.lists = {&cull, 1}}).value();
dev->graphics().submit({.lists = {&draw, 1}, .wait = {&culled, 1}});
```

**Reading back.** No `read_buffer` that lies about being cheap.

```cpp
events::task<void> screenshot(device &dev, texture target, std::filesystem::path path)
{
    auto pixels = co_await dev.copy().download(target);
    if (pixels)
        write_png(path, *pixels);
}
```

---

## Tiers

### Tier 1 — Vocabulary: errors, events, logging ✅ implemented

`error.hpp` (`error_code` + `error` + `name()` + `is_transient` / `is_fatal` + `std::formatter`,
shaped like `audio::error`), `events.hpp` with tag block `0x0003'0000`, and the varargs `report()`
replaced by `catalyst::logging` under a `rendering` category. Handles are left alone — see review
point 7. No concurrency yet: everything below returns `std::expected<T, error>`, so this comes
first.

What landed:

- `include/catalyst/rendering/error.hpp` — sixteen codes, each with the context that makes it
  actionable. The three that fixed a real ambiguity are `not_ready`, `swapchain_out_of_date` and
  `device_lost`, which `acquire_next_image` currently reports with the same invalid texture.
  `error` is trivially copyable, 24 bytes, and allocates only in `message()`.
- `include/catalyst/rendering/events.hpp` — `device_lost_event`, `swapchain_out_of_date_event`,
  `swapchain_resized_event`, with the block subdivided so later tiers append without renumbering.
- `src/rendering/detail_log.hpp` — the `render_log` category; all 47 `report()` sites rewritten as
  `logging::error` / `warn`, and Vulkan's debug messenger now maps the layer's own severity onto a
  level instead of stamping everything `"validation"`.
- `tests/rendering/test_diagnostics.cpp` — CTest `catalyst.rendering.diagnostics`.

**One behaviour change for existing callers.** Backend diagnostics used to go to `stderr`
unconditionally; they are now routed log events, so a program that installs no sink sees nothing.
Add a `console_sink` to `logging::default_logger()` to get the old behaviour back, filtered.

The types are declared and tested but not yet returned by anything: threading them through the
public API is Tier 2's job, which is why that tier comes next rather than the transfers work that
has the bigger frame-time win.

### Tier 2 — Timelines and queues ✅ implemented

`timeline.hpp`, `queue.hpp`. `submit` returns a `timeline_point`; `co_await` support and `pump`. On
Vulkan this was smaller than it sounds: the existing `serial` *was* a timeline value, so
`std::vector<submission>` plus a per-submission `VkFence` and its free-list collapsed into one
`VK_SEMAPHORE_TYPE_TIMELINE` semaphore per queue with `vkWaitSemaphores` for the waits.

What landed:

- `include/catalyst/rendering/timeline.hpp` — `timeline_point`: 24 bytes, trivially copyable,
  `is_complete` / `wait` / `wait_for` / `co_await`, and a `<=>` that returns
  `partial_ordering::unordered` across timelines rather than inventing an order the hardware does
  not give. `pump(dev)` resumes parked coroutines and collects garbage, on the caller's thread.
- `include/catalyst/rendering/queue.hpp` — `queue`, `queue_info`, `submit_info` with GPU-side
  `wait`, and `submit` returning `std::expected<timeline_point, error>`. `last_submitted`,
  `completed`, `wait_idle(queue)`.
- `queue_kind` in types.hpp replaces `queue_type`. Real families where the adapter has them: on an
  Arc 140V the backend resolves graphics 0, compute 1, copy 2, all dedicated, and says so in a log
  line at device creation.
- `is_device_lost(device)` plus a `to_error(VkResult)` that latches device loss in one place, so
  every other entry point fails fast instead of hanging on a semaphore that will never signal.
- `deferred_release` now carries one value per queue and is collected when all three have passed.
- `tests/rendering/test_timeline.cpp` — CTest `catalyst.rendering.timeline`, eight cases including
  a cross-queue GPU-side dependency and the `co_await` / `pump` seam.

**A design correction found by the tests.** The first cut let a list run on any queue that could
execute its work — a `copy` list on the graphics queue, say. That is wrong, and wrong in the way
that segfaults rather than errors: a command buffer belongs to the queue family its pool was created
from, so submitting it elsewhere is undefined behaviour. The rule is now that a list is submitted to
a queue of its own kind and no other, checked in `submit` before the driver ever sees it. Nothing is
lost, because the aliasing happens a level lower: on an adapter with no DMA engine the `copy` queue
*is* the graphics queue, and the list was allocated from the graphics family to begin with. Caller
code is identical either way. `queue_accepts` survives for what it is honestly for — deciding at
record time whether a list may contain a draw or a dispatch.

**What this did not fix.** The three stalls from review point 1 were all still there at the end of
this tier: `begin_recording` still waited for the list's previous submission, `write_buffer` on
GPU-only memory still blocked, and `acquire_next_image` still had no timeout. Tier 2 built the
vocabulary to describe and avoid them; Tier 3 removed the first and Tier 4 the second. The third is
still open — see Tier 6. `end_immediate` did get narrower — it waits for its own submission rather
than for every fence issued before it, which was a side effect of fences signalling in order — but no
frame-time claim was made here, and the `frame` bench numbers taken after this tier are the baseline
the Tier 3 table compares against.

### Tier 3 — Parallel recording and frames ✅ implemented

`command_pool` / `command_list` / `frame_ring`. The hidden wait in `begin_recording` is deleted —
list reuse becomes the frame ring's job, and the ring says out loud that it waits. Locking in the
registry so create and destroy from several threads are defined; the recording path holds only a
shared lock because it shares nothing. Thread rules stated in every file comment, the way
`audio/stream.hpp` states them.

A pool belongs to one queue kind, which Tier 2 already established is a hardware constraint rather
than a convention — so `frame::pool(worker, kind)` is the shape, and the per-thread pool and the
per-kind family index are the same object.

What landed:

- `command_pool` in command.hpp — a handle, created for one `queue_kind`, with
  `create_command_list(pool, name)` allocating from it and `reset_command_pool` recycling every list
  it handed out. `reset_command_pool` never blocks: it returns `error_code::not_ready` when any list
  from the pool is still in flight, having changed nothing.
- `begin_recording` no longer waits. It *refuses* — for a list still executing, and for a list
  already recorded since its pool was reset — and logs which of the two happened along with what to
  do about it. `last_submission(cl)` is the query that lets a caller answer the question itself.
- The one exception is the list from `create_command_list(device, desc)`, which owns a private pool
  containing only itself and may therefore recycle it inside `begin_recording`. That is what keeps
  every call site written before this tier working, minus the wait.
- `include/catalyst/rendering/frame.hpp` — `frame_ring` (move-only, RAII) and `frame` (a view).
  `begin()` waits for the frame `frames_in_flight` frames back, resets that frame's pools and hands
  it out; `frame::pool(worker, kind)` is the per-thread allocator; `frame::end(point)` records what
  the next `begin` on that slot will wait for. Forgetting `end` is handled conservatively — the
  frame is closed against everything outstanding, with a warning — rather than by resetting pools
  the GPU may still be reading.
- `src/rendering/detail_sync.hpp` — one `std::shared_mutex` shared by the public layer and whichever
  backend was linked. Exclusive for creation, destruction, submission and anything else that mutates
  device-wide state; shared for handle resolution and the whole recording path, which touches only
  the list's own record. `queue_wait` and `wait_idle` are called *unlocked* and take the lock
  themselves around the bookkeeping either side of the block, because holding even a shared lock
  across a frame-length wait would stall every other thread for exactly that long.
- `tests/rendering/test_frames.cpp` — CTest `catalyst.rendering.frames`, seven cases including four
  threads recording into four pools of one frame, and four threads creating and destroying buffers
  at once.

**Measured.** The frame benchmark grew `--in-flight N`, so the change can be measured against the
behaviour it replaced. On an Arc 140V, offscreen, release, 600 frames:

| | `--in-flight 1` | `--in-flight 3` |
|---|---|---|
| dynamic instanced, 10k quads | 5,282 FPS, sync med 0.119 ms | 17,818 FPS, sync med 0.0006 ms |
| per-draw, 10k quads | 4,485 FPS, sync med 0.161 ms | 21,156 FPS, sync med 0.0005 ms |

`--in-flight 1` is the old behaviour with the wait made visible: one list, and the CPU blocked on
the GPU every frame. The `sync` column is the whole of the difference — it is the same wait, moved
from inside `begin_recording` to a call whose name says it waits, and then paid three frames apart
instead of every frame.

### Tier 4 — Transfers ✅ implemented

`transfer.hpp`: a staging ring sized in `device_desc`, `transfer_batch`, async `upload`, `download`
returning something awaitable, and back-pressure as `error_code::staging_exhausted`. The inline
`begin_immediate` / `end_immediate` stall leaves `write_buffer`, `create_buffer` and `create_texture`
entirely.

What landed:

- `include/catalyst/rendering/transfer.hpp` — `transfer_batch` (move-only; accumulates uploads,
  submits once on the copy queue, returns the `timeline_point` the data becomes resident at),
  `upload` for the one-shot case, `readback` and `download` for the other direction, and
  `get_staging_info` so a caller can see the budget it is spending.
- The staging ring: one host-visible allocation sized by `device_desc::staging_ring_bytes`
  (16 MiB by default), carved front-to-back with absolute counters and recycled behind the copy
  queue's timeline. An allocation that would straddle the end pads to the start, because a copy
  region has to be contiguous. Full means `error_code::staging_exhausted` — a value the caller can
  respond to — rather than a stall it cannot see. The old single grow-on-demand buffer survives only
  for swapchain setup and teardown, the two places that are synchronous by nature.
- `readback` is not carved from the ring. It owns a host-visible buffer of its own, because it lives
  until the caller has read it and that is the one lifetime "recycle behind the timeline" cannot
  express. Its destructor defers the release, so dropping one whose copy is still running is legal
  and free.
- **Ordering, which is the part that had to be got right.** Read-after-write is closed by
  `device_state::last_transfer`: every submission on a queue other than copy waits on the most recent
  transfer, on the GPU, and the wait is skipped once that transfer has completed. So `write_buffer`
  followed by a draw that reads the buffer is correct with no caller-side synchronisation — exactly
  as it was when the write blocked. Write-after-read is closed the other way, by ordering a transfer
  after work that might still be reading its target; and *that* wait is skipped entirely for a
  resource nothing has been submitted against since it was created, which is the streaming case and
  the one where overlapping with the frame matters.
- Buffers and images are now created with `VK_SHARING_MODE_CONCURRENT` across the distinct queue
  families, where the adapter has more than one. This closes a hole Tier 2 opened rather than one
  Tier 4 created: the moment `compute` and `copy` became real families, an exclusive resource
  written on one and read on another needed an explicit ownership transfer that nothing in the
  backend tracks. Concurrent is the correct-by-construction answer for a backend whose hazard model
  is still one full barrier per pass; narrowing it belongs with Tier 6's explicit resource states.
- `read_buffer` still blocks, deliberately. It hands back bytes, so it has nowhere to put the
  waiting; `download` is the form that does not, and the header says so.
- `tests/rendering/test_transfer.cpp` — CTest `catalyst.rendering.transfer`, eleven cases. The
  assertion throughout is the bytes: upload a known pattern, read it back, compare. Exhaustion and
  wrap-around are exercised through *textures* rather than buffers, because a GPU-only buffer on a
  unified-memory adapter is host-visible and its upload is a memcpy that never touches the ring —
  correct, and useless for testing the ring.
- Both new test executables honour `CATALYST_RENDERING_VALIDATION=1`, which turns on the Vulkan
  validation layers and routes what they say through the module's own logging. The tiers that touch
  synchronisation are exactly the ones whose mistakes a functional test cannot see.

**What this did and did not buy on the machine it was measured on.** The CPU stall is gone: nothing
on the upload path calls `vkWaitForFences` any more, and a level load is one submission rather than
N round trips. The bandwidth figures in the resource benchmark did not move, because the Arc 140V
has unified memory — `gpu_only` buffers there are host-visible, so `write_buffer` was already a
direct `memcpy` and never went near the staging path at all. The adapters this tier is worth the
most to are the discrete ones, which are not the ones to hand.

### Tier 5 — RAII surface

`device::open`, `swapchain::open`, move-only, destructors that close, deferred destruction keyed on
the timeline point that last used a resource. Held to the end deliberately: it touches every call
site, and it is worth doing once, after the shape below it has stopped moving.

The two types Tiers 3 and 4 added are already in that shape — `frame_ring` and `transfer_batch` are
move-only with destructors that close, and `readback` owns its buffer — because they are new and
have no call sites to migrate. Everything older is still `create_*` / `destroy_*`, which is why the
sample code above reads as `create_command_list(...)` rather than the `device::open` form the
sketch at the top of this document uses. That sketch is where Tier 5 arrives, not where the module
is now.

### Tier 6 — Beyond

The last of the four stalls from review point 1: `acquire_next_image` still blocks twice without a
timeout, which is the one place a caller cannot yet choose to skip a frame rather than wait for the
compositor. Explicit resource states / barrier batching in place of the current conservative full
barrier before every pass, dispatch and copy — which would also let images go back to
`VK_SHARING_MODE_EXCLUSIVE` with real ownership transfers, and let a texture live in a layout better
than `GENERAL`; async compute as a first-class scheduling
story rather than just an available queue; the D3D12 and Metal backends, which are stubs today; and
bindless descriptors, which the current four-fixed-set model forecloses.

---

## Deliberately deferred, with reasons

- **A render graph.** It is the right answer to automatic barriers and transient resource aliasing,
  and it is a module, not a feature. It also cannot be designed sensibly on top of an API that
  cannot express "this finishes after that" — which is exactly what Tier 2 adds. Afterwards, not
  before.
- **A job system / thread pool.** Rendering will not ship one. Applications that need one have one,
  and a renderer that starts threads the caller did not ask for is the pattern `audio` removed when
  it stopped calling back on the driver thread. `events::task` plus `pump()` is the seam; anything
  more belongs in `catalyst::core`.
- **Automatic residency and sub-allocation.** The Vulkan backend allocates once per buffer and per
  texture and reports `max_memory_allocation_count` so callers can see the ceiling
  (`device.hpp:44-47`). A real allocator is worth doing and is orthogonal to everything here.
- **Multi-adapter / explicit device groups.** Not until one device is correct under threads.

---

## Conventions

- Headers under `include/catalyst/rendering/`, implementation under `src/rendering/`, backends under
  `src/rendering/<backend>/` behind `src/rendering/detail_backend.hpp`.
- Tests under `tests/rendering/`, one executable per file, CTest name `catalyst.rendering.<name>`.
  The null backend's window-less swapchain is what CI exercises, the same way `offline_stream` lets
  audio test the code that will play.
- `#include <catalyst/rendering/rendering.hpp>` pulls in the module.
- Resource ids are allocated from a monotonic counter and never recycled, so a stale handle resolves
  to nothing rather than aliasing a new resource. Backends must not introduce a free list.
- Clip space is +Y up with depth in [0, 1] on every backend; viewports and scissors are in
  framebuffer pixels, origin top-left. Unchanged.
- Every function that can block says so in its `@details`, names the thread it blocks, and offers a
  non-blocking form. A function with no such note does not block.
- Creation and destruction may happen on any thread and are serialised by the module lock; recording
  is per `command_pool` and therefore per thread, and holds only a shared lock; `submit` may be
  called from any thread and is serialised; `pump()` belongs to whichever thread the application
  picks and must be the same one each time. `destroy_device` must not race anything else on that
  device. See device.hpp, which states all of this where a caller will find it.
- A backend is called with the module lock held and needs no synchronisation of its own. The two
  exceptions are `queue_wait` and `wait_idle`, which block and therefore lock internally around the
  bookkeeping rather than across the wait; both are marked in detail_backend.hpp.
- Every function that can block says so, and after Tier 3 there are three of them in a frame loop:
  `frame_ring::begin`, `timeline_point::wait` / `wait_for`, and `read_buffer`. Nothing else waits.
- A command list is submitted to a queue of its own kind. Not a policy: a command buffer belongs to
  the family its pool came from. Adapters with fewer engines than kinds alias the queues internally,
  so caller code never branches on it.
- Rendering owns event tag block `0x0003'0000`–`0x0003'FFFF`, next after audio. Values are never
  reused or reordered.
