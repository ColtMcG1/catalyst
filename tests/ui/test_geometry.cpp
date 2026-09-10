#include "test_common.hpp"

#include <catalyst/ui/color.hpp>
#include <catalyst/ui/geometry.hpp>

using namespace catalyst::ui;
using catalyst::tests::near;
using catalyst::tests::near_rect;

namespace
{
    void test_edges_construction()
    {
        constexpr edges_px all = edges_px::all(4.0f);
        CT_REQUIRE(near(all.left, 4.0f) && near(all.top, 4.0f) && near(all.right, 4.0f) && near(all.bottom, 4.0f));
        CT_REQUIRE(near(all.horizontal(), 8.0f));
        CT_REQUIRE(near(all.vertical(), 8.0f));

        constexpr edges_px sym = edges_px::symmetric(10.0f, 2.0f);
        CT_REQUIRE(near(sym.left, 10.0f) && near(sym.right, 10.0f));
        CT_REQUIRE(near(sym.top, 2.0f) && near(sym.bottom, 2.0f));
        CT_REQUIRE(near(sym.along(axis::x), 20.0f));
        CT_REQUIRE(near(sym.along(axis::y), 4.0f));
        CT_REQUIRE(near(sym.start(axis::x), 10.0f) && near(sym.end(axis::y), 2.0f));

        constexpr edges_px sum = edges_px::all(1.0f) + edges_px::all(2.0f);
        CT_REQUIRE(sum == edges_px::all(3.0f));
    }

    void test_deflate_and_inflate()
    {
        const rect r = rect::from_xywh(10.0f, 20.0f, 100.0f, 50.0f);

        const rect inner = deflate(r, edges_px{2.0f, 4.0f, 6.0f, 8.0f});
        CT_REQUIRE(near_rect(inner, 12.0f, 24.0f, 92.0f, 38.0f));

        const rect outer = inflate(inner, edges_px{2.0f, 4.0f, 6.0f, 8.0f});
        CT_REQUIRE(near_rect(outer, 10.0f, 20.0f, 100.0f, 50.0f));
    }

    void test_deflate_collapses_instead_of_inverting()
    {
        const rect r = rect::from_xywh(0.0f, 0.0f, 10.0f, 10.0f);
        const rect collapsed = deflate(r, edges_px::all(50.0f));

        CT_REQUIRE(collapsed.max.x() >= collapsed.min.x());
        CT_REQUIRE(collapsed.max.y() >= collapsed.min.y());
        CT_REQUIRE(near(collapsed.max.x() - collapsed.min.x(), 0.0f));
        CT_REQUIRE(near(collapsed.max.y() - collapsed.min.y(), 0.0f));
    }

    void test_resolve_edges_uses_width_on_every_side()
    {
        resolve_context ctx{};
        ctx.parent_width_px = 200.0f;
        ctx.parent_height_px = 1000.0f;

        const edges_length padding = edges_length::all(percent(10.0f));
        const edges_px resolved = resolve(padding, ctx);

        // A symmetric percentage padding stays symmetric: all four sides use the width.
        CT_REQUIRE(near(resolved.left, 20.0f));
        CT_REQUIRE(near(resolved.top, 20.0f));
        CT_REQUIRE(near(resolved.right, 20.0f));
        CT_REQUIRE(near(resolved.bottom, 20.0f));
    }

    void test_resolve_edges_treats_auto_as_zero()
    {
        resolve_context ctx{};
        ctx.parent_width_px = 100.0f;

        edges_length margin{};
        margin.left = auto_();
        margin.right = px(8.0f);

        const edges_px resolved = resolve(margin, ctx);
        CT_REQUIRE(near(resolved.left, 0.0f));
        CT_REQUIRE(near(resolved.right, 8.0f));
    }

    void test_color_packing()
    {
        constexpr color c = color::from_rgba8(255, 128, 0, 255);
        CT_REQUIRE(near(c.r, 1.0f));
        CT_REQUIRE(near(c.g, 128.0f / 255.0f));
        CT_REQUIRE(near(c.b, 0.0f));
        CT_REQUIRE(near(c.a, 1.0f));

        // Round trips through the eight-bit form used by vertex colors.
        CT_REQUIRE(c.to_rgba8() == 0xFF0080FFu);
        CT_REQUIRE(color::from_argb32(0xFF0080FFu) == color::from_rgba8(0x00, 0x80, 0xFF, 0xFF));

        // Out-of-range channels saturate rather than wrapping.
        CT_REQUIRE(color::rgba(2.0f, -1.0f, 0.0f, 1.0f).to_rgba8() == 0xFF0000FFu);
    }

    void test_color_operations()
    {
        constexpr color half = colors::white.with_alpha(0.5f);
        CT_REQUIRE(near(half.a, 0.5f));

        constexpr color pre = half.premultiplied();
        CT_REQUIRE(near(pre.r, 0.5f) && near(pre.a, 0.5f));

        constexpr color mid = lerp(colors::black, colors::white, 0.25f);
        CT_REQUIRE(near(mid.r, 0.25f) && near(mid.g, 0.25f) && near(mid.b, 0.25f));

        CT_REQUIRE(colors::transparent.is_transparent());
        CT_REQUIRE(!colors::black.is_transparent());
    }

} // namespace

int main()
{
    test_edges_construction();
    test_deflate_and_inflate();
    test_deflate_collapses_instead_of_inverting();
    test_resolve_edges_uses_width_on_every_side();
    test_resolve_edges_treats_auto_as_zero();
    test_color_packing();
    test_color_operations();
    return 0;
}
