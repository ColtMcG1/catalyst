#pragma once

#include "detail/concepts.hpp"
#include "detail/scalar_math.hpp"
#include "vector.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace math
{

    // ---------------------------------------------------------------------
    // Reductions and geometry
    // ---------------------------------------------------------------------

    //
    // The reductions accumulate in the element type by default, so an
    // integer dot product overflows exactly as a hand-written loop would.
    // Pass an explicit accumulator to widen: dot<long long>(a, b).

    template <typename Accumulator = void, vector_like V>
    constexpr auto sum(const V &v)
    {
        using T = detail::accumulator_t<Accumulator, typename detail::element<V>::type>;
        T total{};
        for (std::size_t i = 0; i < V::length; ++i)
            total = static_cast<T>(total + v[i]);
        return total;
    }

    template <typename Accumulator = void, vector_like V>
    constexpr auto product(const V &v)
    {
        using T = detail::accumulator_t<Accumulator, typename detail::element<V>::type>;
        T total{1};
        for (std::size_t i = 0; i < V::length; ++i)
            total = static_cast<T>(total * v[i]);
        return total;
    }

    template <typename Accumulator = void, vector_like L, vector_like R>
        requires(L::length == R::length)
    constexpr auto dot(const L &lhs, const R &rhs)
    {
        using T = detail::accumulator_t<Accumulator, detail::common_element_t<L, R>>;
        T total{};
        for (std::size_t i = 0; i < L::length; ++i)
            total = static_cast<T>(total + static_cast<T>(lhs[i]) * static_cast<T>(rhs[i]));
        return total;
    }

    // Smallest and largest element.
    template <vector_like V>
    constexpr auto min_value(const V &v)
    {
        using T = typename detail::element<V>::type;
        T best = v[0];
        for (std::size_t i = 1; i < V::length; ++i)
            if (v[i] < best)
                best = v[i];
        return best;
    }

    template <vector_like V>
    constexpr auto max_value(const V &v)
    {
        using T = typename detail::element<V>::type;
        T best = v[0];
        for (std::size_t i = 1; i < V::length; ++i)
            if (best < v[i])
                best = v[i];
        return best;
    }

    template <vector_like L, vector_like R>
        requires(L::length == 3 && R::length == 3)
    constexpr auto cross(const L &a, const R &b)
    {
        using T = detail::common_element_t<L, R>;
        return vector<T, 3>{
            static_cast<T>(a[1] * b[2] - a[2] * b[1]),
            static_cast<T>(a[2] * b[0] - a[0] * b[2]),
            static_cast<T>(a[0] * b[1] - a[1] * b[0]),
        };
    }

    template <typename Accumulator = void, vector_like V>
    constexpr auto magnitude_squared(const V &v)
    {
        return dot<Accumulator>(v, v);
    }

    // Floating result even for integer vectors (as std::sqrt(int) -> double),
    // and usable in constant evaluation through detail::sqrt_value.
    template <vector_like V>
    constexpr auto magnitude(const V &v)
    {
        using F = detail::floating_t<typename detail::element<V>::type>;
        return detail::sqrt_value(static_cast<F>(magnitude_squared(v)));
    }

    template <vector_like V>
    constexpr auto normalized(const V &v)
    {
        return v / magnitude(v);
    }

    template <vector_like L, vector_like R>
        requires(L::length == R::length)
    constexpr auto distance(const L &a, const R &b)
    {
        return magnitude(a - b);
    }

    template <vector_like L, vector_like R, scalar_like S>
        requires(L::length == R::length)
    constexpr auto lerp(const L &a, const R &b, S t)
    {
        return a + (b - a) * t;
    }

    // The component of v along `onto`, and what is left after removing it.
    // `onto` need not be a unit vector.
    template <vector_like L, vector_like R>
        requires(L::length == R::length)
    constexpr auto project(const L &v, const R &onto)
    {
        return onto * (dot(v, onto) / dot(onto, onto));
    }

    template <vector_like L, vector_like R>
        requires(L::length == R::length)
    constexpr auto reject(const L &v, const R &from)
    {
        return v - project(v, from);
    }

    // Mirror of an incident direction about a unit normal (GLSL reflect).
    template <vector_like L, vector_like R>
        requires(L::length == R::length)
    constexpr auto reflect(const L &incident, const R &normal)
    {
        return incident - normal * (2 * dot(normal, incident));
    }

    // Refraction of a unit incident direction through a surface with unit
    // normal, for the ratio of indices of refraction eta (GLSL refract).
    // Total internal reflection yields the zero vector.
    template <vector_like L, vector_like R, scalar_like S>
        requires(L::length == R::length)
    constexpr auto refract(const L &incident, const R &normal, S eta)
    {
        using T = detail::floating_t<detail::common_element_t<detail::common_element_t<L, R>, S>>;
        const T n_dot_i = static_cast<T>(dot(normal, incident));
        const T k = T(1) - static_cast<T>(eta) * static_cast<T>(eta) * (T(1) - n_dot_i * n_dot_i);
        if (k < T{})
            return vector<T, L::length>{};
        return incident.template cast<T>() * static_cast<T>(eta) -
               normal.template cast<T>() * (static_cast<T>(eta) * n_dot_i + detail::sqrt_value(k));
    }

    // Angle between two vectors in radians, in [0, pi]. Not constexpr:
    // there is no constant-evaluation acos.
    template <vector_like L, vector_like R>
        requires(L::length == R::length)
    auto angle_between(const L &a, const R &b)
    {
        using T = detail::floating_t<detail::common_element_t<L, R>>;
        const T cosine = static_cast<T>(dot(a, b)) / (magnitude(a) * magnitude(b));
        return std::acos(std::clamp(cosine, T(-1), T(1)));
    }

    // ---------------------------------------------------------------------
    // Shape
    // ---------------------------------------------------------------------

    namespace detail
    {
        template <typename Part>
        constexpr std::size_t part_length() noexcept
        {
            if constexpr (vector_like<Part>)
                return bare<Part>::length;
            else
                return 1;
        }

        template <typename T, std::size_t N, typename Part>
        constexpr void append_part(vector<T, N> &out, std::size_t &pos, const Part &part) noexcept
        {
            if constexpr (vector_like<Part>)
                for (std::size_t i = 0; i < Part::length; ++i)
                    out.values[pos++] = static_cast<T>(part[i]);
            else
                out.values[pos++] = static_cast<T>(part);
        }
    } // namespace detail

    // Joins vectors and scalars end to end, in the common element type:
    // concat(position, 1.0f) is the homogeneous vec4 of a vec3 position.
    template <typename... Parts>
        requires(sizeof...(Parts) > 0 && ((vector_like<Parts> || scalar_like<Parts>) && ...))
    constexpr auto concat(const Parts &...parts)
    {
        using T = std::common_type_t<typename detail::element<Parts>::type...>;
        vector<T, (detail::part_length<Parts>() + ...)> result{};
        std::size_t pos = 0;
        (detail::append_part(result, pos, parts), ...);
        return result;
    }

    // The first M elements of v, padded with `fill` when M > length.
    template <std::size_t M, vector_like V, scalar_like S = typename detail::element<V>::type>
        requires(M > 0)
    constexpr auto resize(const V &v, S fill = S{})
    {
        using T = typename detail::element<V>::type;
        vector<T, M> result{};
        for (std::size_t i = 0; i < M; ++i)
            result.values[i] = i < V::length ? static_cast<T>(v[i]) : static_cast<T>(fill);
        return result;
    }

} // namespace math
