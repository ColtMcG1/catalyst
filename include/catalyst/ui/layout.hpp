/**
 * @file layout.hpp
 * @brief The UI layout engine: turns a styled node tree into positioned, sized boxes.
 * @details Layout is a single free function over a `tree`. It resolves every measurement in the
 * subtree against a context, runs a single-line flexbox pass over each container, positions
 * absolutely positioned children against their parent, and writes a `layout_result` onto every node.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/ui/geometry.hpp>
#include <catalyst/ui/measurement.hpp>
#include <catalyst/ui/node.hpp>

namespace catalyst::ui
{

    /**
     * @struct layout_params
     * @brief The inputs a layout pass needs beyond the tree itself.
     * @details `context` supplies everything measurements resolve against that is not per-node: DPI,
     * the root font size and the viewport dimensions. Its `parent_width_px` and `parent_height_px`
     * are overwritten per node by the engine, so they do not need to be set.
     */
    struct layout_params
    {
        /**
         * @brief The measurement context for the pass. Set DPI, root font size and viewport here.
         */
        resolve_context context{};

        /**
         * @brief The space offered to the root node's margin box, in pixels.
         */
        extent available{};

        /**
         * @brief Whether `available.x` is a definite size the root must fit into rather than a hint.
         */
        bool width_definite = true;

        /**
         * @brief Whether `available.y` is a definite size the root must fit into rather than a hint.
         */
        bool height_definite = true;

        /**
         * @brief The absolute position the root node's margin box starts at, in pixels.
         */
        point origin{};

        /**
         * @fn for_viewport
         * @brief Builds parameters that lay a root node out to fill a viewport.
         * @details Sets the viewport and available size to the given extent, the DPI from the given
         * scale, and leaves the root font size at its default.
         * @param viewport The viewport size in pixels.
         * @param dpi_scale The display scale factor, where 1 is 96 DPI.
         * @return Parameters filling the viewport at the given scale.
         */
        [[nodiscard]] static layout_params for_viewport(extent viewport, float dpi_scale = 1.0f) noexcept
        {
            layout_params p{};
            p.context.dpi_scale = dpi_scale;
            p.context.dpi_x = dpi_scale * 96.0f;
            p.context.dpi_y = dpi_scale * 96.0f;
            p.context.viewport_width_px = viewport.x();
            p.context.viewport_height_px = viewport.y();
            p.available = viewport;
            return p;
        }
    };

    /**
     * @fn layout
     * @brief Lays out a node and its subtree, writing a `layout_result` onto every node it visits.
     * @details Each container runs a single flex line along its `flex_direction`. Children are sized
     * from their `flex_basis`, then grown or shrunk to fill the container's main axis, clamped by
     * their `min_*`/`max_*` measurements, distributed by `justify_content` and aligned by
     * `align_items`/`align_self`. A node with a measure callback is treated as a leaf and asked for
     * its content size. Children using `position_mode::absolute` are taken out of the flow and placed
     * against their parent's padding box using `inset`.
     *
     * Percentages in `margin`, `border` and `padding` resolve against the containing block's width on
     * all four sides, as in CSS. `width` and `height` describe the border box unless the node opts
     * into `box_sizing::content_box`.
     *
     * After the pass every visited node's `layout_result::position` is absolute, and the subtree's
     * dirty flags are cleared.
     * @param t The tree that owns the nodes.
     * @param root The node to lay out. It may be any node, not only a parentless one.
     * @param params The available space, origin and measurement context for the pass.
     */
    void layout(tree &t, node root, const layout_params &params) noexcept;

} // namespace catalyst::ui
