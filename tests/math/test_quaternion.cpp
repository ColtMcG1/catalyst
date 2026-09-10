#include <catalyst/math/quaternion.hpp>
#include <catalyst/math/scalar.hpp>
#include <catalyst/math/transform.hpp>
#include <catalyst/math/vector.hpp>

#include "test_common.hpp"

namespace
{

using catalyst::tests::nearly_equal;

constexpr float k_pi = 3.14159265358979323846f;

void test_identity_rotation()
{
    using math::quatf;
    using math::vec3f;

    const quatf q = quatf::identity();
    const vec3f v{1.0f, 2.0f, 3.0f};
    const auto out = math::rotate(q, v);

    CT_REQUIRE(nearly_equal(out.x(), v.x()));
    CT_REQUIRE(nearly_equal(out.y(), v.y()));
    CT_REQUIRE(nearly_equal(out.z(), v.z()));

    // A default-constructed quaternion is the identity, unlike a vector.
    static_assert(quatf{}.w == 1.0f);
}

void test_axis_angle_rotate_90_deg_z()
{
    using math::quatf;
    using math::vec3f;

    const quatf q = quatf::from_axis_angle(vec3f{0.0f, 0.0f, 1.0f}, k_pi * 0.5f);
    const vec3f v{1.0f, 0.0f, 0.0f};
    const auto out = math::rotate(q, v);

    CT_REQUIRE(nearly_equal(out.x(), 0.0f));
    CT_REQUIRE(nearly_equal(out.y(), 1.0f));
    CT_REQUIRE(nearly_equal(out.z(), 0.0f));

    // Angles are radians throughout; radians() is the only degree conversion.
    const quatf q_deg = quatf::from_axis_angle(vec3f{0.0f, 0.0f, 1.0f}, math::radians(90.0f));
    CT_REQUIRE(math::approx_equal(q, q_deg));
}

void test_mul_composes_rotations()
{
    using math::quatf;
    using math::vec3f;

    const quatf qz = quatf::from_axis_angle(vec3f{0.0f, 0.0f, 1.0f}, k_pi * 0.5f);
    const quatf qy = quatf::from_axis_angle(vec3f{0.0f, 1.0f, 0.0f}, k_pi * 0.5f);

    // Apply y then z, the same order a matrix product would.
    const quatf q = math::normalized(qz * qy);

    const vec3f v{1.0f, 0.0f, 0.0f};
    const auto out_step = math::rotate(qz, math::rotate(qy, v));
    const auto out_one = math::rotate(q, v);

    CT_REQUIRE(nearly_equal(out_one.x(), out_step.x()));
    CT_REQUIRE(nearly_equal(out_one.y(), out_step.y()));
    CT_REQUIRE(nearly_equal(out_one.z(), out_step.z()));
}

void test_inverse_undoes_rotation()
{
    using math::quatf;
    using math::vec3f;

    const quatf q = math::normalized(quatf::from_axis_angle(vec3f{1.0f, 2.0f, 3.0f}, k_pi * 0.75f));
    const quatf inv = math::inverse(q);

    const vec3f v{4.0f, -2.0f, 1.0f};
    const auto out = math::rotate(inv, math::rotate(q, v));

    CT_REQUIRE(nearly_equal(out.x(), v.x()));
    CT_REQUIRE(nearly_equal(out.y(), v.y()));
    CT_REQUIRE(nearly_equal(out.z(), v.z()));

    // For a unit quaternion the inverse is the conjugate.
    CT_REQUIRE(math::approx_equal(inv, math::conjugate(q)));
    CT_REQUIRE(nearly_equal(math::magnitude(q), 1.0f));
}

void test_to_matrix_matches_rotation()
{
    using math::quatf;
    using math::vec3f;

    const quatf q = quatf::from_axis_angle(vec3f{0.0f, 0.0f, 1.0f}, k_pi * 0.5f);
    const auto m = q.to_matrix();

    const vec3f v{1.0f, 0.0f, 0.0f};
    const auto out_q = math::rotate(q, v);
    const auto out_m = m * v;

    CT_REQUIRE(nearly_equal(out_m.x(), out_q.x()));
    CT_REQUIRE(nearly_equal(out_m.y(), out_q.y()));
    CT_REQUIRE(nearly_equal(out_m.z(), out_q.z()));

    // to_matrix() and transform::rotation(axis, angle) must agree.
    const auto r = math::rotation(vec3f{0.0f, 0.0f, 1.0f}, k_pi * 0.5f);
    for (std::size_t row = 0; row < 3; ++row)
        for (std::size_t col = 0; col < 3; ++col)
            CT_REQUIRE(nearly_equal(m(row, col), r(row, col), 1e-5f));
}

void test_from_rotation_matrix_round_trips()
{
    using math::quatf;
    using math::vec3f;

    const quatf q = math::normalized(quatf::from_axis_angle(vec3f{0.3f, -0.5f, 0.8f}, k_pi * 0.6f));
    const quatf back = quatf::from_rotation_matrix(q.to_matrix());

    // q and -q are the same rotation, so compare how they act on a vector.
    const vec3f v{1.0f, -2.0f, 0.5f};
    const auto a = math::rotate(q, v);
    const auto b = math::rotate(back, v);

    CT_REQUIRE(nearly_equal(a.x(), b.x(), 1e-4f));
    CT_REQUIRE(nearly_equal(a.y(), b.y(), 1e-4f));
    CT_REQUIRE(nearly_equal(a.z(), b.z(), 1e-4f));
}

void test_slerp_endpoints_and_midpoint()
{
    using math::quatf;
    using math::vec3f;

    const quatf a = quatf::identity();
    const quatf b = quatf::from_axis_angle(vec3f{0.0f, 0.0f, 1.0f}, k_pi * 0.5f);

    CT_REQUIRE(math::approx_equal(math::slerp(a, b, 0.0f), a, 1e-4f));
    CT_REQUIRE(math::approx_equal(math::slerp(a, b, 1.0f), b, 1e-4f));

    // Halfway along the arc is a 45 degree turn about the same axis.
    const quatf mid = math::slerp(a, b, 0.5f);
    const quatf expected = quatf::from_axis_angle(vec3f{0.0f, 0.0f, 1.0f}, k_pi * 0.25f);
    CT_REQUIRE(math::approx_equal(mid, expected, 1e-4f));
    CT_REQUIRE(nearly_equal(math::magnitude(mid), 1.0f, 1e-4f));
}

} // namespace

int main()
{
    test_identity_rotation();
    test_axis_angle_rotate_90_deg_z();
    test_mul_composes_rotations();
    test_inverse_undoes_rotation();
    test_to_matrix_matches_rotation();
    test_from_rotation_matrix_round_trips();
    test_slerp_endpoints_and_midpoint();
    return 0;
}
