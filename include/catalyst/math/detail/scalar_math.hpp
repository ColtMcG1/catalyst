#pragma once

#include "concepts.hpp"

#include <cmath>
#include <limits>
#include <numbers>
#include <type_traits>

// -------------------------------------------------------------------------
// Scalar kernels
// -------------------------------------------------------------------------
//
// The scalar arithmetic the rest of the library is written against: floored
// division, the rounding modes, sqrt, the trigonometric functions, and the
// default comparison tolerance. Each of the <cmath> wrappers has a
// constant-evaluation path, because <cmath> is not constexpr on every
// toolchain (P0533), and forwards to the standard function at run time.
// Everything here takes and returns a single scalar.

namespace math
{
    namespace detail
    {
        // -----------------------------------------------------------------
        // Rounding helpers (large_number.hpp, fraction.hpp)
        // -----------------------------------------------------------------

        template <integral_numeric I>
        struct divmod_result
        {
            I quotient;
            I remainder;
        };

        // Floored division: the quotient rounds toward negative infinity and
        // the remainder takes the sign of the divisor, so for b > 0 it lies
        // in [0, b). Plain `/` and `%` truncate toward zero, which is the
        // wrong convention for splitting a value into whole + fraction.
        // Preconditions: b != 0 and a / b representable (not min / -1).
        template <integral_numeric I>
        constexpr divmod_result<I> floor_divmod(I a, I b) noexcept
        {
            I q = static_cast<I>(a / b);
            I r = static_cast<I>(a % b);
            if constexpr (std::is_signed_v<I>)
            {
                if (r != 0 && ((r < 0) != (b < 0)))
                {
                    --q;
                    r = static_cast<I>(r + b);
                }
            }
            return {q, r};
        }

        // std::floor, usable in constant evaluation on toolchains whose
        // <cmath> is not constexpr yet (P0533). The compile-time path goes
        // through a 64-bit integer, which is exact: every |v| >= 2^64 is
        // already integral for the standard floating types, and anything
        // smaller fits the integer. NaN and infinities are returned as is.
        template <floating_numeric F>
        constexpr F floor_value(F v) noexcept
        {
            if (std::is_constant_evaluated())
            {
                constexpr F limit = static_cast<F>(0x1p64);
                if (!(v > -limit && v < limit))
                    return v;
                const bool negative = v < 0;
                const F magnitude = negative ? -v : v;
                const F truncated = static_cast<F>(static_cast<unsigned long long>(magnitude));
                if (!negative)
                    return truncated;
                return truncated == magnitude ? v : -truncated - F(1);
            }
            return std::floor(v);
        }

        // std::sqrt, usable in constant evaluation. The compile-time path
        // is Newton's iteration started from an upper bound of the root
        // (v itself for v >= 1, else 1) and stopped the first time an
        // iterate fails to decrease, so it lands exactly on the root of a
        // perfect square and within an ulp or two otherwise. Negative
        // values and NaN give NaN; zero and infinity return themselves.
        template <floating_numeric F>
        constexpr F sqrt_value(F v) noexcept
        {
            if (std::is_constant_evaluated())
            {
                if (v != v || v < 0)
                    return std::numeric_limits<F>::quiet_NaN();
                if (v == 0 || v == std::numeric_limits<F>::infinity())
                    return v;
                F x = v < 1 ? F(1) : v;
                for (;;)
                {
                    const F next = (x + v / x) / 2;
                    if (!(next < x))
                        return x;
                    x = next;
                }
            }
            return std::sqrt(v);
        }

        // The remaining rounding modes, derived from floor_value so they
        // share its constant-evaluation path. round() rounds halves away
        // from zero, as std::round does.
        template <floating_numeric F>
        constexpr F ceil_value(F v) noexcept
        {
            if (std::is_constant_evaluated())
                return -floor_value(-v);
            return std::ceil(v);
        }

        template <floating_numeric F>
        constexpr F trunc_value(F v) noexcept
        {
            if (std::is_constant_evaluated())
                return v < 0 ? -floor_value(-v) : floor_value(v);
            return std::trunc(v);
        }

        template <floating_numeric F>
        constexpr F round_value(F v) noexcept
        {
            if (std::is_constant_evaluated())
            {
                const F t = trunc_value(v);
                const F rest = v - t;
                if (rest >= F(0.5))
                    return t + 1;
                if (rest <= F(-0.5))
                    return t - 1;
                return t;
            }
            return std::round(v);
        }

        // |v|, for signed, unsigned and floating types alike. Written with
        // if constexpr so an unsigned argument does not trip "comparison is
        // always false" warnings.
        template <typename T>
        constexpr T abs_value(T v) noexcept
        {
            if constexpr (std::is_arithmetic_v<T> && std::is_unsigned_v<T>)
                return v;
            else
                return v < T{} ? -v : v;
        }

        // ------------------------------------------------------------------
        // Trigonometry (transform.hpp, quaternion.hpp)
        // ------------------------------------------------------------------
        //
        // std::sin, std::cos and std::tan are not constexpr on every
        // toolchain. The constant-evaluation path reduces the angle to
        // [-pi, pi] and sums the Taylor series until it stops changing,
        // which is accurate to an ulp or two at that range. Run-time calls
        // go to <cmath>. Angles are radians.

        template <floating_numeric F>
        constexpr F reduce_angle(F v) noexcept
        {
            constexpr F two_pi = F(2) * std::numbers::pi_v<F>;
            return v - two_pi * floor_value((v + std::numbers::pi_v<F>) / two_pi);
        }

        template <floating_numeric F>
        constexpr F sin_value(F v) noexcept
        {
            if (std::is_constant_evaluated())
            {
                if (!(v > -std::numeric_limits<F>::infinity() && v < std::numeric_limits<F>::infinity()))
                    return std::numeric_limits<F>::quiet_NaN();
                const F x = reduce_angle(v);
                const F x2 = x * x;
                F term = x;
                F sum = x;
                for (int n = 1; n < 64; ++n)
                {
                    term *= -x2 / (F(2 * n) * F(2 * n + 1));
                    const F next = sum + term;
                    if (next == sum)
                        break;
                    sum = next;
                }
                return sum;
            }
            return std::sin(v);
        }

        template <floating_numeric F>
        constexpr F cos_value(F v) noexcept
        {
            if (std::is_constant_evaluated())
            {
                if (!(v > -std::numeric_limits<F>::infinity() && v < std::numeric_limits<F>::infinity()))
                    return std::numeric_limits<F>::quiet_NaN();
                const F x = reduce_angle(v);
                const F x2 = x * x;
                F term = 1;
                F sum = 1;
                for (int n = 1; n < 64; ++n)
                {
                    term *= -x2 / (F(2 * n - 1) * F(2 * n));
                    const F next = sum + term;
                    if (next == sum)
                        break;
                    sum = next;
                }
                return sum;
            }
            return std::cos(v);
        }

        template <floating_numeric F>
        constexpr F tan_value(F v) noexcept
        {
            if (std::is_constant_evaluated())
                return sin_value(v) / cos_value(v);
            return std::tan(v);
        }

        // ------------------------------------------------------------------
        // Approximate comparison
        // ------------------------------------------------------------------

        // The absolute tolerance approx_equal uses when the caller gives
        // none: a hundred ulps at 1 for floating types, exact for integers.
        template <scalar_like T>
        constexpr T default_tolerance() noexcept
        {
            if constexpr (std::is_floating_point_v<T>)
                return std::numeric_limits<T>::epsilon() * T(100);
            else
                return T{};
        }
    } // namespace detail

} // namespace math
