# Catalyst UI subsystem

Status: Tier 1 implemented (2026-09-02). Tiers 2+ are planned.

## Shape of the subsystem

`catalyst::ui` is a **retained, declarative** UI module. Applications build a persistent
tree of nodes, give each node a `style` expressed in CSS-like units, and ask the module to
lay it out. The module then emits a backend-agnostic draw list; it never talks to a GPU
itself.

```
    app                catalyst_ui                       catalyst_ui_renderer
  ┌───────┐   style   ┌──────────────────────────────┐   ┌──────────────────────┐
  │ tree  │──────────▶│ layout → paint → render_batch│──▶│ catalyst::rendering  │
  │ edits │           │        ▲            ▲        │   │ pipelines / buffers  │
  └───────┘           │  measure_fn    font/glyph    │   └──────────────────────┘
      ▲               │   (text seam)    provider    │
      │  ui events    └──────────────────────────────┘
      └───────────────────── hit-test / focus routing
```

### Why these four choices

- **Retained + declarative.** `measurement.hpp` already models the CSS unit system
  (`px`/`dp`/`em`/`rem`/`%`/`vw`/`vh`/`in`/`cm`/`mm`, `auto`, and `calc`-style linear
  combinations). An immediate-mode design would leave most of that unused. A retained tree
  also gives us stable identity for focus, hit-testing, animation and accessibility.
- **Draw list, not direct GPU calls.** `catalyst_ui` depends only on `catalyst::math`
  (and `catalyst::core` for events, from Tier 3). Layout and paint are testable headless, with
  no device. A separate optional `catalyst_ui_renderer` target links `catalyst::rendering`
  and submits the batch. This keeps the module split the rest of the repo already follows.
- **Text seam now, text engine later.** Layout asks a node for its intrinsic size through a
  `measure_fn` callback. Text is just a node with a measure callback, so the real font stack
  can land in Tier 4 without touching the layout engine.
- **Box-sizing defaults to `border_box`.** `width`/`height` include padding and border. This
  differs from CSS's `content_box` default; it is what UI code almost always wants, and
  `box_sizing::content_box` is available per node.

## Tiers

### Tier 1 — Layout core ✅ implemented

| Header | Contents |
| --- | --- |
| `ui/measurement.hpp` | (pre-existing) `length`, `calc_measure`, `resolve_context`, `resolve_or`, unit literals |
| `ui/geometry.hpp` | `point`, `extent`, `rect` aliases over `catalyst::math`; `edges<T>`, `corners<T>`; `inflate`/`deflate` |
| `ui/color.hpp` | linear float RGBA `color`, `rgba8` packing, `lerp`, named `colors` |
| `ui/style.hpp` | `display_mode`, `position_mode`, `box_sizing`, `flex_direction`, `justify`, `align`, `overflow_mode`, and the `style` struct |
| `ui/node.hpp` | `node` handle (index + generation), `layout_result`, `measure_fn` seam, `class tree` |
| `ui/layout.hpp` | `layout_params`, `layout()` — single-line flexbox + absolute positioning |

Implemented layout features: flex row/column (+ reverse), `flex_grow`/`flex_shrink`/
`flex_basis`, `justify_content` (all six), `align_items`/`align_self` (including `stretch`),
`gap`, margin/border/padding (percentages resolve against the containing block width, per
CSS), `min_*`/`max_*` clamping, both box-sizing modes, `display_mode::none`,
`position_mode::absolute` against the parent's padding box, and the
`measure_fn` intrinsic-size seam.

Deliberately deferred: flex wrap, baseline alignment, grid, and incremental (dirty-driven)
relayout. Dirty flags are already tracked, they are just not yet used to skip subtrees.

### Tier 2 — Painting

- Flesh out `ui/batch.hpp`: `vertex` (position, uv, packed color), index buffer,
  `draw_command` (index range, clip rect, texture key), and a `batch_builder` with a clip
  stack and `add_rect` / `add_rounded_rect` / `add_border` / `add_nine_slice`.
- `ui/paint.hpp`: traversal that walks a laid-out tree and emits background, border,
  rounded corners and `overflow` clipping into a batch.
- Dirty/damage rectangles so a static frame costs no vertex work.

### Tier 3 — Input and interaction

- `ui/hit_test.hpp`: point → deepest hit node, honoring clip and `overflow`.
- `ui/event.hpp`: pointer enter/leave/down/up/click/drag, wheel/scroll, key and text events,
  dispatched to a `catalyst::events::bus`, like every other event in the engine.
- Capture, focus tree and tab order; bridge from `catalyst::input` events.

### Tier 4 — Text

- `ui/text.hpp`: `font_id`, `font_metrics`, `glyph_metrics`, `shaped_run`, and a
  `text_provider` interface with a null provider that returns synthetic metrics for tests.
- A real provider: `stb_truetype` vendored in-tree, or platform backends (DirectWrite /
  CoreText / FreeType) behind `CATALYST_UI_TEXT_BACKEND`, mirroring the audio/input pattern.
- Glyph atlas, line breaking, `text_style` (size, line height, weight, align, wrap),
  wired into layout through `measure_fn`.

### Tier 5 — Renderer bridge

- New target `catalyst_ui_renderer` (`src/ui/renderer/`), guarded by
  `CATALYST_BUILD_UI_RENDERER`, linking `catalyst::ui` and `catalyst::rendering`.
- UI pipeline + shaders, dynamic vertex/index buffers, atlas texture upload, scissor from
  each `draw_command`'s clip rect.
- An `examples/ui_basics` app that lays out and draws a real window.

### Tier 6 — Widgets

Button, label, image, checkbox, radio, slider, text field, scroll view, list/virtualized
list, splitter. A `theme` that supplies default styles, plus style inheritance for font
properties.

### Tier 7 — Beyond

`catalyst::animation` integration for style transitions, accessibility metadata hooks,
flex wrap and grid layout, layout benchmarks in `bench/ui`.

## Conventions

- Headers under `include/catalyst/ui/`, implementation under `src/ui/`, tests under
  `tests/ui/` with one executable per test file and a `catalyst.ui.<name>` CTest name.
- `include <catalyst/ui/ui.hpp>` pulls in the whole module.
- Handles are index + generation, so a stale `node` is detected rather than aliasing a
  recycled slot.
