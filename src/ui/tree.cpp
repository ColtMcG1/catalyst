/**
 * @file tree.cpp
 * @brief Implements the UI node pool and hierarchy operations declared in node.hpp.
 * License: MIT (see LICENSE).
 */

#include <algorithm>
#include <utility>

#include <catalyst/ui/node.hpp>

namespace catalyst::ui
{

    namespace
    {
        const style &default_style() noexcept
        {
            static const style s{};
            return s;
        }

        const layout_result &default_layout() noexcept
        {
            static const layout_result r{};
            return r;
        }
    } // namespace

    tree::tree() = default;
    tree::~tree() = default;
    tree::tree(tree &&) noexcept = default;
    tree &tree::operator=(tree &&) noexcept = default;

    const tree::node_data *tree::find(node n) const noexcept
    {
        if (is_null(n) || n.index >= nodes_.size())
            return nullptr;

        const node_data &d = nodes_[n.index];
        if (!d.alive || d.generation != n.generation)
            return nullptr;

        return &d;
    }

    tree::node_data *tree::find(node n) noexcept
    {
        return const_cast<node_data *>(std::as_const(*this).find(n));
    }

    node tree::create()
    {
        std::uint32_t index = 0u;

        if (!free_slots_.empty())
        {
            index = free_slots_.back();
            free_slots_.pop_back();
        }
        else
        {
            index = static_cast<std::uint32_t>(nodes_.size());
            nodes_.emplace_back();
        }

        node_data &d = nodes_[index];
        d.alive = true;
        d.dirty = true;
        d.parent = null_node;
        d.children.clear();
        d.style = style{};
        d.layout = layout_result{};
        d.measure = nullptr;
        d.measure_user = nullptr;

        ++live_count_;

        return node{index, d.generation};
    }

    node tree::create_child(node parent)
    {
        const node child = create();
        if (is_valid(parent))
            add_child(parent, child);

        return child;
    }

    void tree::destroy(node n)
    {
        if (!is_valid(n))
            return;

        detach(n);
        destroy_recursive(n);
    }

    void tree::destroy_recursive(node n)
    {
        node_data *d = find(n);
        if (d == nullptr)
            return;

        // Copy the child list: destroying a child mutates the vector we would otherwise iterate.
        const std::vector<node> children = d->children;
        for (const node &child : children)
            destroy_recursive(child);

        d = find(n);
        if (d == nullptr)
            return;

        d->alive = false;
        d->dirty = false;
        d->parent = null_node;
        d->children.clear();
        d->children.shrink_to_fit();
        d->measure = nullptr;
        d->measure_user = nullptr;
        ++d->generation;

        --live_count_;
        free_slots_.push_back(n.index);
    }

    void tree::detach(node n) noexcept
    {
        node_data *d = find(n);
        if (d == nullptr || is_null(d->parent))
            return;

        const node parent = d->parent;
        d->parent = null_node;

        node_data *p = find(parent);
        if (p == nullptr)
            return;

        p->children.erase(std::remove(p->children.begin(), p->children.end(), n), p->children.end());
        mark_dirty(parent);
    }

    bool tree::is_valid(node n) const noexcept
    {
        return find(n) != nullptr;
    }

    std::size_t tree::node_count() const noexcept
    {
        return live_count_;
    }

    void tree::add_child(node parent, node child)
    {
        insert_child(parent, child, static_cast<std::size_t>(-1));
    }

    void tree::insert_child(node parent, node child, std::size_t index)
    {
        if (!is_valid(parent) || !is_valid(child) || parent == child)
            return;

        // Reparenting a node under its own descendant would detach the subtree from the tree.
        for (node ancestor = parent; !is_null(ancestor); ancestor = parent_of(ancestor))
        {
            if (ancestor == child)
                return;
        }

        detach(child);

        node_data *p = find(parent);
        node_data *c = find(child);
        if (p == nullptr || c == nullptr)
            return;

        const std::size_t at = (index > p->children.size()) ? p->children.size() : index;
        p->children.insert(p->children.begin() + static_cast<std::ptrdiff_t>(at), child);
        c->parent = parent;

        mark_dirty(parent);
    }

    void tree::remove_child(node parent, node child)
    {
        const node_data *c = find(child);
        if (c == nullptr || c->parent != parent)
            return;

        detach(child);
    }

    node tree::parent_of(node n) const noexcept
    {
        const node_data *d = find(n);
        return (d != nullptr) ? d->parent : null_node;
    }

    std::span<const node> tree::children_of(node n) const noexcept
    {
        const node_data *d = find(n);
        if (d == nullptr)
            return {};

        return std::span<const node>{d->children};
    }

    std::size_t tree::child_count(node n) const noexcept
    {
        const node_data *d = find(n);
        return (d != nullptr) ? d->children.size() : 0u;
    }

    const style &tree::style_of(node n) const noexcept
    {
        const node_data *d = find(n);
        return (d != nullptr) ? d->style : default_style();
    }

    style &tree::mutable_style(node n) noexcept
    {
        node_data *d = find(n);
        if (d == nullptr)
        {
            scratch_style_ = style{};
            return scratch_style_;
        }

        mark_dirty(n);
        return d->style;
    }

    void tree::set_style(node n, const style &s) noexcept
    {
        node_data *d = find(n);
        if (d == nullptr)
            return;

        d->style = s;
        mark_dirty(n);
    }

    void tree::set_measure(node n, measure_fn fn, void *user) noexcept
    {
        node_data *d = find(n);
        if (d == nullptr)
            return;

        d->measure = fn;
        d->measure_user = user;
        mark_dirty(n);
    }

    measure_fn tree::measure_of(node n) const noexcept
    {
        const node_data *d = find(n);
        return (d != nullptr) ? d->measure : nullptr;
    }

    void *tree::measure_user(node n) const noexcept
    {
        const node_data *d = find(n);
        return (d != nullptr) ? d->measure_user : nullptr;
    }

    const layout_result &tree::layout_of(node n) const noexcept
    {
        const node_data *d = find(n);
        return (d != nullptr) ? d->layout : default_layout();
    }

    layout_result &tree::mutable_layout(node n) noexcept
    {
        node_data *d = find(n);
        if (d == nullptr)
        {
            scratch_layout_ = layout_result{};
            return scratch_layout_;
        }

        return d->layout;
    }

    void tree::mark_dirty(node n) noexcept
    {
        for (node current = n; !is_null(current);)
        {
            node_data *d = find(current);
            if (d == nullptr)
                return;

            d->dirty = true;
            current = d->parent;
        }
    }

    bool tree::is_dirty(node n) const noexcept
    {
        const node_data *d = find(n);
        return (d != nullptr) && d->dirty;
    }

    void tree::clear_dirty(node n) noexcept
    {
        node_data *d = find(n);
        if (d == nullptr)
            return;

        d->dirty = false;

        // The child list can be reallocated only by structural edits, which cannot happen here.
        for (const node &child : d->children)
            clear_dirty(child);
    }

    void tree::clear() noexcept
    {
        nodes_.clear();
        free_slots_.clear();
        live_count_ = 0u;
    }

} // namespace catalyst::ui
