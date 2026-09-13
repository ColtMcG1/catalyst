#include "test_common.hpp"

#include <catalyst/ui/node.hpp>

using namespace catalyst::ui;

namespace
{
    extent stub_measure(const measure_input &, void *) noexcept { return extent{1.0f, 2.0f}; }

    void test_create_and_validity()
    {
        tree t;
        CT_REQUIRE(t.node_count() == 0u);
        CT_REQUIRE(!t.is_valid(null_node));
        CT_REQUIRE(is_null(null_node));

        const node a = t.create();
        CT_REQUIRE(t.is_valid(a));
        CT_REQUIRE(!is_null(a));
        CT_REQUIRE(t.node_count() == 1u);
        CT_REQUIRE(is_null(t.parent_of(a)));
        CT_REQUIRE(t.child_count(a) == 0u);
    }

    void test_hierarchy()
    {
        tree t;
        const node root = t.create();
        const node a = t.create_child(root);
        const node b = t.create_child(root);

        CT_REQUIRE(t.child_count(root) == 2u);
        CT_REQUIRE(t.children_of(root)[0] == a);
        CT_REQUIRE(t.children_of(root)[1] == b);
        CT_REQUIRE(t.parent_of(a) == root);

        const node c = t.create();
        t.insert_child(root, c, 1u);
        CT_REQUIRE(t.children_of(root)[1] == c);
        CT_REQUIRE(t.child_count(root) == 3u);

        // Reparenting detaches from the previous parent rather than duplicating the node.
        t.add_child(a, c);
        CT_REQUIRE(t.child_count(root) == 2u);
        CT_REQUIRE(t.parent_of(c) == a);
        CT_REQUIRE(t.children_of(a)[0] == c);

        t.remove_child(a, c);
        CT_REQUIRE(t.child_count(a) == 0u);
        CT_REQUIRE(is_null(t.parent_of(c)));
        CT_REQUIRE(t.is_valid(c));
    }

    void test_cycles_are_rejected()
    {
        tree t;
        const node root = t.create();
        const node child = t.create_child(root);

        // Making an ancestor a child of its own descendant would orphan the subtree.
        t.add_child(child, root);
        CT_REQUIRE(t.parent_of(root) != child);
        CT_REQUIRE(t.parent_of(child) == root);

        t.add_child(root, root);
        CT_REQUIRE(is_null(t.parent_of(root)));
    }

    void test_destroy_removes_subtree()
    {
        tree t;
        const node root = t.create();
        const node a = t.create_child(root);
        const node grandchild = t.create_child(a);
        const node b = t.create_child(root);

        CT_REQUIRE(t.node_count() == 4u);

        t.destroy(a);

        CT_REQUIRE(!t.is_valid(a));
        CT_REQUIRE(!t.is_valid(grandchild));
        CT_REQUIRE(t.is_valid(root));
        CT_REQUIRE(t.is_valid(b));
        CT_REQUIRE(t.node_count() == 2u);
        CT_REQUIRE(t.child_count(root) == 1u);
        CT_REQUIRE(t.children_of(root)[0] == b);
    }

    void test_stale_handles_are_detected()
    {
        tree t;
        const node a = t.create();
        t.destroy(a);

        const node b = t.create();

        // The slot is reused, so the generation is what keeps the old handle from aliasing the new node.
        CT_REQUIRE(b.index == a.index);
        CT_REQUIRE(b.generation != a.generation);
        CT_REQUIRE(!t.is_valid(a));
        CT_REQUIRE(t.is_valid(b));
    }

    void test_invalid_handles_are_inert()
    {
        tree t;
        const node a = t.create();
        t.destroy(a);

        // Reads fall back to defaults and writes go nowhere, rather than corrupting a live node.
        CT_REQUIRE(t.style_of(a).display == display_mode::flex);
        CT_REQUIRE(!t.layout_of(a).laid_out);
        CT_REQUIRE(t.child_count(a) == 0u);
        CT_REQUIRE(t.children_of(a).empty());
        CT_REQUIRE(t.measure_of(a) == nullptr);
        CT_REQUIRE(!t.is_dirty(a));

        t.mutable_style(a).width = px(10.0f);
        CT_REQUIRE(t.style_of(a).width.is_auto);
    }

    void test_style_and_measure()
    {
        tree t;
        const node a = t.create();

        CT_REQUIRE(t.style_of(a).width.is_auto);

        t.mutable_style(a).width = px(120.0f);
        CT_REQUIRE(!t.style_of(a).width.is_auto);

        style s{};
        s.flex_grow = 2.0f;
        t.set_style(a, s);
        CT_REQUIRE(t.style_of(a).flex_grow == 2.0f);
        CT_REQUIRE(t.style_of(a).width.is_auto);

        int marker = 0;
        t.set_measure(a, stub_measure, &marker);
        CT_REQUIRE(t.measure_of(a) == stub_measure);
        CT_REQUIRE(t.measure_user(a) == &marker);

        t.set_measure(a, nullptr);
        CT_REQUIRE(t.measure_of(a) == nullptr);
    }

    void test_dirty_propagates_to_ancestors()
    {
        tree t;
        const node root = t.create();
        const node a = t.create_child(root);
        const node leaf = t.create_child(a);

        t.clear_dirty(root);
        CT_REQUIRE(!t.is_dirty(root));
        CT_REQUIRE(!t.is_dirty(a));
        CT_REQUIRE(!t.is_dirty(leaf));

        t.mutable_style(leaf).height = px(4.0f);
        CT_REQUIRE(t.is_dirty(leaf));
        CT_REQUIRE(t.is_dirty(a));
        CT_REQUIRE(t.is_dirty(root));

        t.clear_dirty(root);
        CT_REQUIRE(!t.is_dirty(leaf));

        // Structural edits dirty the new parent too.
        const node extra = t.create();
        t.add_child(a, extra);
        CT_REQUIRE(t.is_dirty(root));
    }

    void test_clear()
    {
        tree t;
        const node root = t.create();
        (void)t.create_child(root);
        (void)t.create_child(root);
        CT_REQUIRE(t.node_count() == 3u);

        t.clear();
        CT_REQUIRE(t.node_count() == 0u);
        CT_REQUIRE(!t.is_valid(root));
    }

} // namespace

int main()
{
    test_create_and_validity();
    test_hierarchy();
    test_cycles_are_rejected();
    test_destroy_removes_subtree();
    test_stale_handles_are_detected();
    test_invalid_handles_are_inert();
    test_style_and_measure();
    test_dirty_propagates_to_ancestors();
    test_clear();
    return 0;
}
