#pragma once

#include "detail/concepts.hpp"
#include "detail/hash.hpp"
#include "detail/scalar_math.hpp"
#include "geometry.hpp"
#include "matrix.hpp"
#include "scalar.hpp"
#include "vector.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <functional>

namespace math
{

    // ---------------------------------------------------------------------
    // quaternion
    // ---------------------------------------------------------------------
    //
    // A rotation quaternion w + xi + yj + zk, stored as {x, y, z, w} so the
    // memory layout matches a vec4 and the usual GPU convention. It is an
    // aggregate, but unlike vector its default is the identity rotation,
    // because an all-zero quaternion is not a rotation at all:
    //
    //   quaternion<float> q;             // identity
    //   quaternion<float>{x, y, z, w};   // explicit components
    //
    // The rotation functions (to_matrix, rotate, slerp) assume a unit
    // quaternion; normalized() restores that after accumulated products.
    // Conventions follow transform.hpp: right-handed, angles in radians,
    // to_matrix() yields the same matrix as rotation(axis, angle).

    template <typename T>
        requires floating_numeric<T>
    struct quaternion;

    namespace detail
    {
        template <typename X>
        struct is_quaternion : std::false_type
        {
        };

        template <typename T>
        struct is_quaternion<quaternion<T>> : std::true_type
        {
        };
    } // namespace detail

    template <typename X>
    concept quaternion_like = detail::is_quaternion<detail::bare<X>>::value;

    template <typename T>
        requires floating_numeric<T>
    struct quaternion
    {
        using value_type = T;
        using vector_type = vector<T, 3>;
        using matrix_type = matrix<T, 3, 3>;

        T x{};
        T y{};
        T z{};
        T w{1};

        // ---- factories ---------------------------------------------------

        static constexpr quaternion identity() noexcept { return quaternion{}; }

        // Rotation by `angle` about `axis`, which need not be normalized.
        template <vector_like V>
            requires(V::length == 3)
        static constexpr quaternion from_axis_angle(const V &axis, T angle) noexcept
        {
            // Qualified: the unqualified name would find the member.
            const vector_type k = math::normalized(axis.template cast<T>());
            const T half = angle / T(2);
            const T s = detail::sin_value(half);
            return {k.x() * s, k.y() * s, k.z() * s, detail::cos_value(half)};
        }

        // Rotation carried by a 3 x 3 rotation matrix (Shepperd's method:
        // pick the largest of the four squared components first so the
        // divisions are well conditioned).
        template <matrix_like M>
            requires(M::rows == 3 && M::cols == 3)
        static constexpr quaternion from_rotation_matrix(const M &m) noexcept
        {
            const T m00 = static_cast<T>(m(0, 0)), m01 = static_cast<T>(m(0, 1)), m02 = static_cast<T>(m(0, 2));
            const T m10 = static_cast<T>(m(1, 0)), m11 = static_cast<T>(m(1, 1)), m12 = static_cast<T>(m(1, 2));
            const T m20 = static_cast<T>(m(2, 0)), m21 = static_cast<T>(m(2, 1)), m22 = static_cast<T>(m(2, 2));
            const T trace = m00 + m11 + m22;
            if (trace > T{})
            {
                const T s = detail::sqrt_value(trace + T(1)) * T(2);
                return {(m21 - m12) / s, (m02 - m20) / s, (m10 - m01) / s, s / T(4)};
            }
            if (m00 > m11 && m00 > m22)
            {
                const T s = detail::sqrt_value(T(1) + m00 - m11 - m22) * T(2);
                return {s / T(4), (m01 + m10) / s, (m02 + m20) / s, (m21 - m12) / s};
            }
            if (m11 > m22)
            {
                const T s = detail::sqrt_value(T(1) + m11 - m00 - m22) * T(2);
                return {(m01 + m10) / s, s / T(4), (m12 + m21) / s, (m02 - m20) / s};
            }
            const T s = detail::sqrt_value(T(1) + m22 - m00 - m11) * T(2);
            return {(m02 + m20) / s, (m12 + m21) / s, s / T(4), (m10 - m01) / s};
        }

        // ---- parts -------------------------------------------------------

        constexpr vector_type vector_part() const noexcept { return {x, y, z}; }
        constexpr T scalar_part() const noexcept { return w; }
        constexpr vector<T, 4> to_vector() const noexcept { return {x, y, z, w}; }

        // Rotation axis (unit) and angle in [0, pi] of a unit quaternion.
        // The identity has no axis; +x is returned for it. angle() is not
        // constexpr: it needs acos.
        constexpr vector_type axis() const noexcept
        {
            const vector_type v{x, y, z};
            const T length_squared = dot(v, v);
            if (length_squared == T{})
                return vector_type::template axis<0>();
            return v / detail::sqrt_value(length_squared);
        }

        T angle() const noexcept { return T(2) * std::acos(std::clamp(w, T(-1), T(1))); }

        // The 3 x 3 rotation matrix of a unit quaternion.
        constexpr matrix_type to_matrix() const noexcept
        {
            const T xx = x * x, yy = y * y, zz = z * z;
            const T xy = x * y, xz = x * z, yz = y * z;
            const T wx = w * x, wy = w * y, wz = w * z;
            return matrix_type::from_rows({
                {T(1) - T(2) * (yy + zz), T(2) * (xy - wz), T(2) * (xz + wy)},
                {T(2) * (xy + wz), T(1) - T(2) * (xx + zz), T(2) * (yz - wx)},
                {T(2) * (xz - wy), T(2) * (yz + wx), T(1) - T(2) * (xx + yy)},
            });
        }

        // ---- algebra -----------------------------------------------------

        constexpr quaternion conjugate() const noexcept { return {-x, -y, -z, w}; }

        constexpr T magnitude_squared() const noexcept { return x * x + y * y + z * z + w * w; }
        constexpr T magnitude() const noexcept { return detail::sqrt_value(magnitude_squared()); }

        constexpr quaternion normalized() const noexcept
        {
            const T inv = T(1) / magnitude();
            return {x * inv, y * inv, z * inv, w * inv};
        }

        // Multiplicative inverse; equals conjugate() for a unit quaternion.
        constexpr quaternion inverse() const noexcept
        {
            const T inv = T(1) / magnitude_squared();
            return {-x * inv, -y * inv, -z * inv, w * inv};
        }

        // Rotates v by a unit quaternion: q v q*.
        template <vector_like V>
            requires(V::length == 3)
        constexpr vector_type rotate(const V &v) const noexcept
        {
            const vector_type u{x, y, z};
            const vector_type p = v.template cast<T>();
            const vector_type t = cross(u, p) * T(2);
            return p + t * w + cross(u, t);
        }

        // ---- comparison --------------------------------------------------

        constexpr bool operator==(const quaternion &) const noexcept = default;

        // ---- compound assignment -----------------------------------------

        constexpr quaternion &operator+=(const quaternion &o) noexcept
        {
            x += o.x;
            y += o.y;
            z += o.z;
            w += o.w;
            return *this;
        }

        constexpr quaternion &operator-=(const quaternion &o) noexcept
        {
            x -= o.x;
            y -= o.y;
            z -= o.z;
            w -= o.w;
            return *this;
        }

        // Hamilton product: (*this) * o applies o first, then *this, so
        // it composes like matrices.
        constexpr quaternion &operator*=(const quaternion &o) noexcept
        {
            const quaternion a = *this;
            x = a.w * o.x + a.x * o.w + a.y * o.z - a.z * o.y;
            y = a.w * o.y - a.x * o.z + a.y * o.w + a.z * o.x;
            z = a.w * o.z + a.x * o.y - a.y * o.x + a.z * o.w;
            w = a.w * o.w - a.x * o.x - a.y * o.y - a.z * o.z;
            return *this;
        }

        constexpr quaternion &operator*=(T s) noexcept
        {
            x *= s;
            y *= s;
            z *= s;
            w *= s;
            return *this;
        }

        constexpr quaternion &operator/=(T s) noexcept
        {
            x /= s;
            y /= s;
            z /= s;
            w /= s;
            return *this;
        }

        constexpr quaternion operator-() const noexcept { return {-x, -y, -z, -w}; }

        friend constexpr quaternion operator+(quaternion a, const quaternion &b) noexcept { return a += b; }
        friend constexpr quaternion operator-(quaternion a, const quaternion &b) noexcept { return a -= b; }
        friend constexpr quaternion operator*(quaternion a, const quaternion &b) noexcept { return a *= b; }
        friend constexpr quaternion operator*(quaternion a, T s) noexcept { return a *= s; }
        friend constexpr quaternion operator*(T s, quaternion a) noexcept { return a *= s; }
        friend constexpr quaternion operator/(quaternion a, T s) noexcept { return a /= s; }
    };

    template <floating_numeric T>
    quaternion(T, T, T, T) -> quaternion<T>;

    using quatf = quaternion<float>;
    using quatd = quaternion<double>;

    // ---------------------------------------------------------------------
    // Free functions
    // ---------------------------------------------------------------------
    //
    // The same names the vector overloads use, constrained on
    // quaternion_like so they overload cleanly with them.

    template <quaternion_like Q>
    constexpr auto dot(const Q &a, const Q &b) noexcept
    {
        return a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
    }

    template <quaternion_like Q>
    constexpr auto magnitude(const Q &q) noexcept
    {
        return q.magnitude();
    }

    template <quaternion_like Q>
    constexpr auto normalized(const Q &q) noexcept
    {
        return q.normalized();
    }

    template <quaternion_like Q>
    constexpr auto conjugate(const Q &q) noexcept
    {
        return q.conjugate();
    }

    template <quaternion_like Q>
    constexpr auto inverse(const Q &q) noexcept
    {
        return q.inverse();
    }

    template <quaternion_like Q, vector_like V>
        requires(V::length == 3)
    constexpr auto rotate(const Q &q, const V &v) noexcept
    {
        return q.rotate(v);
    }

    template <quaternion_like Q, scalar_like S = typename Q::value_type>
    constexpr bool approx_equal(const Q &a, const Q &b, S tolerance = detail::default_tolerance<S>()) noexcept
    {
        return approx_equal(a.x, b.x, tolerance) && approx_equal(a.y, b.y, tolerance) &&
               approx_equal(a.z, b.z, tolerance) && approx_equal(a.w, b.w, tolerance);
    }

    // Straight component-wise interpolation, not normalized.
    template <quaternion_like Q, scalar_like S>
    constexpr Q lerp(const Q &a, const Q &b, S t) noexcept
    {
        using T = typename Q::value_type;
        return a + (b - a) * static_cast<T>(t);
    }

    // Normalized lerp along the shorter arc: cheap, commutative in its
    // endpoints, but not constant speed. Good enough for small steps.
    template <quaternion_like Q, scalar_like S>
    constexpr Q nlerp(const Q &a, const Q &b, S t) noexcept
    {
        const Q target = dot(a, b) < 0 ? -b : b;
        return lerp(a, target, t).normalized();
    }

    // Spherical interpolation along the shorter arc at constant angular
    // speed. Falls back to nlerp when the endpoints are nearly parallel,
    // where the formula would divide by a vanishing sine. Not constexpr:
    // it needs acos.
    template <quaternion_like Q, scalar_like S>
    Q slerp(const Q &a, const Q &b, S t) noexcept
    {
        using T = typename Q::value_type;
        T cosine = dot(a, b);
        Q target = b;
        if (cosine < T{})
        {
            cosine = -cosine;
            target = -b;
        }
        if (cosine > T(0.9995))
            return nlerp(a, target, t);
        const T theta = std::acos(cosine);
        const T sine = std::sin(theta);
        const T wa = std::sin((T(1) - static_cast<T>(t)) * theta) / sine;
        const T wb = std::sin(static_cast<T>(t) * theta) / sine;
        return a * wa + target * wb;
    }

} // namespace math

// -------------------------------------------------------------------------
// std integration: hashing and std::format
// -------------------------------------------------------------------------

namespace std
{
    template <typename T>
    struct hash<math::quaternion<T>>
    {
        std::size_t operator()(const math::quaternion<T> &q) const noexcept
        {
            std::size_t seed = 0;
            math::detail::hash_combine(seed, q.x);
            math::detail::hash_combine(seed, q.y);
            math::detail::hash_combine(seed, q.z);
            math::detail::hash_combine(seed, q.w);
            return seed;
        }
    };

    // Formats as "(x, y, z; w)": the vector part, then the scalar. The
    // format spec applies to every component.
    template <typename T, typename CharT>
    struct formatter<math::quaternion<T>, CharT> : formatter<T, CharT>
    {
        template <typename FormatContext>
        auto format(const math::quaternion<T> &q, FormatContext &ctx) const
        {
            auto out = ctx.out();
            *out++ = CharT('(');
            ctx.advance_to(out);
            out = formatter<T, CharT>::format(q.x, ctx);
            *out++ = CharT(',');
            *out++ = CharT(' ');
            ctx.advance_to(out);
            out = formatter<T, CharT>::format(q.y, ctx);
            *out++ = CharT(',');
            *out++ = CharT(' ');
            ctx.advance_to(out);
            out = formatter<T, CharT>::format(q.z, ctx);
            *out++ = CharT(';');
            *out++ = CharT(' ');
            ctx.advance_to(out);
            out = formatter<T, CharT>::format(q.w, ctx);
            *out++ = CharT(')');
            return out;
        }
    };
} // namespace std
