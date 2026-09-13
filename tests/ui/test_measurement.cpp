#include "test_common.hpp"

#include <catalyst/ui/measurement.hpp>

using namespace catalyst::ui;
using catalyst::tests::near;

namespace
{
    resolve_context make_context()
    {
        resolve_context ctx{};
        ctx.dpi_scale = 2.0f;
        ctx.dpi_x = 192.0f;
        ctx.dpi_y = 192.0f;
        ctx.font_px = 20.0f;
        ctx.root_font_px = 16.0f;
        ctx.parent_width_px = 400.0f;
        ctx.parent_height_px = 200.0f;
        ctx.viewport_width_px = 1000.0f;
        ctx.viewport_height_px = 500.0f;
        return ctx;
    }

    void test_absolute_units()
    {
        const resolve_context ctx = make_context();

        CT_REQUIRE(near(resolve_or(px(12.0f), axis::x, ctx), 12.0f));
        CT_REQUIRE(near(resolve_or(dp(12.0f), axis::x, ctx), 24.0f));
        CT_REQUIRE(near(resolve_or(in(1.0f), axis::x, ctx), 192.0f));
        CT_REQUIRE(near(resolve_or(cm(2.54f), axis::x, ctx), 192.0f));
        CT_REQUIRE(near(resolve_or(mm(25.4f), axis::y, ctx), 192.0f));
    }

    void test_relative_units()
    {
        const resolve_context ctx = make_context();

        CT_REQUIRE(near(resolve_or(em(2.0f), axis::x, ctx), 40.0f));
        CT_REQUIRE(near(resolve_or(rem(2.0f), axis::x, ctx), 32.0f));
        CT_REQUIRE(near(resolve_or(vw(10.0f), axis::x, ctx), 100.0f));
        CT_REQUIRE(near(resolve_or(vh(10.0f), axis::x, ctx), 50.0f));

        // Percentages follow the axis they are resolved on, not the unit itself.
        CT_REQUIRE(near(resolve_or(percent(50.0f), axis::x, ctx), 200.0f));
        CT_REQUIRE(near(resolve_or(percent(50.0f), axis::y, ctx), 100.0f));
    }

    void test_dpi_scale_fallback()
    {
        // A context that only sets dpi_scale still resolves physical units.
        resolve_context ctx{};
        ctx.dpi_scale = 1.5f;

        CT_REQUIRE(near(ctx.effective_dpi(axis::x), 144.0f));
        CT_REQUIRE(near(resolve_or(in(1.0f), axis::x, ctx), 144.0f));
        CT_REQUIRE(near(resolve_or(dp(10.0f), axis::x, ctx), 15.0f));
    }

    void test_calc_arithmetic()
    {
        const resolve_context ctx = make_context();

        const length sum = px(10.0f) + percent(25.0f);
        CT_REQUIRE(near(resolve_or(sum, axis::x, ctx), 110.0f));

        const length diff = percent(50.0f) - px(20.0f);
        CT_REQUIRE(near(resolve_or(diff, axis::x, ctx), 180.0f));

        const length scaled = (px(10.0f) + em(1.0f)) * 2.0f;
        CT_REQUIRE(near(resolve_or(scaled, axis::x, ctx), 60.0f));

        length accumulated = px(5.0f);
        accumulated += px(7.0f);
        CT_REQUIRE(near(resolve_or(accumulated, axis::x, ctx), 12.0f));
    }

    void test_auto_propagates()
    {
        const resolve_context ctx = make_context();
        const length a = auto_();

        CT_REQUIRE(a.is_auto);
        CT_REQUIRE(near(resolve_or(a, axis::x, ctx, 42.0f), 42.0f));

        // Any operand being auto makes the whole expression auto.
        CT_REQUIRE((a + px(10.0f)).is_auto);
        CT_REQUIRE((px(10.0f) + a).is_auto);
        CT_REQUIRE((a * 2.0f).is_auto);
    }

    void test_literals()
    {
        using namespace catalyst::ui::literals;

        const resolve_context ctx = make_context();

        CT_REQUIRE(near(resolve_or(16.0_px, axis::x, ctx), 16.0f));
        CT_REQUIRE(near(resolve_or(1.0_em, axis::x, ctx), 20.0f));
        CT_REQUIRE(near(resolve_or(2.0_rem, axis::x, ctx), 32.0f));
    }

} // namespace

int main()
{
    test_absolute_units();
    test_relative_units();
    test_dpi_scale_fallback();
    test_calc_arithmetic();
    test_auto_propagates();
    test_literals();
    return 0;
}
