#pragma once

#include "detail/concepts.hpp"
#include "detail/scalar_math.hpp"

#include <numbers>
#include <type_traits>

namespace math
{

    // ---------------------------------------------------------------------
    // Scalars
    // ---------------------------------------------------------------------
    //
    // The scalar-level public API the container headers build on: the
    // approximate comparison every element-wise approx_equal reduces to, and
    // the angle conversions. The constexpr <cmath> replacements these are
    // written against are in detail/scalar_math.hpp.

    // ---- approximate comparison ------------------------------------------

    // |a - b| <= tolerance, with equal values (including equal infinities)
    // always approximately equal. The tolerance is absolute; callers
    // comparing values far from 1 should scale it.
    template <scalar_like L, scalar_like R, scalar_like S = std::common_type_t<L, R>>
    constexpr bool approx_equal(L a, R b, S tolerance = detail::default_tolerance<S>()) noexcept
    {
        using T = std::common_type_t<L, R, S>;
        const T x = static_cast<T>(a);
        const T y = static_cast<T>(b);
        if (x == y)
            return true;
        const T diff = x > y ? x - y : y - x;
        return diff <= static_cast<T>(tolerance);
    }

    // ---- angles ----------------------------------------------------------
    //
    // Angles are radians everywhere in this library; these convert at the
    // boundary.

    template <floating_numeric F>
    constexpr F radians(F degrees) noexcept
    {
        return degrees * (std::numbers::pi_v<F> / F(180));
    }

    template <floating_numeric F>
    constexpr F degrees(F radians) noexcept
    {
        return radians * (F(180) / std::numbers::pi_v<F>);
    }

} // namespace math
