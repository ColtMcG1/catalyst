#pragma once

#include "detail/concepts.hpp"
#include "detail/scalar_math.hpp"
#include "matrix.hpp"
#include "scalar.hpp"
#include "vector.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>

namespace math
{

    // ---------------------------------------------------------------------
    // Element-wise functions
    // ---------------------------------------------------------------------
    //
    // Scalar functions lifted to whole containers: each applies the scalar
    // form to every element and returns a container of the same shape. The
    // vector and matrix overloads are constrained on disjoint concepts, so
    // they coexist under one name; the matrix set is the subset that has a
    // meaning independent of shape.

    // ---- vectors ---------------------------------------------------------
    //
    // Each applies the scalar function to every element. The rounding
    // functions keep the element type and are the identity on integer
    // vectors; sqrt promotes integer vectors to double, as magnitude does.
    // pow and exp are not constexpr: they go straight to <cmath>.

    template <vector_like V>
    constexpr auto abs(const V &v)
    {
        return detail::map(v, [](auto e)
                           { return detail::abs_value(e); });
    }

    // -1, 0 or +1 per element, in the element type.
    template <vector_like V>
    constexpr auto sign(const V &v)
    {
        using T = typename detail::element<V>::type;
        return detail::map(v, [](auto e)
                           { return T{} < e ? T{1} : e < T{} ? T(-1)
                                                             : T{}; });
    }

    template <vector_like V>
    constexpr auto sqrt(const V &v)
    {
        using F = detail::floating_t<typename detail::element<V>::type>;
        return detail::generate<F, V::length>([&](std::size_t i)
                                              { return detail::sqrt_value(static_cast<F>(v[i])); });
    }

    template <vector_like V>
    constexpr auto floor(const V &v)
    {
        using T = typename detail::element<V>::type;
        if constexpr (std::is_floating_point_v<T>)
            return detail::map(v, [](auto e)
                               { return detail::floor_value(e); });
        else
            return detail::map(v, [](auto e)
                               { return e; });
    }

    template <vector_like V>
    constexpr auto ceil(const V &v)
    {
        using T = typename detail::element<V>::type;
        if constexpr (std::is_floating_point_v<T>)
            return detail::map(v, [](auto e)
                               { return detail::ceil_value(e); });
        else
            return detail::map(v, [](auto e)
                               { return e; });
    }

    template <vector_like V>
    constexpr auto trunc(const V &v)
    {
        using T = typename detail::element<V>::type;
        if constexpr (std::is_floating_point_v<T>)
            return detail::map(v, [](auto e)
                               { return detail::trunc_value(e); });
        else
            return detail::map(v, [](auto e)
                               { return e; });
    }

    template <vector_like V>
    constexpr auto round(const V &v)
    {
        using T = typename detail::element<V>::type;
        if constexpr (std::is_floating_point_v<T>)
            return detail::map(v, [](auto e)
                               { return detail::round_value(e); });
        else
            return detail::map(v, [](auto e)
                               { return e; });
    }

    template <vector_like V, scalar_like S>
    auto pow(const V &v, S exponent)
    {
        using F = detail::floating_t<detail::common_element_t<V, S>>;
        return detail::generate<F, V::length>(
            [&](std::size_t i)
            { return std::pow(static_cast<F>(v[i]), static_cast<F>(exponent)); });
    }

    template <vector_like L, vector_like R>
        requires(L::length == R::length)
    auto pow(const L &v, const R &exponent)
    {
        using F = detail::floating_t<detail::common_element_t<L, R>>;
        return detail::generate<F, L::length>(
            [&](std::size_t i)
            { return std::pow(static_cast<F>(v[i]), static_cast<F>(exponent[i])); });
    }

    template <vector_like V>
    auto exp(const V &v)
    {
        using F = detail::floating_t<typename detail::element<V>::type>;
        return detail::generate<F, V::length>([&](std::size_t i)
                                              { return std::exp(static_cast<F>(v[i])); });
    }

    // 0 where v < edge, 1 elsewhere (GLSL step), in v's element type.
    template <typename S, vector_like V>
        requires scalar_like<S>
    constexpr auto step(S edge, const V &v)
    {
        using T = typename detail::element<V>::type;
        return detail::map(v, [=](auto e)
                           { return e < edge ? T{} : T{1}; });
    }

    template <vector_like E, vector_like V>
        requires(E::length == V::length)
    constexpr auto step(const E &edge, const V &v)
    {
        using T = typename detail::element<V>::type;
        return detail::generate<T, V::length>([&](std::size_t i)
                                              { return v[i] < edge[i] ? T{} : T{1}; });
    }

    // Hermite ramp from 0 at edge0 to 1 at edge1 (GLSL smoothstep). Floating
    // result.
    template <typename S, vector_like V>
        requires scalar_like<S>
    constexpr auto smoothstep(S edge0, S edge1, const V &v)
    {
        using F = detail::floating_t<detail::common_element_t<V, S>>;
        return detail::generate<F, V::length>([&](std::size_t i)
                                              {
            const F t = std::clamp(
                (static_cast<F>(v[i]) - static_cast<F>(edge0)) / (static_cast<F>(edge1) - static_cast<F>(edge0)), 
                F(0), 
                F(1));
            return t * t * (F(3) - F(2) * t); });
    }

    // clamp(v, 0, 1) in the element type.
    template <vector_like V>
    constexpr auto saturate(const V &v)
    {
        using T = typename detail::element<V>::type;
        return detail::map(v, [](auto e)
                           { return std::clamp(e, T{}, T{1}); });
    }

    template <vector_like L, vector_like R>
        requires(L::length == R::length)
    constexpr auto min(const L &a, const R &b)
    {
        using T = detail::common_element_t<L, R>;
        return detail::zip(a, b, [](auto x, auto y)
                           { return std::min(static_cast<T>(x), static_cast<T>(y)); });
    }

    template <vector_like L, vector_like R>
        requires(L::length == R::length)
    constexpr auto max(const L &a, const R &b)
    {
        using T = detail::common_element_t<L, R>;
        return detail::zip(a, b, [](auto x, auto y)
                           { return std::max(static_cast<T>(x), static_cast<T>(y)); });
    }

    template <vector_like V, scalar_like S>
    constexpr auto clamp(const V &v, S lo, S hi)
    {
        using T = detail::common_element_t<V, S>;
        return detail::map(v, [=](auto e)
                           { return std::clamp(static_cast<T>(e), static_cast<T>(lo), static_cast<T>(hi)); });
    }

    template <vector_like V, vector_like Lo, vector_like Hi>
        requires(V::length == Lo::length && V::length == Hi::length)
    constexpr auto clamp(const V &v, const Lo &lo, const Hi &hi)
    {
        using T = detail::common_element_t<V, detail::common_element_t<Lo, Hi>>;
        return detail::generate<T, V::length>([&](std::size_t i)
                                              { return std::clamp(static_cast<T>(v[i]), static_cast<T>(lo[i]), static_cast<T>(hi[i])); });
    }

    // ---- matrices --------------------------------------------------------
    //
    // The subset of the above that has a meaning independent of shape,
    // constrained on matrix_like instead. The two concepts are disjoint, so
    // these overload cleanly with the vector versions.

    template <matrix_like M>
        requires std::is_signed_v<typename M::value_type>
    constexpr auto abs(const M &m)
    {
        return detail::map(m, [](auto e)
                           { return e < 0 ? -e : e; });
    }

    template <matrix_like L, matrix_like R>
        requires(L::rows == R::rows && L::cols == R::cols)
    constexpr auto min(const L &a, const R &b)
    {
        using T = detail::common_element_t<L, R>;
        return detail::zip(a, b, [](auto x, auto y)
                           { return std::min(static_cast<T>(x), static_cast<T>(y)); });
    }

    template <matrix_like L, matrix_like R>
        requires(L::rows == R::rows && L::cols == R::cols)
    constexpr auto max(const L &a, const R &b)
    {
        using T = detail::common_element_t<L, R>;
        return detail::zip(a, b, [](auto x, auto y)
                           { return std::max(static_cast<T>(x), static_cast<T>(y)); });
    }

    template <matrix_like M, scalar_like S>
    constexpr auto clamp(const M &m, S lo, S hi)
    {
        using T = detail::common_element_t<M, S>;
        return detail::map(m, [=](auto e)
                           { return std::clamp(static_cast<T>(e), static_cast<T>(lo), static_cast<T>(hi)); });
    }

    // ---- approximate comparison ------------------------------------------
    //
    // The scalar approx_equal (scalar.hpp) lifted element-wise: every pair
    // of elements must lie within the absolute tolerance. The default
    // tolerance is that of the common element type, so two integer
    // containers compare exactly.

    template <vector_like L, vector_like R, scalar_like S = detail::common_element_t<L, R>>
        requires(L::length == R::length)
    constexpr bool approx_equal(const L &a, const R &b, S tolerance = detail::default_tolerance<S>()) noexcept
    {
        for (std::size_t i = 0; i < L::length; ++i)
            if (!approx_equal(a[i], b[i], tolerance))
                return false;
        return true;
    }

    template <vector_like V, scalar_like S = typename detail::element<V>::type>
    constexpr bool is_zero(const V &v, S tolerance = detail::default_tolerance<S>()) noexcept
    {
        for (std::size_t i = 0; i < V::length; ++i)
            if (!approx_equal(v[i], S{}, tolerance))
                return false;
        return true;
    }

    // Element-wise approx_equal, as for vectors: every (i, j) pair must lie
    // within the absolute tolerance, which defaults to that of the common
    // element type.
    template <matrix_like L, matrix_like R, scalar_like S = detail::common_element_t<L, R>>
        requires(L::rows == R::rows && L::cols == R::cols)
    constexpr bool approx_equal(const L &a, const R &b, S tolerance = detail::default_tolerance<S>()) noexcept
    {
        for (std::size_t i = 0; i < L::rows; ++i)
            for (std::size_t j = 0; j < L::cols; ++j)
                if (!approx_equal(a(i, j), b(i, j), tolerance))
                    return false;
        return true;
    }

    template <matrix_like M, scalar_like S = typename detail::element<M>::type>
    constexpr bool is_zero(const M &m, S tolerance = detail::default_tolerance<S>()) noexcept
    {
        for (std::size_t i = 0; i < M::rows; ++i)
            for (std::size_t j = 0; j < M::cols; ++j)
                if (!approx_equal(m(i, j), S{}, tolerance))
                    return false;
        return true;
    }

} // namespace math
