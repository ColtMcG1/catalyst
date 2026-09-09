#pragma once

#include "../matrix.hpp"
#include "concepts.hpp"
#include "scalar_math.hpp"

#include <cstddef>

// -------------------------------------------------------------------------
// Gaussian elimination
// -------------------------------------------------------------------------
//
// The elimination kernels linear_algebra.hpp drives: row swapping, partial
// pivot selection, Gauss-Jordan reduction of [a | b], and the two
// determinant strategies (partial-pivot elimination for floating elements,
// Bareiss fraction-free elimination for exact ones). They all take and
// return owning matrices, and none of them throws: gauss_jordan reports a
// singular input by returning false.

namespace math
{
    namespace detail
    {
        // Swaps rows i and j of an owning matrix in place.
        template <scalar_like T, std::size_t R, std::size_t C, matrix_order O>
        constexpr void swap_rows(matrix<T, R, C, O> &m, std::size_t i, std::size_t j) noexcept
        {
            if (i == j)
                return;
            for (std::size_t k = 0; k < C; ++k)
            {
                const T tmp = m(i, k);
                m(i, k) = m(j, k);
                m(j, k) = tmp;
            }
        }

        // Row index in [from, N) with the largest |m(row, col)|.
        template <scalar_like T, std::size_t R, std::size_t C, matrix_order O>
        constexpr std::size_t pivot_row(const matrix<T, R, C, O> &m, std::size_t col, std::size_t from) noexcept
        {
            std::size_t best = from;
            for (std::size_t r = from + 1; r < R; ++r)
                if (abs_value(m(r, col)) > abs_value(m(best, col)))
                    best = r;
            return best;
        }

        // Reduces the augmented system [a | b] to [I | a^-1 b] in place.
        // Returns false, leaving both operands partially reduced, when a
        // zero pivot shows a to be singular.
        template <scalar_like T, std::size_t N, std::size_t K, matrix_order OA, matrix_order OB>
        constexpr bool gauss_jordan(matrix<T, N, N, OA> &a, matrix<T, N, K, OB> &b) noexcept
        {
            for (std::size_t k = 0; k < N; ++k)
            {
                const std::size_t p = pivot_row(a, k, k);
                if (a(p, k) == T{})
                    return false;
                swap_rows(a, k, p);
                swap_rows(b, k, p);

                const T inv = T(1) / a(k, k);
                for (std::size_t j = 0; j < N; ++j)
                    a(k, j) = a(k, j) * inv;
                for (std::size_t j = 0; j < K; ++j)
                    b(k, j) = b(k, j) * inv;

                for (std::size_t i = 0; i < N; ++i)
                {
                    if (i == k)
                        continue;
                    const T f = a(i, k);
                    if (f == T{})
                        continue;
                    for (std::size_t j = 0; j < N; ++j)
                        a(i, j) = a(i, j) - f * a(k, j);
                    for (std::size_t j = 0; j < K; ++j)
                        b(i, j) = b(i, j) - f * b(k, j);
                }
            }
            return true;
        }

        // Determinant by partial-pivot elimination; for floating elements.
        template <scalar_like T, std::size_t N, matrix_order O>
        constexpr T determinant_floating(matrix<T, N, N, O> m) noexcept
        {
            T result{1};
            for (std::size_t k = 0; k < N; ++k)
            {
                const std::size_t p = pivot_row(m, k, k);
                if (m(p, k) == T{})
                    return T{};
                if (p != k)
                {
                    swap_rows(m, k, p);
                    result = -result;
                }
                result = result * m(k, k);
                for (std::size_t i = k + 1; i < N; ++i)
                {
                    const T f = m(i, k) / m(k, k);
                    for (std::size_t j = k; j < N; ++j)
                        m(i, j) = m(i, j) - f * m(k, j);
                }
            }
            return result;
        }

        // Determinant by Bareiss elimination; exact for integer elements.
        // Each step divides by the previous pivot, and that division is
        // exact because the entries are determinants of leading minors.
        template <scalar_like T, std::size_t N, matrix_order O>
        constexpr T determinant_bareiss(matrix<T, N, N, O> m) noexcept
        {
            T sign{1};
            T previous{1};
            for (std::size_t k = 0; k + 1 < N; ++k)
            {
                if (m(k, k) == T{})
                {
                    std::size_t p = k + 1;
                    while (p < N && m(p, k) == T{})
                        ++p;
                    if (p == N)
                        return T{};
                    swap_rows(m, k, p);
                    sign = static_cast<T>(-sign);
                }
                for (std::size_t i = k + 1; i < N; ++i)
                    for (std::size_t j = k + 1; j < N; ++j)
                        m(i, j) = static_cast<T>((m(i, j) * m(k, k) - m(i, k) * m(k, j)) / previous);
                previous = m(k, k);
            }
            return static_cast<T>(sign * m(N - 1, N - 1));
        }
    } // namespace detail

} // namespace math
