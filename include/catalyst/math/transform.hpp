#pragma once

#include "detail/concepts.hpp"
#include "detail/scalar_math.hpp"
#include "geometry.hpp"
#include "matrix.hpp"
#include "scalar.hpp"
#include "vector.hpp"

#include <cstddef>

namespace math
{

    // ---------------------------------------------------------------------
    // Transformations
    // ---------------------------------------------------------------------
    //
    // Conventions, chosen once here and shared by quaternion.hpp:
    //
    //   * Column vectors: a transform is applied as m * v, and a chain
    //     a * b * v applies b first. This is the GLSL convention.
    //   * Right-handed coordinates. With x right and y up, z points out of
    //     the screen, and a positive angle turns counter-clockwise when
    //     looking down the axis toward the origin (x -> y, y -> z, z -> x).
    //   * The projections map view-space depth to clip-space z in [0, 1],
    //     as Vulkan, Direct3D and Metal expect. Cameras look down -z, so
    //     near and far are positive distances in front of the eye.
    //   * Angles are radians. radians() and degrees() convert.
    //
    // An N-dimensional affine transform is an (N + 1) x (N + 1) homogeneous
    // matrix. translation(), affine() and the projections build them;
    // transform_point() and transform_direction() apply them to plain
    // N-vectors, so callers never touch the homogeneous coordinate.
    //
    // Everything except the functions that need acos is constexpr: the
    // trigonometry goes through detail::sin_value and friends.

    // ---- linear parts ----------------------------------------------------

    // 2D rotation by angle: x -> y is positive.
    template <floating_numeric F>
    constexpr matrix<F, 2, 2> rotation(F angle) noexcept
    {
        const F c = detail::cos_value(angle);
        const F s = detail::sin_value(angle);
        return matrix<F, 2, 2>::from_rows({{c, -s}, {s, c}});
    }

    template <floating_numeric F>
    constexpr matrix<F, 3, 3> rotation_x(F angle) noexcept
    {
        const F c = detail::cos_value(angle);
        const F s = detail::sin_value(angle);
        return matrix<F, 3, 3>::from_rows({{1, 0, 0}, {0, c, -s}, {0, s, c}});
    }

    template <floating_numeric F>
    constexpr matrix<F, 3, 3> rotation_y(F angle) noexcept
    {
        const F c = detail::cos_value(angle);
        const F s = detail::sin_value(angle);
        return matrix<F, 3, 3>::from_rows({{c, 0, s}, {0, 1, 0}, {-s, 0, c}});
    }

    template <floating_numeric F>
    constexpr matrix<F, 3, 3> rotation_z(F angle) noexcept
    {
        const F c = detail::cos_value(angle);
        const F s = detail::sin_value(angle);
        return matrix<F, 3, 3>::from_rows({{c, -s, 0}, {s, c, 0}, {0, 0, 1}});
    }

    // 3D rotation about an arbitrary axis (Rodrigues). The axis need not
    // be normalized; a zero axis is a precondition violation.
    template <vector_like V, scalar_like S>
        requires(V::length == 3)
    constexpr auto rotation(const V &axis, S angle) noexcept
    {
        using F = detail::floating_t<detail::common_element_t<V, S>>;
        const vector<F, 3> k = normalized(axis.template cast<F>());
        const F c = detail::cos_value(static_cast<F>(angle));
        const F s = detail::sin_value(static_cast<F>(angle));
        const F t = F(1) - c;
        return matrix<F, 3, 3>::from_rows({
            {t * k.x() * k.x() + c, t * k.x() * k.y() - s * k.z(), t * k.x() * k.z() + s * k.y()},
            {t * k.x() * k.y() + s * k.z(), t * k.y() * k.y() + c, t * k.y() * k.z() - s * k.x()},
            {t * k.x() * k.z() - s * k.y(), t * k.y() * k.z() + s * k.x(), t * k.z() * k.z() + c},
        });
    }

    // Diagonal scale matrix: one factor per axis, or one for all N axes.
    template <vector_like V>
    constexpr auto scaling(const V &factors) noexcept
    {
        using T = typename detail::element<V>::type;
        return matrix<T, V::length, V::length>::from_diagonal(factors);
    }

    template <std::size_t N, scalar_like S>
        requires(N > 0)
    constexpr matrix<S, N, N> scaling(S factor) noexcept
    {
        return matrix<S, N, N>::from_diagonal(vector<S, N>::filled(factor));
    }

    // ---- homogeneous matrices --------------------------------------------

    // Embeds an N x N linear map, and optionally a translation, in the
    // (N + 1) x (N + 1) homogeneous matrix [L t; 0 1].
    template <matrix_like L>
        requires(L::rows == L::cols)
    constexpr auto affine(const L &linear) noexcept
    {
        using T = typename L::value_type;
        constexpr std::size_t N = L::rows;
        return detail::generate<T, N + 1, N + 1, detail::order_of<L>()>([&](std::size_t i, std::size_t j) {
            if (i < N && j < N)
                return linear(i, j);
            return i == j ? T{1} : T{};
        });
    }

    template <matrix_like L, vector_like V>
        requires(L::rows == L::cols && V::length == L::rows)
    constexpr auto affine(const L &linear, const V &translation) noexcept
    {
        using T = detail::common_element_t<L, V>;
        constexpr std::size_t N = L::rows;
        return detail::generate<T, N + 1, N + 1, detail::order_of<L>()>([&](std::size_t i, std::size_t j) {
            if (i < N && j < N)
                return static_cast<T>(linear(i, j));
            if (i < N)
                return static_cast<T>(translation[i]);
            return j == N ? T{1} : T{};
        });
    }

    template <vector_like V>
    constexpr auto translation(const V &offset) noexcept
    {
        using T = typename detail::element<V>::type;
        return affine(matrix<T, V::length, V::length>::identity(), offset);
    }

    // The two halves of a homogeneous matrix.
    template <matrix_like M>
        requires(M::rows > 1 && M::cols > 1)
    constexpr auto linear_part(const M &m) noexcept
    {
        return submatrix<0, 0, M::rows - 1, M::cols - 1>(m);
    }

    template <matrix_like M>
        requires(M::rows > 1 && M::cols > 1)
    constexpr auto translation_part(const M &m) noexcept
    {
        return detail::generate<typename M::value_type, M::rows - 1>([&](std::size_t i) { return m(i, M::cols - 1); });
    }

    // Applies an (N + 1) x (N + 1) matrix to an N-point (w = 1, divided by
    // the resulting w, which is 1 for affine maps and the perspective
    // divide for projections) or to an N-direction (w = 0: translation is
    // ignored).
    template <matrix_like M, vector_like V>
        requires(M::rows == M::cols && V::length + 1 == M::cols)
    constexpr auto transform_point(const M &m, const V &point) noexcept
    {
        using T = detail::common_element_t<M, V>;
        constexpr std::size_t N = V::length;
        vector<T, N> result{};
        T w{};
        for (std::size_t k = 0; k < N; ++k)
            w = static_cast<T>(w + m(N, k) * point[k]);
        w = static_cast<T>(w + m(N, N));
        for (std::size_t i = 0; i < N; ++i)
        {
            T total{};
            for (std::size_t k = 0; k < N; ++k)
                total = static_cast<T>(total + m(i, k) * point[k]);
            result.values[i] = static_cast<T>((total + m(i, N)) / w);
        }
        return result;
    }

    template <matrix_like M, vector_like V>
        requires(M::rows == M::cols && V::length + 1 == M::cols)
    constexpr auto transform_direction(const M &m, const V &direction) noexcept
    {
        using T = detail::common_element_t<M, V>;
        constexpr std::size_t N = V::length;
        return detail::generate<T, N>([&](std::size_t i) {
            T total{};
            for (std::size_t k = 0; k < N; ++k)
                total = static_cast<T>(total + m(i, k) * direction[k]);
            return total;
        });
    }

    // ---- cameras ---------------------------------------------------------

    // View matrix for an eye at `eye` looking at `target`, with `up`
    // fixing the roll. Maps world space to a view space whose camera sits
    // at the origin looking down -z with +y up.
    template <vector_like E, vector_like C, vector_like U>
        requires(E::length == 3 && C::length == 3 && U::length == 3)
    constexpr auto look_at(const E &eye, const C &target, const U &up) noexcept
    {
        using F = detail::floating_t<detail::common_element_t<E, detail::common_element_t<C, U>>>;
        const vector<F, 3> e = eye.template cast<F>();
        const vector<F, 3> f = normalized(target.template cast<F>() - e);
        const vector<F, 3> s = normalized(cross(f, up.template cast<F>()));
        const vector<F, 3> u = cross(s, f);
        return matrix<F, 4, 4>::from_rows({
            {s.x(), s.y(), s.z(), -dot(s, e)},
            {u.x(), u.y(), u.z(), -dot(u, e)},
            {-f.x(), -f.y(), -f.z(), dot(f, e)},
            {0, 0, 0, 1},
        });
    }

    // Perspective projection from a vertical field of view (radians) and a
    // width / height aspect ratio. Points between the near and far planes
    // map to z in [0, 1], with near at 0.
    template <floating_numeric F>
    constexpr matrix<F, 4, 4> perspective(F fov_y, F aspect, F near, F far) noexcept
    {
        const F f = F(1) / detail::tan_value(fov_y / F(2));
        return matrix<F, 4, 4>::from_rows({
            {f / aspect, 0, 0, 0},
            {0, f, 0, 0},
            {0, 0, far / (near - far), -(far * near) / (far - near)},
            {0, 0, -1, 0},
        });
    }

    // Orthographic projection of the view-space box [left, right] x
    // [bottom, top] x [-far, -near] onto [-1, 1] x [-1, 1] x [0, 1].
    template <floating_numeric F>
    constexpr matrix<F, 4, 4> orthographic(F left, F right, F bottom, F top, F near, F far) noexcept
    {
        return matrix<F, 4, 4>::from_rows({
            {F(2) / (right - left), 0, 0, -(right + left) / (right - left)},
            {0, F(2) / (top - bottom), 0, -(top + bottom) / (top - bottom)},
            {0, 0, F(-1) / (far - near), -near / (far - near)},
            {0, 0, 0, 1},
        });
    }

    // Symmetric orthographic projection of a width x height view volume.
    template <floating_numeric F>
    constexpr matrix<F, 4, 4> orthographic(F width, F height, F near, F far) noexcept
    {
        return orthographic(-width / F(2), width / F(2), -height / F(2), height / F(2), near, far);
    }

} // namespace math

