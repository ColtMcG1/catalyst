# Catalyst resource subsystem

Status: Tier 1 (naming, sourcing, ownership) implemented 2026-09-11; Tier 2 (loaders and image
decoders) implemented 2026-09-12. The document sub-modules (`json`, `csv`, `uri`) were already in
the build and are unchanged. This document is the plan and the record of what has landed.

The short version: the module already knew how to *parse* a document and how to *name* a thing. It
had no answer to "give me the bytes this name points at" or "who is keeping this 80 MB texture
resident"; Tier 1 is those two answers. Tier 2 is the third, "turn those bytes into a thing" --
a `loader<T>` seam so that is one call, image decoders behind it for both source formats and cooked
containers, and a mip filter for the images that arrive without a chain.

---

## The constraint that shaped it

**The renderer already defines pixel formats and resource handles, and this module does not get a
second copy of them.**

`include/catalyst/rendering/types.hpp` holds `resource_handle<Tag>`, `resource_id`, `format`,
`format_size_bytes`, `extent3d` and the `flags_enum` operators. A parallel
`resource::image_format { rgba8, rgb8, rgba16f, ... }` would have bought a translation function, a
set of formats no backend can actually sample (`rgb8` is in none of their intersections), and two
enums to keep in step every time a format is added. So `resource::image` stores a
`rendering::format`, and `resource/handle.hpp` re-exports the handle machinery rather than restating
it.

That reuse costs nothing at link time. `rendering/types.hpp` is `constexpr` enums and templates with
no external linkage, so including it is an include-path dependency and **not** a link dependency:

```
catalyst_resource ──(include only)──► catalyst/rendering/types.hpp
                  ──(links)─────────► nothing but the standard library

catalyst_resource_gpu ──(links)─────► catalyst::rendering, catalyst::resource     [Tier 3]
```

`catalyst_resource` builds and its tests pass with `CATALYST_BUILD_RENDERING=OFF`. A headless tool
that wants JSON and URIs does not drag in Vulkan or D3D12. The one thing that genuinely needs a live
device — turning a decoded `image` into a `rendering::texture` — goes in a separate target in Tier 3,
where linking the renderer is honest rather than incidental.

The cost of the choice, stated once: opting a `catalyst::resource` flag enum into the shared
operators means specialising a variable template in the namespace it was declared in, so
`source.hpp` ends with a three-line `namespace catalyst::rendering { template <> ... }` block. That
is the whole tax, and it beats maintaining a second copy of the operators.

**The choice bought its keep in Tier 2, and also sent work the other way.** KTX2 and DDS exist to
carry block-compressed texels, and `rendering::format` had no BC formats -- so rather than inventing
`resource::compressed_format`, which is the second enum this module spent choice 1 avoiding, the
fourteen BC entries went into `rendering::format` where they belong. Along with them went the
arithmetic they break:

| Added to `rendering/types.hpp` | Why |
| --- | --- |
| `bc1_rgba_*` .. `bc7_*` | 14 enumerators, appended, so nothing renumbers. Vulkan's `BC1_RGB_*` folds onto the RGBA spelling: identical blocks, and the difference is only the alpha a sampler is promised. |
| `is_block_compressed`, `format_block_width` / `_height` / `_size_bytes` | A block format's unit is a 4x4 block, not a texel. Width and height are separate even though all fourteen are square, because the families that would come next are not. |
| `format_image_size_bytes(format, extent3d)` | The size computation that is correct for **both** kinds. Block dimensions round up, so a 5x5 BC7 surface occupies the 2x2 blocks an 8x8 one does. |
| `to_srgb_format` | A spelling change, not a conversion. It exists so a caller who knows an asset is colour -- which no container reliably states -- can say so without a switch. |

`format_size_bytes` now answers **0** for every block format. Deliberately: the expression it exists
to be multiplied into is wrong by the block area for those, and 0 turns each such site into a
visibly empty allocation rather than one silently 16x too small. The two places in the renderer that
computed a texture's size that way -- `vulkan_backend.hpp` and `null_resources.cpp` -- were moved
onto `format_image_size_bytes`, which is what makes the 0 safe rather than a trap laid for Tier 3.

---

## Shape of the subsystem

```
                    ┌───────────────────── catalyst::resource ─────────────────────┐
                    │                                                              │
  a name ───────────┤  "../textures/stone_d.png"                                   │
                    │        │                                                     │
                    │        ▼                                                     │
                    │  vfs::resolve ──► uri   (resolved against base, normalized)   │
                    │        │            └─ the canonical spelling, and the        │
                    │        │               cache key: two spellings, one entry    │
                    │        ▼                                                     │
                    │  vfs mount table ── most specific mount wins                  │
                    │        │   asset://core/ ─► pack     asset:// ─► on disk      │
                    │        ▼                                                     │
                    │  source::open ──► std::expected<blob, error>                  │
                    │        │   file_source  memory_source   [pack_source: Tier 4]  │
                    │        │                                                     │
                    │  vfs::read_async ──► events::task<expected<blob, error>>       │
                    │        │   Tier 1: completes inline. Tier 3: worker pool.     │
                    │        ▼                                                     │
                    │  loader<T>::decode ──► T                                     │
                    │        │   image (stb / KTX2 / DDS) -- mesh, material later   │
                    │        ▼                                                     │
                    │  registry<T>::insert ──► asset_handle<T>                      │
                    │        │   generation:32 | index:32, refcounted               │
                    │        │                                                     │
  a scene holds ────┼──► retain / release ── the strong reference, owned explicitly  │
                    │                                                              │
  store ────────────┤  one registry per type, so the app holds one object           │
                    └──────────────────────────────────────────────────────────────┘
                                             │
                     [Tier 3] catalyst_resource_gpu: image ──► rendering::texture
```

### The four choices that define it

**1. A URI is the only name an asset has.** Not a path, not a string id, not an integer baked by a
cooker. `catalyst::resource::uri` was already here and already does the two hard parts — `resolve`,
so a manifest saying `../shared/x.json` means what its author meant, and `normalized`, so
`asset:/a/./b` and `asset:/a/b` are one cache entry rather than two copies of a texture. Everything
above the vfs names things this way and nothing above the vfs knows whether the bytes came from a
directory, a pack, or a table in a test.

**2. Mounting is the whole of the shipped/editor difference.** The editor mounts the artist's working
tree on `asset:`, a shipped build mounts a sealed pack on the same scheme, a test mounts
`make_memory_source({{"meshes/hull.bin", bytes}})`. Mounts are consulted most-specific first and an
equally specific later mount shadows an earlier one, so a patch pack over a base pack does what it
looks like. No loader changes by a line across the three.

**3. A handle is a weak reference; the strong one is counted and owned by something nameable.**
`registry::insert` starts an asset at one reference, `retain` adds, `release` drops, and zero
destroys. Holding an `asset_handle` keeps nothing alive — resolve it every time and treat `nullptr`
as an ordinary answer. That is deliberate: a `shared_ptr`-shaped API makes "who is keeping this 80 MB
texture resident?" unanswerable, and a scene, a level or a material is exactly the thing that should
be answering it.

**4. Slots are recycled and generations tell the occupants apart.** This is the one place the asset
system deviates from the rendering module's convention, which allocates ids from a monotonic counter
and forbids backends a free list. Rendering has a bounded set of GPU objects and a 64-bit counter it
will never exhaust; a registry wants a dense array it can index in O(1) and a level load churns
thousands of assets, so it recycles indices and disambiguates with a 32-bit generation. The
*observable* behaviour is identical and is the point of both schemes: a stale handle resolves to
nothing rather than aliasing whatever moved into the slot.

---

## Headers

| Header | What is in it |
| --- | --- |
| `resource/error.hpp` | `error_code`, `error`, `make_error`. The `std::expected` failure value. |
| `resource/handle.hpp` | Re-exports of `resource_handle` / `format` / flag operators; `asset_handle<T>` and the index/generation packing. |
| `resource/blob.hpp` | Move-only owning byte range with a pluggable releaser. Allocating, borrowing and (later) mapped reads all hand back one type. |
| `resource/source.hpp` | The `source` interface, `source_caps`, `entry_info`; `make_file_source`, `make_memory_source`. |
| `resource/vfs.hpp` | `mount_point`, `vfs`: scheme dispatch, `resolve`, `read`, `read_async`, `stat`, `list`. |
| `resource/registry.hpp` | `registry<T>`, `asset_info`, `store`. Implementation in `detail/registry_impl.hpp`. |
| `resource/loader.hpp` | The `loader<T>` seam, `load_context`, the `loadable` concept, and `load<T>` / `load_async<T>`. Implementation in `detail/loader_impl.hpp`. |
| `resource/image.hpp` | The CPU image asset, described in `rendering::format` / `rendering::extent3d`; `load_image` and its options; `generate_mips`; the mip arithmetic (`mip_extent`, `max_mip_levels`, `packed_size_bytes`); and the `loader<image>` specialisation. |
| `resource/json/` `csv/` `uri/` | Unchanged. The document formats assets are written in. |

---

## What it reads like

```cpp
using namespace catalyst::resource;

vfs files;
files.mount({.scheme = "asset"}, make_file_source("C:/game/assets"));
files.mount({.scheme = "asset", .path_prefix = "/core"}, make_pack_source("core.pak")); // Tier 4

store assets;

// Tier 1: name it, read it, own it.
auto key = files.resolve("asset:/textures/stone_d.png").value();
auto bytes = files.read(key).value();

auto &images = assets.registry_for<image>();
if (auto existing = images.acquire(key))
    return existing;                          // already resident, now retained

auto handle = images.insert(key, load_image(bytes).value()).value();

// ...and later, from whoever owned the reference:
images.release(handle);
```

```cpp
// Tier 2: the same four steps, as one call. The handle carries one reference you own, whether it
// loaded the asset or found it already resident.
auto stone = load<image>(files, assets, "asset:/textures/stone_d.ktx2");
if (!stone)
    logging::warn<res>("{}", stone.error().message());   // ...and the message names the asset

// A source format, wanted as colour and sampled minified, so ask for both.
auto icon = load<image>(files, assets, "asset:/ui/icon.png", {.srgb = true, .generate_mips = true});

assets.registry_for<image>().release(*stone);
```

```cpp
// Streaming a level without blocking the frame loop. The signature is Tier 1; the worker pool
// behind it is Tier 3, and this caller does not change when it lands.
events::task<void> load_level(vfs &files, store &assets, std::vector<uri> names)
{
    for (const uri &name : names)
    {
        auto bytes = co_await files.read_async(name);
        if (!bytes)
        {
            logging::warn<res>("{}", bytes.error().message());
            continue;
        }
        // ... decode, insert ...
    }
}
```

---

## Tiers

### Tier 1 — Naming, sourcing, ownership ✅ implemented

The layer everything else sits on: a failure value, a byte source behind the URI namespace, and a
registry that can tell a live handle from a stale one. No decoders, no threads, no GPU.

What landed:

- `include/catalyst/resource/error.hpp` + `src/resource/error.cpp` — twelve codes. The two that earn
  their place immediately are `no_such_mount` and `not_found`, which a loader must tell apart:
  the first means the build was configured wrong, the second means a content author typo'd a path.
- `include/catalyst/resource/handle.hpp` — the re-export layer and `asset_handle<T>`. Deleted the
  verbatim copy of `rendering::resource_handle` and the flag-enum operators that a draft had put
  here under a bare `namespace resource`.
- `include/catalyst/resource/blob.hpp` + `src/resource/blob.cpp` — `adopt` (allocating) and `borrow`
  (bytes that outlive the blob). Not `std::vector<std::byte>`, because that would force the mapped
  and resident paths to copy, and for a 200 MB pack that copy is the whole cost of loading it.
- `include/catalyst/resource/source.hpp` + `src/resource/source.cpp` — the interface plus the file
  and memory sources. The file source refuses a path that climbs out of its root with `..`: content
  names files, and content does not get to name files outside its mount.
- `include/catalyst/resource/vfs.hpp` + `src/resource/vfs.cpp` — the mount table, kept sorted by
  descending specificity so dispatch is a linear scan that takes the first match. Prefixes match on
  whole path segments, so `/core` claims `/core/x` and not `/core_extra/x`.
- `include/catalyst/resource/registry.hpp` + `detail/registry_impl.hpp` — `registry<T>` and `store`.
  Inserting a name that is already loaded retains the existing asset and returns its handle rather
  than overwriting, so two loaders racing on one name converge; `replace` is the deliberate
  overwrite, and it keeps the slot, the generation and the count, which is what makes hot reload a
  one-liner later.
- `include/catalyst/resource/image.hpp` + `src/resource/image.cpp` — the asset type and its mip
  arithmetic, depending on nothing. The decoder behind `load_image` is a separate file and part of
  Tier 2; see below.
- `tests/resource/test_assets.cpp` — CTest `catalyst.resource.assets`. Thirteen cases; the ones
  worth naming are stale-handle detection after slot reuse, double-release being a no-op, and
  specificity-ordered mount shadowing.

**Not thread-safe, and says so.** A `registry` is owned by one thread. Reads through a `vfs` are safe
from any thread; mounting is not. Both headers state it where a caller will find it. Locking every
`get` to allow otherwise would cost the hot path — resolving a handle — for a benefit only the
loader needs.

### Tier 2 — Loaders and decoders ✅ implemented

A `loader<T>` registration seam plus the image decoders behind it, so `load<image>(files, assets,
name)` is one call. The seam is what matters: the registry and the vfs must not learn what a PNG is,
and they do not -- neither header includes `loader.hpp`.

#### The seam

`loader<T>` is a class template with no primary definition. A type becomes loadable by specialising
it with one static `decode` and, optionally, an `options` type. That is a **compile-time**
registration, not a runtime `register_loader<T>(...)` table, and the three reasons are worth keeping:

- No global to initialise, so no static initialisation order to get wrong and no "loader not
  registered" that only happens in the shipped build.
- `load<T>` resolves to a direct call. Nothing sees a `std::function` and the decode is inlinable.
- Asking for a type with no loader is a compile error naming the type at the call site.

The cost is the one thing a table buys: a plugin the application did not compile against cannot add
a loader. When that is genuinely needed it is a `loader<T>` specialisation dispatching through a
table of its own -- a decision belonging to the one type that needs it, not to every type.

The member is `decode` and not `load` for a concrete reason: a member named `load` hides the
namespace-scope `load` from every expression inside it, so a loader calling `load<image>` for a
dependency would need full qualification or fail to compile depending on the compiler. MSVC and
clang disagreed about exactly that during implementation.

**What `load<T>` guarantees.** Four steps -- `registry<T>::acquire`, `vfs::read`,
`loader<T>::decode`, `registry<T>::insert` -- and one contract: **the handle you get back carries one
reference you own**, on the cache-hit path as much as on the miss. A scene loading the same texture
twice releases it twice and the count balances. That symmetry is why `acquire` exists as its own
operation rather than being spelled `find`.

The name is the identity, and options do not vary it: loading one URI twice with different options
hands back the first result both times. That follows from choice 1 above and is the honest behaviour
rather than a limitation to route around -- an asset needing two treatments needs two names.

**A loader may load.** `load_context` hands a loader the same `vfs` and `store` the call started
from, plus `load_context::resolve`, which resolves a reference *against the asset's own name* rather
than against `vfs::base`. That distinction is the classic dependency bug: it works until two
materials in different directories name the same relative path. Cycle detection for a declared
dependency graph is still deferred.

#### The decoders

One entry point, `load_image`, which sniffs the container from its magic number -- not from a file
extension, because the vfs deals in URIs a pack source may have stripped of any suffix.

| Leading bytes | Reader | Yields |
| --- | --- | --- |
| `AB 4B 54 58 20 32 30 ...` | KTX2 | the file's chain, array layers and cube faces |
| `DDS ` | DDS | the file's chain and array slices |
| anything else | stb_image | one uncompressed level |

- **stb_image** (`image_stb.cpp`) — PNG, JPEG, BMP, TGA, PSD, GIF, PIC, PNM as 8-bit; Radiance
  `.hdr` as `rgba32_float`. Fetched at a pinned commit by `cmake/stb.cmake`, with `STBI_NO_STDIO` on
  the interface target so a decoder that could open paths of its own is a compile error rather than
  a convention. Every decode calls `stbi_info_from_memory` first, which is what distinguishes
  `unsupported_format` (nothing recognised these bytes) from `decode_failed` (something did, then
  choked) -- a bare `stbi_load` answers null to both.
- **KTX2** (`image_ktx2.cpp`) — the substance is the transposition. KTX2 is level-major, every layer
  and face of level 0 then every layer and face of level 1; an `image` is layer-major, because that
  is the order `transfer_batch::upload` wants. Every level is checked against the size
  `rendering::format` says it must be *before* a byte is copied, which both bounds the allocation by
  the file's own size and stops a crafted level index from walking the copy loop out of bounds.
  Refused with `unsupported_format`: any supercompression (BasisLZ needs a transcoder that picks a
  target from device capabilities, which is a Tier 3 conversation; Zstandard and ZLIB are each a
  dependency), and `VK_FORMAT_UNDEFINED`, which means the texels are Basis Universal.
- **DDS** (`image_dds.cpp`) — no transposition; DDS is already layer-major. The work is the format,
  which DDS identifies three different ways depending on when the file was written: a FourCC, a set
  of channel bit masks, or a `DXGI_FORMAT` in a DX10 extension header. Legacy cube maps are counted
  by their face bits rather than assumed to be six, because partial cube maps are legal and shipped.
  **It does not guess**: a `DDPF_RGB` file with no alpha mask (`X8R8G8B8`) is refused rather than
  mapped onto `bgra8_unorm`, since its fourth byte is undefined and passing it through as alpha
  would make the image arbitrarily transparent with nothing in the log.

The KTX2 reader's `VkFormat` table is **not** a duplicate of `vulkan_convert.hpp`. That one
translates this project's enum to a backend's; this one reads a number out of a file. They coincide
because KTX2 chose to identify formats by Vulkan's enumerators, which is a fact about the file format
and stays true in a build with no Vulkan in it -- `catalyst_resource` links no renderer and includes
no Vulkan header.

#### Mip generation

`generate_mips` is a box filter down to 1x1, each level filtered from the one before. The filter is
the boring half; the two corrections are not, and neither is something a caller can apply afterwards:

- **`srgb_aware`, on by default.** sRGB texels are not proportional to light. Averaging the stored
  bytes of 0 and 255 gives 128, which is about 22% of the light the correct answer -- 188 --
  represents. Compounded down a nine-level chain a surface visibly darkens as it recedes, and it
  darkens differently from the surface beside it that kept level 0, which is what makes the artefact
  read as a lighting bug rather than a texture bug. Alpha is exempt: it is linear even in an sRGB
  format. `tests/resource/test_mips.cpp` asserts on 188 against 128 directly.
- **`alpha_weighted`, off by default.** A cutout texture's fully transparent texels usually hold
  something arbitrary, often black, and an unweighted average pulls it into the visible neighbours a
  little more at every level -- the dark fringe on minified foliage. It is off by default because it
  is wrong for an image whose alpha is data rather than coverage, and the caller knows which it has.

Block-compressed images are refused with `unsupported_format`: filtering them means decompress,
filter, re-encode, and a real BC encoder has quality settings of its own and belongs in a cooker. A
cooked container already carries the chain this would be rebuilding. `rgba16_float` is refused too,
because a half-float converter is code this module does not otherwise need.

`image_decode_options::generate_mips` wires it into the decode, and a container that already carries
a chain is never regenerated -- the flag asks for the levels a file did not have. A failure to
generate fails the whole decode rather than quietly returning one level, because a caller that asked
for a chain and got one level has a texture that shimmers and no indication why.

#### What `CATALYST_RESOURCE_STB=OFF` actually removes

Less than its name suggests, and this is checked rather than asserted. `image_stb.cpp` is the only
translation unit that includes stb; the container readers are this module's own code with no codec
behind them. So that build still reads every KTX2 and DDS, still generates mip chains, and still has
a working `loader<image>` -- it loses PNG, JPEG and the rest of the source formats, which is what an
editor reads and a shipped build should not be. Three of the four new test suites run there
unchanged.

#### Tests

| Suite | CTest name | Built when |
| --- | --- | --- |
| `test_container.cpp` | `catalyst.resource.container` | always |
| `test_mips.cpp` | `catalyst.resource.mips` | always |
| `test_loader.cpp` | `catalyst.resource.loader` | always |
| `test_image.cpp` | `catalyst.resource.image` | `CATALYST_RESOURCE_STB=ON` |

Fixtures are assembled field by field in `scripts/gen_image_fixtures.py` into two generated
includes, so a failure names a header field rather than reporting that a checksum moved. Every
container level's payload is a run of one byte identifying the level and layer it belongs to, which
makes "did the transposition put this surface where it belongs?" a single byte comparison.

The loader suite defines two asset types of its own -- a counter and a material -- precisely because
a seam that only worked for `image` would not be a seam. The material's loader loads its texture
through `load<image>`, which is what proves `load_context` carries what it claims to.

#### Still ahead in this tier

A JSON-backed material and mesh format, and the `reload<T>` that Tier 4's hot reload wants --
`registry::replace` is already the swap that keeps every outstanding handle valid, so it is a short
function once there is a watcher to call it.

### Tier 3 — Threads, and the GPU bridge

Two independent pieces that both want Tier 2 done first:

- **A worker pool behind `vfs::read_async`.** The signature is already the one callers write against;
  today the coroutine does the blocking read on whichever thread resumes it. This replaces the body
  and no caller changes. Decode goes on the worker too; `registry::insert` stays on the owning
  thread.
- **`catalyst_resource_gpu`.** The target that *does* link `catalyst::rendering`: `upload(dev, image)
  -> expected<rendering::texture, rendering::error>`, built on `rendering::transfer_batch` so a level
  load is one submit and a `timeline_point`, not one round trip per texture. It is a separate target
  precisely so the rest of the module stays headless.

### Tier 4 — Packs, budgets, hot reload

- **`make_pack_source`.** A sealed archive with an index, memory-mapped, handing out
  `blob::borrow` into the mapping — the case `blob`'s releaser exists for.
- **A budget-driven cache.** Built *on* the registry by releasing what it decides to drop, not woven
  into it. LRU over a byte budget per asset type.
- **Hot reload.** `source_caps::mutable_entries` and `entry_info::revision` are already there for the
  watcher; `registry::replace` is already the swap that keeps every outstanding handle valid.

---

## Deliberately deferred, with reasons

- **A cooker / offline pipeline.** The vfs and the pack format are the runtime half and settle first.
  A cooker that targets a pack format nothing reads yet is a guess.
- **Dependency graphs between assets** (a material pulling its textures, a scene pulling its meshes).
  Real, and it wants the loader seam to exist before it can be designed. Tier 2 makes a loader able
  to call back into `load`, which is most of it; a declared graph with cycle detection is later.
- **A thread pool of this module's own.** Same answer as rendering: applications that need one have
  one. `events::task` plus a pool the *application* supplies is the seam.
- **`shared_ptr`-flavoured handles.** See choice 3. The point of the counted API is that someone has
  to name the owner.
- **Asset ids baked at cook time.** A hashed id is a URI with the debugging removed. If profiling
  ever says the string comparison in `registry::find` matters, hash the normalized URI *inside* the
  registry and keep the URI as the name.

---

## Conventions

- Headers under `include/catalyst/resource/`, implementation under `src/resource/`. Template
  definitions in `include/catalyst/resource/detail/`, included at the bottom of their public header.
- Tests under `tests/resource/`, one executable per file, CTest name `catalyst.resource.<name>`.
  Add the suite to the `foreach(suite ...)` list in `tests/resource/CMakeLists.txt`.
- `#include <catalyst/resource/resource.hpp>` pulls in the module.
- Every fallible operation returns `std::expected<T, error>`. Nothing in this module throws.
- `catalyst_resource` links nothing but the standard library. Anything that needs a GPU device goes
  in `catalyst_resource_gpu`. If a header here needs a renderer type, it reuses the one in
  `rendering/types.hpp` — it does not restate it, and it does not create a link dependency.
- Asset names are `uri`, always resolved and normalized before they are used as a key. `vfs::resolve`
  is the only correct way to produce one.
- A handle is weak; a reference count is strong. A function that hands back a handle says in its
  `@details` whether it also took a reference — `find` does not, `acquire` does.
- Sources must be callable from several threads at once. Registries must not be assumed to be.
- Sources are ASCII-only, like the rest of the tree: there is no `/utf-8` flag, so a non-ASCII byte
  in a string literal breaks the MSVC build and not the clang-cl one.
