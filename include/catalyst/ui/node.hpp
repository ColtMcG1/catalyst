/**
 * @file node.hpp
 * @brief Defines the UI node handle, its layout result, and the tree that owns both.
 * @details A `tree` is the retained side of the UI module: applications create nodes, give them
 * styles and parent them to each other, and the layout engine writes a `layout_result` back onto
 * every node. Nodes are referred to by value through a `node` handle carrying an index and a
 * generation, so a handle to a destroyed node is detected rather than silently aliasing whatever
 * was created in its place.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <catalyst/ui/geometry.hpp>
#include <catalyst/ui/measurement.hpp>
#include <catalyst/ui/style.hpp>

namespace catalyst::ui
{

    /**
     * @struct node
     * @brief A handle to a node in a `tree`.
     * @details The handle is a value type: copying it is free and it does not keep the node alive.
     * `index` selects a slot in the owning tree and `generation` records which occupant of that slot
     * this handle refers to, so `tree::is_valid` can reject a handle to a node that has been
     * destroyed even after the slot has been reused.
     */
    struct node
    {
        /**
         * @brief The index of the node's slot in its owning tree.
         */
        std::uint32_t index = 0xFFFFFFFFu;
        /**
         * @brief The generation of the slot occupant this handle refers to.
         */
        std::uint32_t generation = 0u;

        /**
         * @brief Compares two handles. Handles are equal when they refer to the same occupant of the same slot.
         */
        [[nodiscard]] constexpr bool operator==(const node &other) const noexcept = default;
    };

    /**
     * @brief The handle value that refers to no node. `tree::is_valid` always rejects it.
     */
    inline constexpr node null_node{};

    /**
     * @fn is_null
     * @brief Reports whether a handle is the null handle.
     * @param n The handle to test.
     * @return True when `n` refers to no node at all. A non-null handle may still be invalid if the node it referred to has been destroyed.
     */
    [[nodiscard]] constexpr bool is_null(const node &n) noexcept { return n.index == null_node.index; }

    /**
     * @struct measure_input
     * @brief The space and context offered to a node's measure callback.
     * @details The layout engine passes this to leaf nodes that size themselves from content it
     * cannot see, most importantly text. A dimension is only meaningful as a hard limit when its
     * `*_definite` flag is set; otherwise it is an upper bound the callback may ignore.
     */
    struct measure_input
    {
        /**
         * @brief The content width available to the node, in pixels.
         */
        float available_width_px = 0.0f;
        /**
         * @brief The content height available to the node, in pixels.
         */
        float available_height_px = 0.0f;
        /**
         * @brief Whether `available_width_px` is a definite size the node must fit into rather than a hint.
         */
        bool width_definite = false;
        /**
         * @brief Whether `available_height_px` is a definite size the node must fit into rather than a hint.
         */
        bool height_definite = false;
        /**
         * @brief The measurement context for this node, including its resolved font size. A text
         * provider reads `font_px` from here.
         */
        resolve_context context{};
    };

    /**
     * @typedef measure_fn
     * @brief The callback a node uses to report its intrinsic content size.
     * @details This is the seam through which text, images and any other self-sizing content enter
     * layout without the layout engine depending on them. The returned extent is a content size, in
     * pixels; padding and border are added by the engine.
     * @param input The space and context offered to the node.
     * @param user The opaque pointer registered alongside the callback.
     * @return The node's content size in pixels.
     */
    using measure_fn = extent (*)(const measure_input &input, void *user) noexcept;

    /**
     * @struct layout_result
     * @brief The output of the layout engine for one node.
     * @details `position` is the absolute top-left of the node's border box once `layout` has
     * returned; while layout is running it holds a position relative to the parent. The resolved
     * `margin`, `border` and `padding` are kept so the box-model rectangles can be derived without
     * re-resolving measurements.
     */
    struct layout_result
    {
        /**
         * @brief The absolute top-left corner of this node's border box, in pixels.
         */
        point position{};
        /**
         * @brief The size of this node's border box, in pixels.
         */
        extent size{};
        /**
         * @brief The resolved margin around this node's border box.
         */
        edges_px margin{};
        /**
         * @brief The resolved border widths of this node.
         */
        edges_px border{};
        /**
         * @brief The resolved padding between this node's border and its content.
         */
        edges_px padding{};
        /**
         * @brief The font size in pixels that applied to this node, after inheriting from its parent.
         */
        float font_px = 16.0f;
        /**
         * @brief Whether the layout engine has produced a result for this node in the last pass.
         */
        bool laid_out = false;
        /**
         * @brief Whether this node was skipped because it, or an ancestor, used `display_mode::none`.
         */
        bool hidden = false;

        /**
         * @fn border_box
         * @brief Returns the rectangle covered by this node's border box, the box its background and border are painted into.
         * @return The border box rectangle.
         */
        [[nodiscard]] constexpr rect border_box() const noexcept { return rect::from_pos_size(position, size); }

        /**
         * @fn padding_box
         * @brief Returns the rectangle inside this node's border, the box its children and its overflow clip are placed against.
         * @return The padding box rectangle.
         */
        [[nodiscard]] constexpr rect padding_box() const noexcept { return deflate(border_box(), border); }

        /**
         * @fn content_box
         * @brief Returns the rectangle inside this node's padding, the box its in-flow children flow within.
         * @return The content box rectangle.
         */
        [[nodiscard]] constexpr rect content_box() const noexcept { return deflate(padding_box(), padding); }

        /**
         * @fn margin_box
         * @brief Returns the rectangle this node reserves from its siblings, including its margin.
         * @return The margin box rectangle.
         */
        [[nodiscard]] constexpr rect margin_box() const noexcept { return inflate(border_box(), margin); }
    };

    /**
     * @class tree
     * @brief Owns a set of UI nodes, their parent/child relationships, their styles and their layout results.
     * @details Nodes live in a slot pool, so creating and destroying nodes does not invalidate handles
     * to other nodes and destroyed slots are reused without handles to the old occupant becoming
     * ambiguous. A tree does not designate a root: any node can be passed to `layout`, which makes it
     * cheap to lay out a detached subtree for measurement.
     *
     * Accessors given an invalid handle do not crash. Read accessors return a shared default value and
     * write accessors return a reference to a scratch slot whose contents are discarded, so a stale
     * handle degrades to a no-op rather than corrupting a live node.
     */
    class tree
    {
    public:
        /**
         * @brief Constructs an empty tree.
         */
        tree();

        /**
         * @brief Destroys the tree and every node in it.
         */
        ~tree();

        tree(const tree &) = delete;
        tree &operator=(const tree &) = delete;

        /**
         * @brief Moves a tree. Handles remain valid against the moved-to tree.
         */
        tree(tree &&) noexcept;

        /**
         * @brief Move-assigns a tree, destroying any nodes this tree already owned.
         */
        tree &operator=(tree &&) noexcept;

        /**
         * @fn create
         * @brief Creates a parentless node with a default style.
         * @return A handle to the new node.
         */
        [[nodiscard]] node create();

        /**
         * @fn create_child
         * @brief Creates a node and appends it to the given parent's children.
         * @param parent The node to append the new node to. If it is invalid the new node is left parentless.
         * @return A handle to the new node.
         */
        [[nodiscard]] node create_child(node parent);

        /**
         * @fn destroy
         * @brief Destroys a node and its whole subtree, detaching it from its parent first.
         * @param n The node to destroy. Invalid handles are ignored.
         */
        void destroy(node n);

        /**
         * @fn is_valid
         * @brief Reports whether a handle refers to a live node in this tree.
         * @param n The handle to test.
         * @return True when the node exists and the handle's generation matches.
         */
        [[nodiscard]] bool is_valid(node n) const noexcept;

        /**
         * @fn node_count
         * @brief Returns the number of live nodes in this tree.
         * @return The live node count.
         */
        [[nodiscard]] std::size_t node_count() const noexcept;

        /**
         * @fn add_child
         * @brief Appends a node to a parent's children, detaching it from any previous parent.
         * @param parent The new parent.
         * @param child The node to reparent.
         */
        void add_child(node parent, node child);

        /**
         * @fn insert_child
         * @brief Inserts a node into a parent's children at a given position, detaching it from any previous parent.
         * @param parent The new parent.
         * @param child The node to reparent.
         * @param index The position to insert at. An index past the end appends.
         */
        void insert_child(node parent, node child, std::size_t index);

        /**
         * @fn remove_child
         * @brief Detaches a node from its parent without destroying it.
         * @param parent The current parent.
         * @param child The node to detach. Ignored if it is not a child of `parent`.
         */
        void remove_child(node parent, node child);

        /**
         * @fn parent_of
         * @brief Returns a node's parent.
         * @param n The node to query.
         * @return The parent handle, or `null_node` for a root or invalid node.
         */
        [[nodiscard]] node parent_of(node n) const noexcept;

        /**
         * @fn children_of
         * @brief Returns a node's children in order.
         * @param n The node to query.
         * @return A view of the child handles, valid until the tree's structure changes. Empty for an invalid node.
         */
        [[nodiscard]] std::span<const node> children_of(node n) const noexcept;

        /**
         * @fn child_count
         * @brief Returns how many children a node has.
         * @param n The node to query.
         * @return The child count, or zero for an invalid node.
         */
        [[nodiscard]] std::size_t child_count(node n) const noexcept;

        /**
         * @fn style_of
         * @brief Returns a node's style for reading.
         * @param n The node to query.
         * @return The node's style, or a default style for an invalid node.
         */
        [[nodiscard]] const ui::style &style_of(node n) const noexcept;

        /**
         * @fn mutable_style
         * @brief Returns a node's style for writing and marks the node dirty.
         * @details Every call marks the node dirty whether or not the style is actually changed, so
         * prefer `style_of` when only reading.
         * @param n The node to modify.
         * @return The node's style, or a scratch style whose contents are discarded for an invalid node.
         */
        [[nodiscard]] ui::style &mutable_style(node n) noexcept;

        /**
         * @fn set_style
         * @brief Replaces a node's style and marks the node dirty.
         * @param n The node to modify.
         * @param s The style to assign.
         */
        void set_style(node n, const ui::style &s) noexcept;

        /**
         * @fn set_measure
         * @brief Registers the callback a node uses to report its intrinsic content size.
         * @details A node with a measure callback is treated as a leaf: the layout engine calls the
         * callback instead of laying out children. Pass `nullptr` to clear it.
         * @param n The node to modify.
         * @param fn The callback, or `nullptr` to remove the current one.
         * @param user An opaque pointer passed back to the callback on every call.
         */
        void set_measure(node n, measure_fn fn, void *user = nullptr) noexcept;

        /**
         * @fn measure_of
         * @brief Returns the measure callback registered on a node.
         * @param n The node to query.
         * @return The callback, or `nullptr` if the node has none or is invalid.
         */
        [[nodiscard]] measure_fn measure_of(node n) const noexcept;

        /**
         * @fn measure_user
         * @brief Returns the opaque pointer registered alongside a node's measure callback.
         * @param n The node to query.
         * @return The pointer, or `nullptr` if the node has no callback or is invalid.
         */
        [[nodiscard]] void *measure_user(node n) const noexcept;

        /**
         * @fn layout_of
         * @brief Returns a node's most recent layout result.
         * @param n The node to query.
         * @return The layout result, or a default-constructed result for an invalid node.
         */
        [[nodiscard]] const layout_result &layout_of(node n) const noexcept;

        /**
         * @fn mutable_layout
         * @brief Returns a node's layout result for writing.
         * @details This exists for the layout engine. Applications should read results through
         * `layout_of` and let `layout` produce them.
         * @param n The node to modify.
         * @return The layout result, or a scratch result whose contents are discarded for an invalid node.
         */
        [[nodiscard]] layout_result &mutable_layout(node n) noexcept;

        /**
         * @fn mark_dirty
         * @brief Marks a node and all of its ancestors as needing layout.
         * @param n The node whose layout inputs have changed.
         */
        void mark_dirty(node n) noexcept;

        /**
         * @fn is_dirty
         * @brief Reports whether a node is marked as needing layout.
         * @param n The node to query.
         * @return True when the node is dirty. Invalid nodes are never dirty.
         */
        [[nodiscard]] bool is_dirty(node n) const noexcept;

        /**
         * @fn clear_dirty
         * @brief Clears the dirty flag on a node and its whole subtree.
         * @details `layout` calls this on the subtree it lays out. Incremental, dirty-driven relayout
         * is not implemented yet; the flag is tracked so that it can be without an API change.
         * @param n The root of the subtree to clear.
         */
        void clear_dirty(node n) noexcept;

        /**
         * @fn clear
         * @brief Destroys every node in the tree, leaving it as if freshly constructed.
         */
        void clear() noexcept;

    private:
        struct node_data
        {
            std::uint32_t generation = 0u;
            bool alive = false;
            bool dirty = true;
            node parent = null_node;
            std::vector<node> children;
            ui::style style{};
            layout_result layout{};
            measure_fn measure = nullptr;
            void *measure_user = nullptr;
        };

        [[nodiscard]] const node_data *find(node n) const noexcept;
        [[nodiscard]] node_data *find(node n) noexcept;
        void detach(node n) noexcept;
        void destroy_recursive(node n);

        std::vector<node_data> nodes_;
        std::vector<std::uint32_t> free_slots_;
        std::size_t live_count_ = 0;

        ui::style scratch_style_{};
        layout_result scratch_layout_{};
    };

} // namespace catalyst::ui
