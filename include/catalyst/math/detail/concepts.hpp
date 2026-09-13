#pragma once

#include <concepts>
#include <cstddef>
#include <type_traits>

// -------------------------------------------------------------------------
// Element concepts and traits
// -------------------------------------------------------------------------
//
// What a vector or matrix element is allowed to be, and the traits the
// element-wise machinery in every container header hangs off: the cv/ref
// stripping the *_like concepts are written in terms of, the element type of
// a container, the result element type of a binary operation, and the
// floating type a division-producing computation yields. Nothing here knows
// about any particular container; the container headers add their own
// partial specializations of detail::element on top.

namespace math
{

    // ---------------------------------------------------------------------
    // Concepts
    // ---------------------------------------------------------------------

    namespace detail
    {
        // bool is arithmetic in the language but is not a useful element:
        // true + true == 2, which then narrows back to true. The character
        // types are excluded too: vector<char, 3> would format as letters.
        // signed char and unsigned char stay, they are the 8-bit integers.
        template <typename T>
        concept excluded_arithmetic = std::is_same_v<T, bool> || std::is_same_v<T, char> || std::is_same_v<T, wchar_t> ||
                                      std::is_same_v<T, char8_t> || std::is_same_v<T, char16_t> || std::is_same_v<T, char32_t>;
    }

    // The builtin arithmetic types.
    template <typename T>
    concept numeric = std::is_arithmetic_v<T> && !detail::excluded_arithmetic<std::remove_cv_t<T>>;

    // Opt-in for user-defined element types. A type that specializes this
    // to true and supports the field operations below can be the element
    // of a vector or matrix, and every algorithm that needs only those
    // operations (dot, determinant, inverse, ...) works on it exactly.
    // fraction.hpp and large_number.hpp opt in.
    template <typename T>
    inline constexpr bool enable_scalar = false;

    // Anything usable as an element: a builtin number, or an opted-in type
    // with +, -, *, /, unary -, ==, < and construction from 0 and 1.
    template <typename T>
    concept scalar_like =
        numeric<std::remove_cv_t<T>> ||
        (enable_scalar<std::remove_cv_t<T>> && std::regular<std::remove_cv_t<T>> &&
         requires(const std::remove_cv_t<T> &a, const std::remove_cv_t<T> &b) {
             { a + b } -> std::convertible_to<std::remove_cv_t<T>>;
             { a - b } -> std::convertible_to<std::remove_cv_t<T>>;
             { a * b } -> std::convertible_to<std::remove_cv_t<T>>;
             { a / b } -> std::convertible_to<std::remove_cv_t<T>>;
             { -a } -> std::convertible_to<std::remove_cv_t<T>>;
             { a < b } -> std::convertible_to<bool>;
             std::remove_cv_t<T>{};
             std::remove_cv_t<T>{1};
         });

    // The two halves of numeric, for large_number.hpp and fraction.hpp, which
    // treat integer and floating-point operands differently.
    template <typename T>
    concept integral_numeric = numeric<T> && std::is_integral_v<T>;

    template <typename T>
    concept floating_numeric = std::is_floating_point_v<T>;
    namespace detail
    {
        // The *_like concepts are written against bare<X> so that they are
        // satisfied by cv/ref-qualified containers too, and can therefore
        // constrain forwarding references.
        template <typename X>
        using bare = std::remove_cvref_t<X>;

        // -----------------------------------------------------------------
        // Element type
        // -----------------------------------------------------------------

        // Element type of a container, or the scalar itself. The primary
        // template handles scalars; vector.hpp and matrix.hpp each add a
        // partial specialization for their own *_like concept.
        template <typename X>
        struct element
        {
            using type = bare<X>;
        };

        // Result element type of a binary operation: the common type of the
        // operands' elements, so int op float -> float, and short op short
        // stays short instead of promoting to int.
        template <typename L, typename R>
        using common_element_t = std::common_type_t<typename element<L>::type, typename element<R>::type>;

        // The type a division-producing computation on T naturally yields:
        // T itself when it is floating, double for the builtin integers
        // (what std::sqrt(int) returns), and T again for an opted-in
        // element type, which is its own field: the inverse of a
        // matrix<fraction<int>> is a matrix<fraction<int>>.
        template <scalar_like T>
        using floating_t =
            std::conditional_t<std::is_floating_point_v<T>, T, std::conditional_t<std::is_arithmetic_v<T>, double, T>>;

        // -----------------------------------------------------------------
        // Accumulators
        // -----------------------------------------------------------------

        // The type a reduction accumulates in: the caller's explicit choice,
        // or the container's own element type when none is given. This is
        // what lets dot<long long>(a, b) widen a reduction that would
        // otherwise overflow exactly as a hand-written loop does.
        template <typename Accumulator, typename Default>
        using accumulator_t = std::conditional_t<std::is_void_v<Accumulator>, Default, Accumulator>;
    } // namespace detail

} // namespace math
