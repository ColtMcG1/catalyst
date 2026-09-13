#pragma once

#include "detail/concepts.hpp"
#include "detail/hash.hpp"
#include "detail/scalar_math.hpp"

#include <compare>
#include <cstddef>
#include <format>
#include <functional>
#include <limits>
#include <numeric>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace math
{

    // ---------------------------------------------------------------------
    // fraction
    // ---------------------------------------------------------------------
    //
    // An exact rational number numerator / denominator.
    //
    // Invariants, established by every constructor and operation:
    //   - denominator > 0
    //   - gcd(numerator, denominator) == 1, so zero is stored as 0/1
    //
    // Because the stored form is canonical, member-wise equality is numeric
    // equality and the type hashes correctly. The members stay public so the
    // type remains a simple value; after writing them directly call reduce().
    //
    // Overflow: each operand is reduced against the other's denominator
    // before multiplying (Knuth, TAOCP 4.5.1) and comparison never
    // cross-multiplies, but a result whose reduced numerator or denominator
    // does not fit Integer overflows exactly as Integer would.

    template <typename Integer>
        requires integral_numeric<Integer>
    struct fraction;

    namespace detail
    {
        template <typename X>
        struct is_fraction : std::false_type
        {
        };

        template <typename Integer>
        struct is_fraction<fraction<Integer>> : std::true_type
        {
        };
    } // namespace detail

    template <typename X>
    concept fraction_like = detail::is_fraction<detail::bare<X>>::value;

    // A fraction is a field, so it can be the element of a vector or
    // matrix: matrix<fraction<int>, 3, 3> inverts exactly.
    template <typename Integer>
    inline constexpr bool enable_scalar<fraction<Integer>> = true;

    template <typename Integer>
        requires integral_numeric<Integer>
    struct fraction
    {
        using value_type = Integer;

        Integer numerator{};
        Integer denominator{1};

        // -----------------------------------------------------------------
        // Construction
        // -----------------------------------------------------------------

        constexpr fraction() = default;

        // Implicit, so that `f + 2` and `2 < f` work. Only integers convert;
        // a floating-point literal is rejected rather than truncated.
        // Throws std::overflow_error when the value does not fit Integer.
        template <integral_numeric I>
        constexpr fraction(I value) : numerator(static_cast<Integer>(value))
        {
            if (!std::in_range<Integer>(value))
                throw std::overflow_error("fraction: integer does not fit");
        }

        // Throws std::domain_error on a zero denominator.
        constexpr fraction(Integer num, Integer den) : numerator(num), denominator(den) { reduce(); }

        // The simplest fraction that reproduces `value` to the precision of
        // F, found as the last continued-fraction convergent whose
        // denominator does not exceed max_denominator. So from_floating(0.1)
        // is 1/10, not the exact binary expansion 3602879701896397/2^55, and
        // from_floating(3.14159, 1000) is 355/113. A convergent is always a
        // good approximation but not always the closest one under the cap.
        // Throws std::invalid_argument for NaN or infinity and
        // std::overflow_error when the integer part does not fit.
        template <floating_numeric F>
        static constexpr fraction from_floating(F value, Integer max_denominator = std::numeric_limits<Integer>::max())
        {
            if (!(value == value) || value > std::numeric_limits<F>::max() || value < -std::numeric_limits<F>::max())
                throw std::invalid_argument("fraction: cannot represent a non-finite value");
            if (max_denominator < 1)
                throw std::invalid_argument("fraction: max_denominator must be positive");

            constexpr Integer max = std::numeric_limits<Integer>::max();
            constexpr F max_as_floating = static_cast<F>(max);

            const bool negative = value < 0;
            const F x0 = negative ? -value : value;
            F x = x0;

            // Convergents h/k with h_{-2} = 0, h_{-1} = 1, k_{-2} = 1, k_{-1} = 0.
            Integer h0 = 0, h1 = 1;
            Integer k0 = 1, k1 = 0;
            for (int term = 0; term < 128; ++term)
            {
                const F integral_part = detail::floor_value(x);
                if (integral_part >= max_as_floating)
                {
                    if (term == 0)
                        throw std::overflow_error("fraction: value does not fit");
                    break;
                }
                const Integer a = static_cast<Integer>(integral_part);

                // Next convergent is a * h1 + h0 over a * k1 + k0. Stop before
                // the denominator passes the cap or either side overflows.
                if (k1 != 0 && a > (max_denominator - k0) / k1)
                    break;
                if (h1 != 0 && a > (max - h0) / h1)
                    break;
                const Integer h2 = static_cast<Integer>(a * h1 + h0);
                const Integer k2 = static_cast<Integer>(a * k1 + k0);
                h0 = h1;
                h1 = h2;
                k0 = k1;
                k1 = k2;

                // Stop once the convergent reproduces the value, or when the
                // remainder is below the rounding noise of x (each reciprocal
                // step carries a relative error of about epsilon), which would
                // otherwise turn 0.75 into 3/4 + noise instead of 3/4.
                if (static_cast<F>(h1) / static_cast<F>(k1) == x0)
                    break;
                const F rest = x - integral_part; // exact
                if (rest <= x * std::numeric_limits<F>::epsilon())
                    break;
                x = F(1) / rest;
            }

            if constexpr (std::is_signed_v<Integer>)
                return fraction(negative ? static_cast<Integer>(-h1) : h1, k1);
            else
                return fraction(h1, k1);
        }

        // -----------------------------------------------------------------
        // Invariant
        // -----------------------------------------------------------------

        // Restores the canonical form after the members were written by hand.
        // Throws std::domain_error on a zero denominator.
        constexpr void reduce()
        {
            if (denominator == 0)
                throw std::domain_error("fraction: zero denominator");
            if constexpr (std::is_signed_v<Integer>)
            {
                if (denominator < 0)
                {
                    numerator = static_cast<Integer>(-numerator);
                    denominator = static_cast<Integer>(-denominator);
                }
            }
            const Integer g = static_cast<Integer>(std::gcd(numerator, denominator)); // gcd(0, d) == d
            numerator /= g;
            denominator /= g;
        }

        constexpr bool is_reduced() const noexcept
        {
            return denominator > 0 && std::gcd(numerator, denominator) == 1;
        }

        // -----------------------------------------------------------------
        // Observers
        // -----------------------------------------------------------------

        constexpr bool is_integer() const noexcept { return denominator == 1; }

        constexpr bool is_negative() const noexcept
        {
            if constexpr (std::is_signed_v<Integer>)
                return numerator < 0;
            else
                return false;
        }

        constexpr Integer floor() const noexcept { return detail::floor_divmod(numerator, denominator).quotient; }

        constexpr Integer ceil() const noexcept
        {
            const auto [quotient, remainder] = detail::floor_divmod(numerator, denominator);
            return remainder == 0 ? quotient : static_cast<Integer>(quotient + 1);
        }

        constexpr Integer trunc() const noexcept { return static_cast<Integer>(numerator / denominator); }

        // Half away from zero, like std::round.
        constexpr Integer round() const noexcept
        {
            const auto [quotient, remainder] = detail::floor_divmod(numerator, denominator);
            // remainder / denominator is the distance down to `quotient`;
            // (denominator - remainder) / denominator the distance up.
            const Integer up = static_cast<Integer>(denominator - remainder);
            if (remainder > up)
                return static_cast<Integer>(quotient + 1);
            if (remainder < up)
                return quotient;
            return is_negative() ? quotient : static_cast<Integer>(quotient + 1);
        }

        template <floating_numeric F = double>
        constexpr F to_floating() const noexcept
        {
            return static_cast<F>(numerator) / static_cast<F>(denominator);
        }

        explicit constexpr operator double() const noexcept { return to_floating(); }

        // -----------------------------------------------------------------
        // Sign, reciprocal, powers
        // -----------------------------------------------------------------

        // The sign lives in the numerator, so negation keeps the form reduced.
        constexpr fraction operator-() const noexcept
            requires std::is_signed_v<Integer>
        {
            fraction r = *this;
            r.numerator = static_cast<Integer>(-r.numerator);
            return r;
        }

        constexpr fraction abs() const noexcept
        {
            if constexpr (std::is_signed_v<Integer>)
                return is_negative() ? -*this : *this;
            else
                return *this;
        }

        // Throws std::domain_error for zero.
        constexpr fraction reciprocal() const
        {
            if (numerator == 0)
                throw std::domain_error("fraction: reciprocal of zero");
            return fraction(denominator, numerator); // the constructor fixes the sign
        }

        // Integer power by repeated squaring; a negative exponent inverts
        // first (so zero to a negative power throws std::domain_error).
        constexpr fraction pow(int exponent) const
        {
            fraction base = exponent < 0 ? reciprocal() : *this;
            unsigned remaining = exponent < 0 ? 0u - static_cast<unsigned>(exponent) : static_cast<unsigned>(exponent);
            fraction result = 1;
            while (remaining != 0)
            {
                if (remaining & 1u)
                    result *= base;
                remaining >>= 1;
                if (remaining != 0)
                    base *= base;
            }
            return result;
        }

        // -----------------------------------------------------------------
        // Arithmetic
        // -----------------------------------------------------------------

        // a/b + c/d == (a (d/g) + c (b/g)) / ((b/g) d) with g = gcd(b, d):
        // the smallest intermediate the sum can be written with.
        constexpr fraction &operator+=(const fraction &other)
        {
            const Integer g = static_cast<Integer>(std::gcd(denominator, other.denominator));
            const Integer num = static_cast<Integer>(numerator * (other.denominator / g) + other.numerator * (denominator / g));
            const Integer den = static_cast<Integer>((denominator / g) * other.denominator);
            return *this = fraction(num, den);
        }

        constexpr fraction &operator-=(const fraction &other)
        {
            const Integer g = static_cast<Integer>(std::gcd(denominator, other.denominator));
            const Integer num = static_cast<Integer>(numerator * (other.denominator / g) - other.numerator * (denominator / g));
            const Integer den = static_cast<Integer>((denominator / g) * other.denominator);
            return *this = fraction(num, den);
        }

        // a/b * c/d == (a/g1)(c/g2) / ((b/g2)(d/g1)) with g1 = gcd(a, d) and
        // g2 = gcd(c, b): cross-cancel first, so the products stay small.
        constexpr fraction &operator*=(const fraction &other)
        {
            const Integer g1 = static_cast<Integer>(std::gcd(numerator, other.denominator));
            const Integer g2 = static_cast<Integer>(std::gcd(other.numerator, denominator));
            const Integer num = static_cast<Integer>((numerator / g1) * (other.numerator / g2));
            const Integer den = static_cast<Integer>((denominator / g2) * (other.denominator / g1));
            return *this = fraction(num, den);
        }

        // Throws std::domain_error on a zero divisor.
        constexpr fraction &operator/=(const fraction &other)
        {
            if (other.numerator == 0)
                throw std::domain_error("fraction: division by zero");
            return *this *= other.reciprocal();
        }

        // Integer operands convert through the implicit constructor.
        friend constexpr fraction operator+(fraction lhs, const fraction &rhs) { return lhs += rhs; }
        friend constexpr fraction operator-(fraction lhs, const fraction &rhs) { return lhs -= rhs; }
        friend constexpr fraction operator*(fraction lhs, const fraction &rhs) { return lhs *= rhs; }
        friend constexpr fraction operator/(fraction lhs, const fraction &rhs) { return lhs /= rhs; }

        // -----------------------------------------------------------------
        // Comparison
        // -----------------------------------------------------------------

        // Canonical form makes member-wise equality numeric equality.
        friend constexpr bool operator==(const fraction &, const fraction &) noexcept = default;

        // Compares by continued fraction instead of cross-multiplying, so it
        // cannot overflow: compare the integer parts, and if they agree
        // compare the reciprocals of the remainders, swapped. Each round
        // divides by a smaller denominator, so the loop ends like Euclid's.
        friend constexpr std::strong_ordering operator<=>(const fraction &lhs, const fraction &rhs) noexcept
        {
            Integer a = lhs.numerator, b = lhs.denominator;
            Integer c = rhs.numerator, d = rhs.denominator;
            while (true)
            {
                const auto [q1, r1] = detail::floor_divmod(a, b);
                const auto [q2, r2] = detail::floor_divmod(c, d);
                if (q1 != q2)
                    return q1 <=> q2;
                if (r1 == 0 && r2 == 0)
                    return std::strong_ordering::equal;
                if (r1 == 0)
                    return std::strong_ordering::less;
                if (r2 == 0)
                    return std::strong_ordering::greater;
                // r1/b <=> r2/d  ==  d/r2 <=> b/r1
                const Integer old_b = b;
                a = d;
                b = r2;
                c = old_b;
                d = r1;
            }
        }
    };

} // namespace math

// -------------------------------------------------------------------------
// std integration: hashing, std::format
// -------------------------------------------------------------------------

namespace std
{
    template <typename Integer>
    struct hash<math::fraction<Integer>>
    {
        std::size_t operator()(const math::fraction<Integer> &f) const noexcept
        {
            std::size_t seed = 0;
            math::detail::hash_combine(seed, f.numerator);
            math::detail::hash_combine(seed, f.denominator);
            return seed;
        }
    };

    // Formats as "3/4"; an integer value prints without the denominator.
    template <typename Integer, typename CharT>
    struct formatter<math::fraction<Integer>, CharT>
    {
        constexpr auto parse(basic_format_parse_context<CharT> &ctx)
        {
            auto it = ctx.begin();
            if (it != ctx.end() && *it != CharT('}'))
                throw format_error("fraction: unsupported format specifier");
            return it;
        }

        template <typename FormatContext>
        auto format(const math::fraction<Integer> &f, FormatContext &ctx) const
        {
            auto out = ctx.out();
            ctx.advance_to(out);
            out = formatter<Integer, CharT>{}.format(f.numerator, ctx);
            if (f.denominator != 1)
            {
                *out++ = CharT('/');
                ctx.advance_to(out);
                out = formatter<Integer, CharT>{}.format(f.denominator, ctx);
            }
            return out;
        }
    };
} // namespace std
