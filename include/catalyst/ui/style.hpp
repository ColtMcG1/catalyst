/**
 * @file style.hpp
 * @brief Defines the style properties that drive UI layout and painting.
 * @details A `style` is plain data attached to every node in a `tree`. Sizes are stored as
 * unresolved `length` measurements so that a style can be authored once in relative units and
 * resolved differently per DPI, font size or viewport. Layout reads the box and flex properties;
 * the painting tier reads the visual properties.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <cstdint>

#include <catalyst/ui/color.hpp>
#include <catalyst/ui/geometry.hpp>
#include <catalyst/ui/measurement.hpp>

namespace catalyst::ui
{

    /**
     * @enum display_mode
     * @brief Controls whether a node participates in layout at all.
     */
    enum class display_mode : std::uint8_t
    {
        /**
         * @brief The node lays its children out as a flex container. This is the only container mode in
         * Tier 1; a "block" layout is expressed as a flex container with `flex_direction::column`.
         */
        flex = 0,
        /**
         * @brief The node and its whole subtree are skipped: they take no space and produce no layout
         * result. This is distinct from a zero-sized or fully transparent node, which still occupies a
         * slot in its parent's flex line.
         */
        none,
    };

    /**
     * @enum position_mode
     * @brief Controls how a node is positioned within its parent.
     */
    enum class position_mode : std::uint8_t
    {
        /**
         * @brief The node is placed in its parent's flex flow, and its `inset` offsets it from that
         * position without affecting its siblings.
         */
        relative = 0,
        /**
         * @brief The node is taken out of the flex flow and positioned against its parent's padding box
         * using `inset` and its own size. Opposing insets with an `auto` size pin both edges and size the node.
         */
        absolute,
    };

    /**
     * @enum box_sizing
     * @brief Controls whether `width` and `height` describe the border box or the content box.
     * @details Catalyst defaults to `border_box`, unlike CSS. Sizing a UI element by the box you can
     * actually see is nearly always what is wanted, and it keeps padding from silently growing a
     * fixed-size control.
     */
    enum class box_sizing : std::uint8_t
    {
        /**
         * @brief `width` and `height` include padding and border. This is the default.
         */
        border_box = 0,
        /**
         * @brief `width` and `height` describe the content area only; padding and border are added on
         * top. This matches the CSS default.
         */
        content_box,
    };

    /**
     * @enum flex_direction
     * @brief Selects the main axis of a flex container and the direction items flow along it.
     */
    enum class flex_direction : std::uint8_t
    {
        /**
         * @brief Children are laid out left to right; the main axis is x.
         */
        row = 0,
        /**
         * @brief Children are laid out right to left; the main axis is x.
         */
        row_reverse,
        /**
         * @brief Children are laid out top to bottom; the main axis is y.
         */
        column,
        /**
         * @brief Children are laid out bottom to top; the main axis is y.
         */
        column_reverse,
    };

    /**
     * @enum justify
     * @brief Distributes children along a flex container's main axis once their sizes are known.
     */
    enum class justify : std::uint8_t
    {
        /**
         * @brief Children are packed against the start of the main axis.
         */
        start = 0,
        /**
         * @brief Children are packed against the end of the main axis.
         */
        end,
        /**
         * @brief Children are packed together in the middle of the main axis.
         */
        center,
        /**
         * @brief Leftover space is divided evenly between children, with none before the first or after the last.
         */
        space_between,
        /**
         * @brief Leftover space is divided so each child has equal space on both sides, halving at the ends.
         */
        space_around,
        /**
         * @brief Leftover space is divided evenly into gaps of equal size, including before the first and after the last child.
         */
        space_evenly,
    };

    /**
     * @enum align
     * @brief Positions children across a flex container's cross axis.
     */
    enum class align : std::uint8_t
    {
        /**
         * @brief Only valid for `align_self`: defer to the parent's `align_items`.
         */
        automatic = 0,
        /**
         * @brief The child is placed against the start of the cross axis.
         */
        start,
        /**
         * @brief The child is placed against the end of the cross axis.
         */
        end,
        /**
         * @brief The child is centered on the cross axis.
         */
        center,
        /**
         * @brief A child with an `auto` cross size is grown to fill the container's cross axis. A child with
         * a definite cross size behaves as `start`.
         */
        stretch,
    };

    /**
     * @enum overflow_mode
     * @brief Controls what happens to content that extends past a node's padding box.
     * @details Layout does not act on this in Tier 1; it is consumed by the painting tier, which
     * establishes a clip for anything other than `visible`.
     */
    enum class overflow_mode : std::uint8_t
    {
        /**
         * @brief Overflowing content is drawn outside the node.
         */
        visible = 0,
        /**
         * @brief Overflowing content is clipped to the padding box.
         */
        hidden,
        /**
         * @brief Overflowing content is clipped to the padding box and the node accepts scroll offsets.
         */
        scroll,
    };

    /**
     * @struct style
     * @brief The complete set of style properties for one node.
     * @details Every field has a default that makes an unstyled node behave predictably: an
     * auto-sized, transparent flex row that neither grows nor is offset. Sizes and spacing are
     * `length` values, so `auto` is a distinct state from zero. For `min_width`, `min_height`,
     * `max_width` and `max_height`, `auto` means "no constraint".
     */
    struct style
    {
        /**
         * @brief Whether this node participates in layout.
         */
        display_mode display = display_mode::flex;
        /**
         * @brief Whether this node is placed in its parent's flow or positioned against an ancestor.
         */
        position_mode position = position_mode::relative;
        /**
         * @brief Whether `width` and `height` include padding and border.
         */
        box_sizing sizing = box_sizing::border_box;
        /**
         * @brief How content that overflows this node's padding box is treated when painting.
         */
        overflow_mode overflow = overflow_mode::visible;

        /**
         * @brief The node's width. `auto` sizes it from its content, or from its flex container.
         */
        length width = auto_();
        /**
         * @brief The node's height. `auto` sizes it from its content, or from its flex container.
         */
        length height = auto_();
        /**
         * @brief A lower bound on the resolved width. `auto` means no lower bound.
         */
        length min_width = auto_();
        /**
         * @brief A lower bound on the resolved height. `auto` means no lower bound.
         */
        length min_height = auto_();
        /**
         * @brief An upper bound on the resolved width. `auto` means no upper bound.
         */
        length max_width = auto_();
        /**
         * @brief An upper bound on the resolved height. `auto` means no upper bound.
         */
        length max_height = auto_();

        /**
         * @brief Space reserved outside this node's border box, separating it from its siblings.
         */
        edges_length margin{};
        /**
         * @brief The width of this node's border, between its padding box and its border box.
         */
        edges_length border{};
        /**
         * @brief Space between this node's border and its content.
         */
        edges_length padding{};

        /**
         * @brief Offsets from the corresponding sides of the containing block.
         * @details For `position_mode::absolute` these place the node against its ancestor's padding
         * box, and an `auto` side means "let the opposite side and the size decide". For
         * `position_mode::relative` they shift the node from its flow position without moving siblings.
         */
        edges_length inset{.left = auto_(), .top = auto_(), .right = auto_(), .bottom = auto_()};

        /**
         * @brief The direction children flow along this container's main axis.
         */
        flex_direction direction = flex_direction::row;
        /**
         * @brief How leftover main-axis space is distributed between children.
         */
        justify justify_content = justify::start;
        /**
         * @brief How children are placed on the cross axis by default.
         */
        align align_items = align::stretch;
        /**
         * @brief How this node is placed on its parent's cross axis, overriding the parent's `align_items`.
         */
        align align_self = align::automatic;

        /**
         * @brief The share of positive leftover space this node claims from its flex container.
         */
        float flex_grow = 0.0f;
        /**
         * @brief The share of any main-axis overflow this node absorbs by shrinking, weighted by its base size.
         */
        float flex_shrink = 1.0f;
        /**
         * @brief The node's main-axis size before growing or shrinking. `auto` falls back to `width` or
         * `height` on the main axis, and then to the node's content size.
         */
        length flex_basis = auto_();

        /**
         * @brief Space inserted between adjacent children along the x axis.
         */
        length column_gap{};
        /**
         * @brief Space inserted between adjacent children along the y axis.
         */
        length row_gap{};

        /**
         * @brief The font size for this node, which also defines what `em` resolves to for its own
         * properties and those of its children. `auto` inherits the parent's font size.
         */
        length font_size = auto_();

        /**
         * @brief The color painted across this node's border box, behind its content.
         */
        color background = colors::transparent;
        /**
         * @brief The color painted in this node's border region.
         */
        color border_color = colors::transparent;
        /**
         * @brief The radius applied to each corner of this node's border box.
         */
        corners_length border_radius{};
        /**
         * @brief A multiplier applied to the alpha of this node and its subtree when painting.
         */
        float opacity = 1.0f;

        /**
         * @fn set_gap
         * @brief Sets both the row and column gap to the same measurement.
         * @param g The gap to apply on both axes.
         */
        constexpr void set_gap(const length &g) noexcept
        {
            column_gap = g;
            row_gap = g;
        }
    };

} // namespace catalyst::ui
