#pragma once

#include "detail/concepts.hpp"
#include "detail/elimination.hpp"
#include "detail/scalar_math.hpp"
#include "matrix.hpp"
#include "scalar.hpp"
#include "vector.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>

namespace math
{

    // ---------------------------------------------------------------------
    // Linear algebra
    // ---------------------------------------------------------------------
    //
    // The algorithms on matrices, as opposed to the operators that belong to
    // the type itself (matrix.hpp). They are written against matrix_like, so
    // they accept views as well as owning matrices, and they return owning
    // matrices in the source's storage order.

    // ---- rearrangement ---------------------------------------------------

    template <matrix_like M>
    constexpr auto transpose(const M &m)
    {
        return detail::generate<typename M::value_type, M::cols, M::rows, detail::order_of<M>()>(
            [&](std::size_t i, std::size_t j) { return m(j, i); });
    }

    template <typename Accumulator = void, matrix_like M>
        requires(M::rows == M::cols)
    constexpr auto trace(const M &m)
    {
        using T = detail::accumulator_t<Accumulator, typename M::value_type>;
        T total{};
        for (std::size_t i = 0; i < M::rows; ++i)
            total = static_cast<T>(total + m(i, i));
        return total;
    }

    // Outer product u v^T: (i, j) = u[i] * v[j], an L::length x R::length
    // row-major matrix.
    template <vector_like L, vector_like R>
    constexpr auto outer(const L &u, const R &v)
    {
        using T = detail::common_element_t<L, R>;
        return detail::generate<T, L::length, R::length>([&](std::size_t i, std::size_t j) { return u[i] * v[j]; });
    }

    // ---- blocks ----------------------------------------------------------

    // The R x C block whose top-left element is (R0, C0), in the source's
    // storage order.
    template <std::size_t R0, std::size_t C0, std::size_t R, std::size_t C, matrix_like M>
        requires(R > 0 && C > 0 && R0 + R <= M::rows && C0 + C <= M::cols)
    constexpr auto submatrix(const M &m)
    {
        return detail::generate<typename M::value_type, R, C, detail::order_of<M>()>(
            [&](std::size_t i, std::size_t j) { return m(R0 + i, C0 + j); });
    }

    // m with row i and column j removed: the matrix whose determinant is
    // the (i, j) minor. Named minor_matrix because some C libraries define
    // minor() as a macro.
    template <matrix_like M>
        requires(M::rows > 1 && M::cols > 1)
    constexpr auto minor_matrix(const M &m, std::size_t i, std::size_t j)
    {
        return detail::generate<typename M::value_type, M::rows - 1, M::cols - 1, detail::order_of<M>()>(
            [&](std::size_t r, std::size_t c) { return m(r < i ? r : r + 1, c < j ? c : c + 1); });
    }

    // ---------------------------------------------------------------------
    // Linear algebra on square matrices
    // ---------------------------------------------------------------------
    //
    // determinant keeps the element type: closed forms up to 3 x 3, then
    // Bareiss fraction-free elimination for integer elements (exact, every
    // division is exact by construction) and partial-pivot Gaussian
    // elimination for floating ones. Integer intermediates can overflow as
    // the element type would; use a wider element type for large entries.
    //
    // inverse and solve always produce a floating result (double for
    // integer inputs, see detail::floating_t) through Gauss-Jordan
    // elimination with partial pivoting. A matrix is treated as singular
    // when a pivot is exactly zero; a nearly singular one is not detected
    // and yields large, inaccurate numbers, which approx_equal against the
    // identity will reveal. The throwing forms raise singular_matrix; the
    // try_ forms return an empty optional instead, and are the ones to use
    // in constant evaluation when the input may be singular.

    struct singular_matrix : std::domain_error
    {
        singular_matrix() : std::domain_error("math: matrix is singular") {}
    };

    template <matrix_like M>
        requires(M::rows == M::cols)
    constexpr auto determinant(const M &m)
    {
        using T = typename M::value_type;
        constexpr std::size_t N = M::rows;

        if constexpr (N == 1)
            return static_cast<T>(m(0, 0));
        else if constexpr (N == 2)
            return static_cast<T>(m(0, 0) * m(1, 1) - m(0, 1) * m(1, 0));
        else if constexpr (N == 3)
            return static_cast<T>(m(0, 0) * (m(1, 1) * m(2, 2) - m(1, 2) * m(2, 1)) -
                                  m(0, 1) * (m(1, 0) * m(2, 2) - m(1, 2) * m(2, 0)) +
                                  m(0, 2) * (m(1, 0) * m(2, 1) - m(1, 1) * m(2, 0)));
        else
        {
            matrix<T, N, N, detail::order_of<M>()> copy{};
            for (std::size_t i = 0; i < N; ++i)
                for (std::size_t j = 0; j < N; ++j)
                    copy(i, j) = m(i, j);
            if constexpr (std::is_floating_point_v<T>)
                return detail::determinant_floating(copy);
            else
                return detail::determinant_bareiss(copy);
        }
    }

    // ---- inverse ---------------------------------------------------------

    template <matrix_like M>
        requires(M::rows == M::cols)
    using inverse_t = matrix<detail::floating_t<typename M::value_type>, M::rows, M::cols, detail::order_of<M>()>;

    template <matrix_like M>
        requires(M::rows == M::cols)
    constexpr std::optional<inverse_t<M>> try_inverse(const M &m) noexcept
    {
        using R = inverse_t<M>;
        using F = typename R::value_type;
        R a{};
        for (std::size_t i = 0; i < M::rows; ++i)
            for (std::size_t j = 0; j < M::cols; ++j)
                a(i, j) = static_cast<F>(m(i, j));
        R b = R::identity();
        if (!detail::gauss_jordan(a, b))
            return std::nullopt;
        return b;
    }

    template <matrix_like M>
        requires(M::rows == M::cols)
    constexpr inverse_t<M> inverse(const M &m)
    {
        if (auto result = try_inverse(m))
            return *result;
        throw singular_matrix{};
    }

    // ---- solve -----------------------------------------------------------
    //
    // Solves a x = b for x. b may be a vector (one right-hand side) or an
    // N x K matrix (K right-hand sides at once, x is N x K in b's order).

    template <matrix_like A, matrix_like B>
        requires(A::rows == A::cols && B::rows == A::rows)
    using solution_t = matrix<detail::floating_t<detail::common_element_t<A, B>>, B::rows, B::cols, detail::order_of<B>()>;

    template <matrix_like A, vector_like B>
        requires(A::rows == A::cols && B::length == A::rows)
    using vector_solution_t = vector<detail::floating_t<detail::common_element_t<A, B>>, B::length>;

    template <matrix_like A, matrix_like B>
        requires(A::rows == A::cols && B::rows == A::rows)
    constexpr std::optional<solution_t<A, B>> try_solve(const A &a, const B &b) noexcept
    {
        using X = solution_t<A, B>;
        using F = typename X::value_type;
        matrix<F, A::rows, A::cols, detail::order_of<A>()> lhs{};
        for (std::size_t i = 0; i < A::rows; ++i)
            for (std::size_t j = 0; j < A::cols; ++j)
                lhs(i, j) = static_cast<F>(a(i, j));
        X rhs{};
        for (std::size_t i = 0; i < B::rows; ++i)
            for (std::size_t j = 0; j < B::cols; ++j)
                rhs(i, j) = static_cast<F>(b(i, j));
        if (!detail::gauss_jordan(lhs, rhs))
            return std::nullopt;
        return rhs;
    }

    template <matrix_like A, vector_like B>
        requires(A::rows == A::cols && B::length == A::rows)
    constexpr std::optional<vector_solution_t<A, B>> try_solve(const A &a, const B &b) noexcept
    {
        using F = typename vector_solution_t<A, B>::value_type;
        matrix<F, B::length, 1> column{};
        for (std::size_t i = 0; i < B::length; ++i)
            column(i, 0) = static_cast<F>(b[i]);
        auto result = try_solve(a, column);
        if (!result)
            return std::nullopt;
        return result->column(0);
    }

    template <matrix_like A, matrix_like B>
        requires(A::rows == A::cols && B::rows == A::rows)
    constexpr solution_t<A, B> solve(const A &a, const B &b)
    {
        if (auto result = try_solve(a, b))
            return *result;
        throw singular_matrix{};
    }

    template <matrix_like A, vector_like B>
        requires(A::rows == A::cols && B::length == A::rows)
    constexpr vector_solution_t<A, B> solve(const A &a, const B &b)
    {
        if (auto result = try_solve(a, b))
            return *result;
        throw singular_matrix{};
    }

    // ---- predicates ------------------------------------------------------

    template <matrix_like M, scalar_like S = typename M::value_type>
    constexpr bool is_square(const M &) noexcept
    {
        return M::rows == M::cols;
    }

    template <matrix_like M, scalar_like S = typename M::value_type>
        requires(M::rows == M::cols)
    constexpr bool is_symmetric(const M &m, S tolerance = detail::default_tolerance<S>()) noexcept
    {
        for (std::size_t i = 0; i < M::rows; ++i)
            for (std::size_t j = i + 1; j < M::cols; ++j)
                if (!approx_equal(m(i, j), m(j, i), tolerance))
                    return false;
        return true;
    }

    template <matrix_like M, scalar_like S = typename M::value_type>
    constexpr bool is_diagonal(const M &m, S tolerance = detail::default_tolerance<S>()) noexcept
    {
        for (std::size_t i = 0; i < M::rows; ++i)
            for (std::size_t j = 0; j < M::cols; ++j)
                if (i != j && !approx_equal(m(i, j), S{}, tolerance))
                    return false;
        return true;
    }

    template <matrix_like M, scalar_like S = typename M::value_type>
        requires(M::rows == M::cols)
    constexpr bool is_identity(const M &m, S tolerance = detail::default_tolerance<S>()) noexcept
    {
        for (std::size_t i = 0; i < M::rows; ++i)
            for (std::size_t j = 0; j < M::cols; ++j)
                if (!approx_equal(m(i, j), i == j ? S{1} : S{}, tolerance))
                    return false;
        return true;
    }

    // m^T m == I: the columns are orthonormal, so the inverse is the
    // transpose. Rotation matrices satisfy this.
    template <matrix_like M, scalar_like S = typename M::value_type>
        requires(M::rows == M::cols)
    constexpr bool is_orthogonal(const M &m, S tolerance = detail::default_tolerance<S>()) noexcept
    {
        return is_identity(transpose(m) * m, tolerance);
    }

} // namespace math
