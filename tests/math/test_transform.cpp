#include <catalyst/math/linear_algebra.hpp>
#include <catalyst/math/quaternion.hpp>
#include <catalyst/math/transform.hpp>
#include <catalyst/math/vector.hpp>

#include "test_common.hpp"

namespace
{

using catalyst::tests::nearly_equal;

constexpr float k_pi = 3.14159265358979323846f;

void test_translation_affects_points_not_directions()
{
    using math::mat4f;
    using math::vec3f;

    const vec3f t{1.0f, 2.0f, 3.0f};
    const mat4f m = math::translation(t);

    const vec3f p{4.0f, 5.0f, 6.0f};

    const auto p_out = math::transform_point(m, p);
    const auto d_out = math::transform_direction(m, p);

    CT_REQUIRE(nearly_equal(p_out.x(), 5.0f));
    CT_REQUIRE(nearly_equal(p_out.y(), 7.0f));
    CT_REQUIRE(nearly_equal(p_out.z(), 9.0f));

    // A direction has no position, so translation must not move it.
    CT_REQUIRE(nearly_equal(d_out.x(), p.x()));
    CT_REQUIRE(nearly_equal(d_out.y(), p.y()));
    CT_REQUIRE(nearly_equal(d_out.z(), p.z()));

    CT_REQUIRE(math::is_identity(math::linear_part(m)));
    const auto tp = math::translation_part(m);
    CT_REQUIRE(nearly_equal(tp.x(), t.x()));
    CT_REQUIRE(nearly_equal(tp.z(), t.z()));
}

void test_affine_composition_matches_manual_application()
{
    using math::quatf;
    using math::vec3f;

    const vec3f t{1.0f, 2.0f, 3.0f};
    const vec3f s{2.0f, 3.0f, 4.0f};
    const quatf r = math::normalized(quatf::from_axis_angle(vec3f{0.0f, 0.0f, 1.0f}, k_pi * 0.5f));

    // There is no single trs() any more: affine() composes a linear part with a
    // translation, and the linear part is the rotation times the scale.
    const auto m = math::affine(r.to_matrix() * math::scaling(s), t);

    const vec3f v{1.0f, 0.0f, 0.0f};
    const auto mv = math::transform_point(m, v);

    // Manual: v' = t + rotate(r, s * v)
    const vec3f sv{s.x() * v.x(), s.y() * v.y(), s.z() * v.z()};
    const vec3f rv = math::rotate(r, sv);
    const vec3f expected{t.x() + rv.x(), t.y() + rv.y(), t.z() + rv.z()};

    CT_REQUIRE(nearly_equal(mv.x(), expected.x(), 1e-5f));
    CT_REQUIRE(nearly_equal(mv.y(), expected.y(), 1e-5f));
    CT_REQUIRE(nearly_equal(mv.z(), expected.z(), 1e-5f));
}

void test_inverse_affine_round_trips_a_point()
{
    using math::quatf;
    using math::vec3f;

    const vec3f t{1.0f, 2.0f, 3.0f};
    const vec3f s{2.0f, 3.0f, 4.0f};
    const quatf r = math::normalized(quatf::from_axis_angle(vec3f{0.0f, 1.0f, 0.0f}, k_pi * 0.25f));

    const auto m = math::affine(r.to_matrix() * math::scaling(s), t);
    const auto inv = math::inverse(m);

    const vec3f p{0.25f, -1.5f, 2.0f};
    const auto p2 = math::transform_point(inv, math::transform_point(m, p));

    CT_REQUIRE(nearly_equal(p2.x(), p.x(), 1e-4f));
    CT_REQUIRE(nearly_equal(p2.y(), p.y(), 1e-4f));
    CT_REQUIRE(nearly_equal(p2.z(), p.z(), 1e-4f));
}

void test_rotation_axes_agree_with_the_axis_angle_form()
{
    using math::vec3f;

    const float a = k_pi * 0.3f;

    const auto rx = math::rotation_x(a);
    const auto ry = math::rotation_y(a);
    const auto rz = math::rotation_z(a);

    const auto ax = math::rotation(vec3f{1.0f, 0.0f, 0.0f}, a);
    const auto ay = math::rotation(vec3f{0.0f, 1.0f, 0.0f}, a);
    const auto az = math::rotation(vec3f{0.0f, 0.0f, 1.0f}, a);

    for (std::size_t r = 0; r < 3; ++r)
    {
        for (std::size_t c = 0; c < 3; ++c)
        {
            CT_REQUIRE(nearly_equal(rx(r, c), ax(r, c), 1e-5f));
            CT_REQUIRE(nearly_equal(ry(r, c), ay(r, c), 1e-5f));
            CT_REQUIRE(nearly_equal(rz(r, c), az(r, c), 1e-5f));
        }
    }

    // Right-handed: a positive turn about z takes x to y.
    const auto turned = math::rotation_z(k_pi * 0.5f) * vec3f{1.0f, 0.0f, 0.0f};
    CT_REQUIRE(nearly_equal(turned.x(), 0.0f, 1e-5f));
    CT_REQUIRE(nearly_equal(turned.y(), 1.0f, 1e-5f));
}

void test_look_at_puts_the_eye_at_the_origin_looking_down_minus_z()
{
    using math::vec3f;

    const vec3f eye{1.0f, 2.0f, 3.0f};
    const vec3f target{1.0f, 2.0f, 2.0f}; // one unit along -z
    const vec3f up{0.0f, 1.0f, 0.0f};

    const auto v = math::look_at(eye, target, up);

    const auto eye_vs = math::transform_point(v, eye);
    CT_REQUIRE(nearly_equal(eye_vs.x(), 0.0f, 1e-5f));
    CT_REQUIRE(nearly_equal(eye_vs.y(), 0.0f, 1e-5f));
    CT_REQUIRE(nearly_equal(eye_vs.z(), 0.0f, 1e-5f));

    const auto target_vs = math::transform_point(v, target);
    CT_REQUIRE(nearly_equal(target_vs.x(), 0.0f, 1e-5f));
    CT_REQUIRE(nearly_equal(target_vs.y(), 0.0f, 1e-5f));

    // The camera looks down -z, so anything in front of it has negative z.
    CT_REQUIRE(target_vs.z() < 0.0f);
}

void test_perspective_maps_near_and_far_to_zero_and_one()
{
    using math::vec4f;

    const float fov_y = k_pi * 0.5f;
    const float aspect = 16.0f / 9.0f;
    const float zn = 1.0f;
    const float zf = 10.0f;

    const auto p = math::perspective(fov_y, aspect, zn, zf);

    // The camera looks down -z, so near and far sit at -zn and -zf.
    const vec4f v_near{0.0f, 0.0f, -zn, 1.0f};
    const vec4f v_far{0.0f, 0.0f, -zf, 1.0f};

    const auto c_near = p * v_near;
    const auto c_far = p * v_far;

    CT_REQUIRE(nearly_equal(c_near.z() / c_near.w(), 0.0f, 1e-5f));
    CT_REQUIRE(nearly_equal(c_far.z() / c_far.w(), 1.0f, 1e-5f));

    // A point on the right edge of the frustum lands on x = +w.
    const float half_h = zn * std::tan(fov_y * 0.5f);
    const vec4f edge{half_h * aspect, 0.0f, -zn, 1.0f};
    const auto c_edge = p * edge;
    CT_REQUIRE(nearly_equal(c_edge.x() / c_edge.w(), 1.0f, 1e-4f));
}

void test_orthographic_maps_near_and_far_to_zero_and_one()
{
    using math::vec4f;

    const float left = -2.0f;
    const float right = 2.0f;
    const float bottom = -1.0f;
    const float top = 1.0f;
    const float zn = 1.0f;
    const float zf = 10.0f;

    const auto o = math::orthographic(left, right, bottom, top, zn, zf);

    const vec4f v_near{0.0f, 0.0f, -zn, 1.0f};
    const vec4f v_far{0.0f, 0.0f, -zf, 1.0f};

    const auto c_near = o * v_near;
    const auto c_far = o * v_far;

    CT_REQUIRE(nearly_equal(c_near.z() / c_near.w(), 0.0f, 1e-5f));
    CT_REQUIRE(nearly_equal(c_far.z() / c_far.w(), 1.0f, 1e-5f));

    // The centred overload is the symmetric case of the explicit one.
    const auto centred = math::orthographic(right - left, top - bottom, zn, zf);
    for (std::size_t r = 0; r < 4; ++r)
        for (std::size_t c = 0; c < 4; ++c)
            CT_REQUIRE(nearly_equal(centred(r, c), o(r, c), 1e-5f));
}

} // namespace

int main()
{
    test_translation_affects_points_not_directions();
    test_affine_composition_matches_manual_application();
    test_inverse_affine_round_trips_a_point();
    test_rotation_axes_agree_with_the_axis_angle_form();
    test_look_at_puts_the_eye_at_the_origin_looking_down_minus_z();
    test_perspective_maps_near_and_far_to_zero_and_one();
    test_orthographic_maps_near_and_far_to_zero_and_one();
    return 0;
}
