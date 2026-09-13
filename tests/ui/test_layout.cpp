#include "test_common.hpp"

#include <catalyst/ui/layout.hpp>

using namespace catalyst::ui;
using catalyst::tests::near;
using catalyst::tests::near_rect;

namespace
{
    void run(tree &t, node root, float w, float h)
    {
        layout(t, root, layout_params::for_viewport(extent{w, h}));
    }

    rect box(const tree &t, node n) { return t.layout_of(n).border_box(); }

    extent measure_fixed(const measure_input &, void *user) noexcept
    {
        return *static_cast<const extent *>(user);
    }

    extent measure_wrapping(const measure_input &input, void *) noexcept
    {
        // Stands in for text: a fixed amount of content reflowed into the offered width.
        const float total = 400.0f;
        const float line_height = 20.0f;
        const float width = (input.available_width_px > 0.0f) ? input.available_width_px : total;
        const float lines = std::ceil(total / width);
        return extent{(width < total) ? width : total, lines * line_height};
    }

    void test_row_places_children_in_order()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = px(400.0f);
        t.mutable_style(root).height = px(200.0f);

        const node a = t.create_child(root);
        t.mutable_style(a).width = px(100.0f);
        t.mutable_style(a).height = px(50.0f);

        const node b = t.create_child(root);
        t.mutable_style(b).width = px(150.0f);
        t.mutable_style(b).height = px(60.0f);

        run(t, root, 800.0f, 600.0f);

        CT_REQUIRE(near_rect(box(t, root), 0.0f, 0.0f, 400.0f, 200.0f));
        CT_REQUIRE(near_rect(box(t, a), 0.0f, 0.0f, 100.0f, 50.0f));
        CT_REQUIRE(near_rect(box(t, b), 100.0f, 0.0f, 150.0f, 60.0f));
        CT_REQUIRE(t.layout_of(a).laid_out);
        CT_REQUIRE(!t.layout_of(a).hidden);
    }

    void test_column_direction()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = px(400.0f);
        t.mutable_style(root).height = px(300.0f);
        t.mutable_style(root).direction = flex_direction::column;

        const node a = t.create_child(root);
        t.mutable_style(a).height = px(100.0f);

        const node b = t.create_child(root);
        t.mutable_style(b).height = px(50.0f);

        run(t, root, 400.0f, 300.0f);

        // Stacked on the main axis, stretched across the cross axis.
        CT_REQUIRE(near_rect(box(t, a), 0.0f, 0.0f, 400.0f, 100.0f));
        CT_REQUIRE(near_rect(box(t, b), 0.0f, 100.0f, 400.0f, 50.0f));
    }

    void test_reverse_direction()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = px(400.0f);
        t.mutable_style(root).height = px(100.0f);
        t.mutable_style(root).direction = flex_direction::row_reverse;

        const node a = t.create_child(root);
        t.mutable_style(a).width = px(100.0f);
        t.mutable_style(a).height = px(10.0f);

        const node b = t.create_child(root);
        t.mutable_style(b).width = px(100.0f);
        t.mutable_style(b).height = px(10.0f);

        run(t, root, 400.0f, 100.0f);

        // The first child sits at the main-start, which for row_reverse is on the right.
        CT_REQUIRE(near(box(t, b).min.x(), 0.0f));
        CT_REQUIRE(near(box(t, a).min.x(), 100.0f));
    }

    void test_padding_and_border_offset_content()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = px(400.0f);
        t.mutable_style(root).height = px(200.0f);
        t.mutable_style(root).padding = edges_length::all(px(10.0f));
        t.mutable_style(root).border = edges_length::all(px(5.0f));

        const node a = t.create_child(root);
        t.mutable_style(a).width = px(50.0f);
        t.mutable_style(a).height = px(20.0f);

        run(t, root, 400.0f, 200.0f);

        CT_REQUIRE(near_rect(t.layout_of(root).padding_box(), 5.0f, 5.0f, 390.0f, 190.0f));
        CT_REQUIRE(near_rect(t.layout_of(root).content_box(), 15.0f, 15.0f, 370.0f, 170.0f));
        CT_REQUIRE(near_rect(box(t, a), 15.0f, 15.0f, 50.0f, 20.0f));
    }

    void test_box_sizing()
    {
        tree t;
        const node border_boxed = t.create();
        t.mutable_style(border_boxed).width = px(100.0f);
        t.mutable_style(border_boxed).height = px(100.0f);
        t.mutable_style(border_boxed).padding = edges_length::all(px(10.0f));
        t.mutable_style(border_boxed).border = edges_length::all(px(5.0f));

        run(t, border_boxed, 400.0f, 400.0f);
        CT_REQUIRE(near_rect(box(t, border_boxed), 0.0f, 0.0f, 100.0f, 100.0f));
        CT_REQUIRE(near_rect(t.layout_of(border_boxed).content_box(), 15.0f, 15.0f, 70.0f, 70.0f));

        const node content_boxed = t.create();
        t.mutable_style(content_boxed).sizing = box_sizing::content_box;
        t.mutable_style(content_boxed).width = px(100.0f);
        t.mutable_style(content_boxed).height = px(100.0f);
        t.mutable_style(content_boxed).padding = edges_length::all(px(10.0f));
        t.mutable_style(content_boxed).border = edges_length::all(px(5.0f));

        run(t, content_boxed, 400.0f, 400.0f);
        CT_REQUIRE(near_rect(box(t, content_boxed), 0.0f, 0.0f, 130.0f, 130.0f));
        CT_REQUIRE(near_rect(t.layout_of(content_boxed).content_box(), 15.0f, 15.0f, 100.0f, 100.0f));
    }

    void test_margins_separate_siblings()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = px(400.0f);
        t.mutable_style(root).height = px(100.0f);

        const node a = t.create_child(root);
        t.mutable_style(a).width = px(100.0f);
        t.mutable_style(a).height = px(10.0f);
        t.mutable_style(a).margin.left = px(10.0f);
        t.mutable_style(a).margin.right = px(20.0f);

        const node b = t.create_child(root);
        t.mutable_style(b).width = px(100.0f);
        t.mutable_style(b).height = px(10.0f);

        run(t, root, 400.0f, 100.0f);

        CT_REQUIRE(near(box(t, a).min.x(), 10.0f));
        CT_REQUIRE(near(box(t, b).min.x(), 130.0f));
        CT_REQUIRE(near_rect(t.layout_of(a).margin_box(), 0.0f, 0.0f, 130.0f, 10.0f));
    }

    void test_percentage_sizes()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = px(400.0f);
        t.mutable_style(root).height = px(200.0f);

        const node a = t.create_child(root);
        t.mutable_style(a).width = percent(25.0f);
        t.mutable_style(a).height = percent(50.0f);

        run(t, root, 400.0f, 200.0f);

        CT_REQUIRE(near_rect(box(t, a), 0.0f, 0.0f, 100.0f, 100.0f));
    }

    void test_flex_grow_shares_free_space()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = px(400.0f);
        t.mutable_style(root).height = px(50.0f);

        const node a = t.create_child(root);
        t.mutable_style(a).flex_grow = 1.0f;

        const node b = t.create_child(root);
        t.mutable_style(b).flex_grow = 3.0f;

        run(t, root, 400.0f, 50.0f);

        CT_REQUIRE(near(box(t, a).min.x(), 0.0f));
        CT_REQUIRE(near(box(t, a).max.x() - box(t, a).min.x(), 100.0f));
        CT_REQUIRE(near(box(t, b).min.x(), 100.0f));
        CT_REQUIRE(near(box(t, b).max.x() - box(t, b).min.x(), 300.0f));
    }

    void test_flex_shrink_absorbs_overflow()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = px(100.0f);
        t.mutable_style(root).height = px(50.0f);

        const node a = t.create_child(root);
        t.mutable_style(a).width = px(100.0f);

        const node b = t.create_child(root);
        t.mutable_style(b).width = px(100.0f);

        run(t, root, 100.0f, 50.0f);

        CT_REQUIRE(near(box(t, a).max.x() - box(t, a).min.x(), 50.0f));
        CT_REQUIRE(near(box(t, b).max.x() - box(t, b).min.x(), 50.0f));
        CT_REQUIRE(near(box(t, b).min.x(), 50.0f));
    }

    void test_flex_shrink_respects_zero_factor()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = px(100.0f);
        t.mutable_style(root).height = px(50.0f);

        const node fixed = t.create_child(root);
        t.mutable_style(fixed).width = px(80.0f);
        t.mutable_style(fixed).flex_shrink = 0.0f;

        const node flexible = t.create_child(root);
        t.mutable_style(flexible).width = px(80.0f);

        run(t, root, 100.0f, 50.0f);

        CT_REQUIRE(near(box(t, fixed).max.x() - box(t, fixed).min.x(), 80.0f));
        CT_REQUIRE(near(box(t, flexible).max.x() - box(t, flexible).min.x(), 20.0f));
    }

    void test_max_width_redistributes_to_siblings()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = px(300.0f);
        t.mutable_style(root).height = px(50.0f);

        const node capped = t.create_child(root);
        t.mutable_style(capped).flex_grow = 1.0f;
        t.mutable_style(capped).max_width = px(50.0f);

        const node rest = t.create_child(root);
        t.mutable_style(rest).flex_grow = 1.0f;

        run(t, root, 300.0f, 50.0f);

        // The capped item freezes at its bound and what it could not take goes to its sibling.
        CT_REQUIRE(near(box(t, capped).max.x() - box(t, capped).min.x(), 50.0f));
        CT_REQUIRE(near(box(t, rest).max.x() - box(t, rest).min.x(), 250.0f));
    }

    void test_min_width_is_honored_when_shrinking()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = px(100.0f);
        t.mutable_style(root).height = px(50.0f);

        const node floored = t.create_child(root);
        t.mutable_style(floored).width = px(100.0f);
        t.mutable_style(floored).min_width = px(80.0f);

        const node other = t.create_child(root);
        t.mutable_style(other).width = px(100.0f);

        run(t, root, 100.0f, 50.0f);

        CT_REQUIRE(near(box(t, floored).max.x() - box(t, floored).min.x(), 80.0f));
        CT_REQUIRE(near(box(t, other).max.x() - box(t, other).min.x(), 20.0f));
    }

    void test_gap_between_children()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = px(400.0f);
        t.mutable_style(root).height = px(50.0f);
        t.mutable_style(root).set_gap(px(20.0f));

        const node a = t.create_child(root);
        t.mutable_style(a).width = px(100.0f);

        const node b = t.create_child(root);
        t.mutable_style(b).width = px(100.0f);

        run(t, root, 400.0f, 50.0f);

        CT_REQUIRE(near(box(t, a).min.x(), 0.0f));
        CT_REQUIRE(near(box(t, b).min.x(), 120.0f));
    }

    void test_justify_content()
    {
        const auto positions = [](justify mode) {
            tree t;
            const node root = t.create();
            t.mutable_style(root).width = px(400.0f);
            t.mutable_style(root).height = px(50.0f);
            t.mutable_style(root).justify_content = mode;

            const node a = t.create_child(root);
            t.mutable_style(a).width = px(100.0f);

            const node b = t.create_child(root);
            t.mutable_style(b).width = px(100.0f);

            run(t, root, 400.0f, 50.0f);
            return extent{box(t, a).min.x(), box(t, b).min.x()};
        };

        const extent start = positions(justify::start);
        CT_REQUIRE(near(start.x(), 0.0f) && near(start.y(), 100.0f));

        const extent end = positions(justify::end);
        CT_REQUIRE(near(end.x(), 200.0f) && near(end.y(), 300.0f));

        const extent center = positions(justify::center);
        CT_REQUIRE(near(center.x(), 100.0f) && near(center.y(), 200.0f));

        const extent between = positions(justify::space_between);
        CT_REQUIRE(near(between.x(), 0.0f) && near(between.y(), 300.0f));

        // 200px spare over two items: 50 before each and 50 after each.
        const extent around = positions(justify::space_around);
        CT_REQUIRE(near(around.x(), 50.0f) && near(around.y(), 250.0f));

        // 200px spare over three gaps of 66.67.
        const extent evenly = positions(justify::space_evenly);
        CT_REQUIRE(near(evenly.x(), 200.0f / 3.0f, 0.01f));
        CT_REQUIRE(near(evenly.y(), 100.0f + 400.0f / 3.0f, 0.01f));
    }

    void test_align_items()
    {
        const auto cross_position = [](align mode) {
            tree t;
            const node root = t.create();
            t.mutable_style(root).width = px(400.0f);
            t.mutable_style(root).height = px(200.0f);
            t.mutable_style(root).align_items = mode;

            const node a = t.create_child(root);
            t.mutable_style(a).width = px(50.0f);
            t.mutable_style(a).height = px(50.0f);

            run(t, root, 400.0f, 200.0f);
            return box(t, a);
        };

        CT_REQUIRE(near(cross_position(align::start).min.y(), 0.0f));
        CT_REQUIRE(near(cross_position(align::end).min.y(), 150.0f));
        CT_REQUIRE(near(cross_position(align::center).min.y(), 75.0f));

        // A definite cross size is not stretched.
        CT_REQUIRE(near_rect(cross_position(align::stretch), 0.0f, 0.0f, 50.0f, 50.0f));
    }

    void test_stretch_fills_cross_axis()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = px(400.0f);
        t.mutable_style(root).height = px(200.0f);

        const node a = t.create_child(root);
        t.mutable_style(a).width = px(50.0f);

        const node b = t.create_child(root);
        t.mutable_style(b).width = px(50.0f);
        t.mutable_style(b).align_self = align::center;
        t.mutable_style(b).height = px(20.0f);

        run(t, root, 400.0f, 200.0f);

        CT_REQUIRE(near_rect(box(t, a), 0.0f, 0.0f, 50.0f, 200.0f));
        CT_REQUIRE(near_rect(box(t, b), 50.0f, 90.0f, 50.0f, 20.0f));
    }

    void test_auto_size_hugs_content()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).align_items = align::start;
        t.mutable_style(root).padding = edges_length::all(px(5.0f));

        const node a = t.create_child(root);
        t.mutable_style(a).width = px(60.0f);
        t.mutable_style(a).height = px(30.0f);

        const node b = t.create_child(root);
        t.mutable_style(b).width = px(40.0f);
        t.mutable_style(b).height = px(50.0f);

        run(t, root, 800.0f, 600.0f);

        // Main axis sums the children, cross axis takes the tallest, then padding is added.
        CT_REQUIRE(near_rect(box(t, root), 0.0f, 0.0f, 110.0f, 60.0f));
        CT_REQUIRE(near_rect(box(t, a), 5.0f, 5.0f, 60.0f, 30.0f));
        CT_REQUIRE(near_rect(box(t, b), 65.0f, 5.0f, 40.0f, 50.0f));
    }

    void test_min_height_expands_auto_container()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = px(200.0f);
        t.mutable_style(root).min_height = px(300.0f);

        const node a = t.create_child(root);
        t.mutable_style(a).width = px(50.0f);

        run(t, root, 400.0f, 400.0f);

        // The min bound bites after the children were placed, so they are placed again against it.
        CT_REQUIRE(near(box(t, root).max.y() - box(t, root).min.y(), 300.0f));
        CT_REQUIRE(near_rect(box(t, a), 0.0f, 0.0f, 50.0f, 300.0f));
    }

    void test_display_none_is_skipped()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = px(400.0f);
        t.mutable_style(root).height = px(50.0f);

        const node hidden = t.create_child(root);
        t.mutable_style(hidden).display = display_mode::none;
        t.mutable_style(hidden).width = px(100.0f);

        const node inner = t.create_child(hidden);
        t.mutable_style(inner).width = px(10.0f);

        const node visible = t.create_child(root);
        t.mutable_style(visible).width = px(100.0f);

        run(t, root, 400.0f, 50.0f);

        CT_REQUIRE(t.layout_of(hidden).hidden);
        CT_REQUIRE(!t.layout_of(hidden).laid_out);
        CT_REQUIRE(t.layout_of(inner).hidden);
        CT_REQUIRE(near(box(t, visible).min.x(), 0.0f));
    }

    void test_relative_offset_does_not_move_siblings()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = px(400.0f);
        t.mutable_style(root).height = px(100.0f);

        const node a = t.create_child(root);
        t.mutable_style(a).width = px(100.0f);
        t.mutable_style(a).height = px(10.0f);
        t.mutable_style(a).inset.left = px(15.0f);
        t.mutable_style(a).inset.top = px(5.0f);

        const node b = t.create_child(root);
        t.mutable_style(b).width = px(100.0f);
        t.mutable_style(b).height = px(10.0f);

        run(t, root, 400.0f, 100.0f);

        CT_REQUIRE(near_rect(box(t, a), 15.0f, 5.0f, 100.0f, 10.0f));
        CT_REQUIRE(near(box(t, b).min.x(), 100.0f));
    }

    void test_absolute_positioning()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = px(400.0f);
        t.mutable_style(root).height = px(200.0f);
        t.mutable_style(root).border = edges_length::all(px(5.0f));

        const node flow = t.create_child(root);
        t.mutable_style(flow).width = px(30.0f);
        t.mutable_style(flow).height = px(30.0f);

        const node pinned = t.create_child(root);
        t.mutable_style(pinned).position = position_mode::absolute;
        t.mutable_style(pinned).inset.left = px(10.0f);
        t.mutable_style(pinned).inset.top = px(20.0f);
        t.mutable_style(pinned).width = px(50.0f);
        t.mutable_style(pinned).height = px(30.0f);

        const node from_end = t.create_child(root);
        t.mutable_style(from_end).position = position_mode::absolute;
        t.mutable_style(from_end).inset.right = px(10.0f);
        t.mutable_style(from_end).inset.bottom = px(10.0f);
        t.mutable_style(from_end).width = px(50.0f);
        t.mutable_style(from_end).height = px(30.0f);

        const node stretched = t.create_child(root);
        t.mutable_style(stretched).position = position_mode::absolute;
        t.mutable_style(stretched).inset.left = px(10.0f);
        t.mutable_style(stretched).inset.right = px(30.0f);
        t.mutable_style(stretched).height = px(10.0f);

        run(t, root, 400.0f, 200.0f);

        // The containing block is the root's padding box: 390x190 at (5, 5).
        CT_REQUIRE(near_rect(box(t, pinned), 15.0f, 25.0f, 50.0f, 30.0f));
        CT_REQUIRE(near_rect(box(t, from_end), 5.0f + 390.0f - 10.0f - 50.0f, 5.0f + 190.0f - 10.0f - 30.0f, 50.0f, 30.0f));
        CT_REQUIRE(near_rect(box(t, stretched), 15.0f, 5.0f, 350.0f, 10.0f));

        // An out-of-flow child does not push its in-flow siblings around.
        CT_REQUIRE(near_rect(box(t, flow), 5.0f, 5.0f, 30.0f, 30.0f));
    }

    void test_measure_callback_sizes_leaf()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).align_items = align::start;

        extent content{70.0f, 30.0f};
        const node leaf = t.create_child(root);
        t.set_measure(leaf, measure_fixed, &content);
        t.mutable_style(leaf).padding = edges_length::all(px(4.0f));

        run(t, root, 800.0f, 600.0f);

        // The callback reports a content size; padding is added by the engine.
        CT_REQUIRE(near_rect(box(t, leaf), 0.0f, 0.0f, 78.0f, 38.0f));
        CT_REQUIRE(near_rect(box(t, root), 0.0f, 0.0f, 78.0f, 38.0f));
    }

    void test_measure_callback_sees_available_width()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = px(200.0f);
        t.mutable_style(root).align_items = align::start;
        t.mutable_style(root).direction = flex_direction::column;

        const node leaf = t.create_child(root);
        t.set_measure(leaf, measure_wrapping, nullptr);
        t.mutable_style(leaf).width = percent(100.0f);

        run(t, root, 200.0f, 600.0f);

        // 400 units of content reflowed into 200 gives two 20px lines.
        CT_REQUIRE(near(box(t, leaf).max.x() - box(t, leaf).min.x(), 200.0f));
        CT_REQUIRE(near(box(t, leaf).max.y() - box(t, leaf).min.y(), 40.0f));
    }

    void test_font_size_inherits_for_em()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = px(400.0f);
        t.mutable_style(root).height = px(200.0f);
        t.mutable_style(root).font_size = px(20.0f);

        const node a = t.create_child(root);
        t.mutable_style(a).width = em(2.0f);
        t.mutable_style(a).height = px(10.0f);

        const node b = t.create_child(root);
        t.mutable_style(b).font_size = em(2.0f);
        t.mutable_style(b).width = em(1.0f);
        t.mutable_style(b).height = px(10.0f);

        run(t, root, 400.0f, 200.0f);

        CT_REQUIRE(near(t.layout_of(root).font_px, 20.0f));
        CT_REQUIRE(near(t.layout_of(a).font_px, 20.0f));
        CT_REQUIRE(near(box(t, a).max.x() - box(t, a).min.x(), 40.0f));

        // The node's own font_size resolves against the parent's, then its em uses the new size.
        CT_REQUIRE(near(t.layout_of(b).font_px, 40.0f));
        CT_REQUIRE(near(box(t, b).max.x() - box(t, b).min.x(), 40.0f));
    }

    void test_dpi_scale_applies_to_dp()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = dp(100.0f);
        t.mutable_style(root).height = dp(50.0f);

        layout_params params = layout_params::for_viewport(extent{800.0f, 600.0f}, 2.0f);
        layout(t, root, params);

        CT_REQUIRE(near_rect(box(t, root), 0.0f, 0.0f, 200.0f, 100.0f));
    }

    void test_origin_and_root_margin()
    {
        tree t;
        const node root = t.create();
        t.mutable_style(root).width = px(100.0f);
        t.mutable_style(root).height = px(50.0f);
        t.mutable_style(root).margin = edges_length::all(px(7.0f));

        layout_params params = layout_params::for_viewport(extent{800.0f, 600.0f});
        params.origin = point{30.0f, 40.0f};
        layout(t, root, params);

        CT_REQUIRE(near_rect(box(t, root), 37.0f, 47.0f, 100.0f, 50.0f));
    }

    void test_layout_clears_dirty_flags()
    {
        tree t;
        const node root = t.create();
        const node a = t.create_child(root);
        t.mutable_style(a).width = px(10.0f);

        CT_REQUIRE(t.is_dirty(root));

        run(t, root, 400.0f, 400.0f);

        CT_REQUIRE(!t.is_dirty(root));
        CT_REQUIRE(!t.is_dirty(a));

        t.mutable_style(a).width = px(20.0f);
        CT_REQUIRE(t.is_dirty(root));
    }

    void test_invalid_root_is_ignored()
    {
        tree t;
        const node a = t.create();
        t.destroy(a);

        run(t, a, 100.0f, 100.0f);
        CT_REQUIRE(!t.layout_of(a).laid_out);
    }

} // namespace

int main()
{
    test_row_places_children_in_order();
    test_column_direction();
    test_reverse_direction();
    test_padding_and_border_offset_content();
    test_box_sizing();
    test_margins_separate_siblings();
    test_percentage_sizes();
    test_flex_grow_shares_free_space();
    test_flex_shrink_absorbs_overflow();
    test_flex_shrink_respects_zero_factor();
    test_max_width_redistributes_to_siblings();
    test_min_width_is_honored_when_shrinking();
    test_gap_between_children();
    test_justify_content();
    test_align_items();
    test_stretch_fills_cross_axis();
    test_auto_size_hugs_content();
    test_min_height_expands_auto_container();
    test_display_none_is_skipped();
    test_relative_offset_does_not_move_siblings();
    test_absolute_positioning();
    test_measure_callback_sizes_leaf();
    test_measure_callback_sees_available_width();
    test_font_size_inherits_for_em();
    test_dpi_scale_applies_to_dp();
    test_origin_and_root_margin();
    test_layout_clears_dirty_flags();
    test_invalid_root_is_ignored();
    return 0;
}
