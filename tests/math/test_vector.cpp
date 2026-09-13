#include <catalyst/math/elementwise.hpp>
#include <catalyst/math/geometry.hpp>
#include <catalyst/math/vector.hpp>

#include "test_common.hpp"

namespace
{

using catalyst::tests::nearly_equal;

void test_vec2f_basics()
{
    using math::vec2f;

    const vec2f a{1.0f, 2.0f};
    const vec2f b{3.0f, 4.0f};

    const auto c = a + b;
    CT_REQUIRE(nearly_equal(c.x(), 4.0f));
    CT_REQUIRE(nearly_equal(c.y(), 6.0f));

    const auto d = b - a;
    CT_REQUIRE(nearly_equal(d.x(), 2.0f));
    CT_REQUIRE(nearly_equal(d.y(), 2.0f));

    CT_REQUIRE(nearly_equal(math::dot(a, b), 11.0f));

    const auto n = math::normalized(vec2f{3.0f, 4.0f});
    CT_REQUIRE(nearly_equal(math::magnitude(n), 1.0f));
    CT_REQUIRE(nearly_equal(math::magnitude(vec2f{3.0f, 4.0f}), 5.0f));
    CT_REQUIRE(nearly_equal(math::magnitude_squared(vec2f{3.0f, 4.0f}), 25.0f));
}

void test_vecN_helpers()
{
    using math::vector;

    const vector<float, 4> a{1.0f, 2.0f, 3.0f, 4.0f};
    const vector<float, 4> b{0.0f, 10.0f, 2.0f, 5.0f};

    const auto lo = math::min(a, b);
    CT_REQUIRE(nearly_equal(lo[0], 0.0f));
    CT_REQUIRE(nearly_equal(lo[1], 2.0f));
    CT_REQUIRE(nearly_equal(lo[2], 2.0f));
    CT_REQUIRE(nearly_equal(lo[3], 4.0f));

    const auto hi = math::max(a, b);
    CT_REQUIRE(nearly_equal(hi[0], 1.0f));
    CT_REQUIRE(nearly_equal(hi[1], 10.0f));
    CT_REQUIRE(nearly_equal(hi[2], 3.0f));
    CT_REQUIRE(nearly_equal(hi[3], 5.0f));

    const auto clamped = math::clamp(a, 1.5f, 3.5f);
    CT_REQUIRE(nearly_equal(clamped[0], 1.5f));
    CT_REQUIRE(nearly_equal(clamped[1], 2.0f));
    CT_REQUIRE(nearly_equal(clamped[2], 3.0f));
    CT_REQUIRE(nearly_equal(clamped[3], 3.5f));

    // Per-component bounds take the same route as the scalar ones.
    const auto clamped_v = math::clamp(a, vector<float, 4>::filled(1.5f), vector<float, 4>::filled(3.5f));
    for (std::size_t i = 0; i < 4; ++i)
        CT_REQUIRE(nearly_equal(clamped_v[i], clamped[i]));

    // Wider vectors: N is not limited to the named 2/3/4 aliases.
    const vector<float, 8> w0{1, 2, 3, 4, 5, 6, 7, 8};
    const vector<float, 8> w1{8, 7, 6, 5, 4, 3, 2, 1};
    const auto w2 = w0 + w1;
    CT_REQUIRE(nearly_equal(w2[0], 9.0f));
    CT_REQUIRE(nearly_equal(w2[7], 9.0f));
    CT_REQUIRE(nearly_equal(math::dot(w0, w1), 120.0f));

    const vector<double, 4> d0{1.0, 2.0, 3.0, 4.0};
    const vector<double, 4> d1{2.0, 3.0, 4.0, 5.0};
    CT_REQUIRE(nearly_equal(math::dot(d0, d1), 40.0));
}

void test_vec3_cross()
{
    using math::vec3f;

    const vec3f ex{1.0f, 0.0f, 0.0f};
    const vec3f ey{0.0f, 1.0f, 0.0f};

    const auto ez = math::cross(ex, ey);
    CT_REQUIRE(nearly_equal(ez.x(), 0.0f));
    CT_REQUIRE(nearly_equal(ez.y(), 0.0f));
    CT_REQUIRE(nearly_equal(ez.z(), 1.0f));
}

void test_vec_swizzles()
{
    using math::vec2f;
    using math::vec3f;
    using math::vec4f;

    const vec2f a{1.0f, 2.0f};
    const auto ayx = a.yx();
    CT_REQUIRE(nearly_equal(ayx.x(), 2.0f));
    CT_REQUIRE(nearly_equal(ayx.y(), 1.0f));

    const vec3f b{1.0f, 2.0f, 3.0f};
    const auto bxy = b.xy();
    CT_REQUIRE(nearly_equal(bxy.x(), 1.0f));
    CT_REQUIRE(nearly_equal(bxy.y(), 2.0f));

    // zyx is not one of the named swizzles; the indexed form covers it.
    const auto bzyx = b.swizzle<2, 1, 0>();
    CT_REQUIRE(nearly_equal(bzyx.x(), 3.0f));
    CT_REQUIRE(nearly_equal(bzyx.y(), 2.0f));
    CT_REQUIRE(nearly_equal(bzyx.z(), 1.0f));

    const vec4f c{1.0f, 2.0f, 3.0f, 4.0f};
    const auto czw = c.zw();
    CT_REQUIRE(nearly_equal(czw.x(), 3.0f));
    CT_REQUIRE(nearly_equal(czw.y(), 4.0f));

    const auto cwzyx = c.swizzle<3, 2, 1, 0>();
    CT_REQUIRE(nearly_equal(cwzyx.x(), 4.0f));
    CT_REQUIRE(nearly_equal(cwzyx.y(), 3.0f));
    CT_REQUIRE(nearly_equal(cwzyx.z(), 2.0f));
    CT_REQUIRE(nearly_equal(cwzyx.w(), 1.0f));

    // Repeats are allowed, and the result length follows the index count.
    const auto s = c.swizzle<3, 0, 0, 2>();
    CT_REQUIRE(nearly_equal(s.x(), 4.0f));
    CT_REQUIRE(nearly_equal(s.y(), 1.0f));
    CT_REQUIRE(nearly_equal(s.z(), 1.0f));
    CT_REQUIRE(nearly_equal(s.w(), 3.0f));
}

void test_vec_is_an_aggregate()
{
    using math::vec3f;

    // The single array member is what keeps brace init, structured bindings
    // and constexpr evaluation all working on the same type.
    static_assert(sizeof(vec3f) == 3 * sizeof(float));
    static_assert(std::is_trivially_copyable_v<vec3f>);

    constexpr vec3f v{1.0f, 2.0f, 3.0f};
    static_assert(v.x() == 1.0f && v.y() == 2.0f && v.z() == 3.0f);
    static_assert(math::dot(v, v) == 14.0f);

    CT_REQUIRE(nearly_equal(math::vec3f::axis<1>().y(), 1.0f));
    CT_REQUIRE(nearly_equal(math::vec3f::filled(2.0f).z(), 2.0f));
    CT_REQUIRE(nearly_equal(math::vec3f::zero().x(), 0.0f));
}

} // namespace

int main()
{
    test_vec2f_basics();
    test_vecN_helpers();
    test_vec3_cross();
    test_vec_swizzles();
    test_vec_is_an_aggregate();
    return 0;
}
