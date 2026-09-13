#pragma once

#include "detail/concepts.hpp"
#include "detail/config.hpp"
#include "detail/hash.hpp"
#include "detail/matrix_iterator.hpp"
#include "vector.hpp"
#include "vector_view.hpp"

#include <cstddef>
#include <format>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <type_traits>
#include <utility>

// -------------------------------------------------------------------------
// matrix
// -------------------------------------------------------------------------
//
// The fixed-size matrix type in either storage order, its concept, and the
// operators that belong to it: element-wise arithmetic, the products,
// equality and the std integration. The algorithms *on* matrices --
// transpose, trace, determinant, inverse, solve -- live in
// linear_algebra.hpp, and the per-element scalar functions in
// elementwise.hpp.

namespace math
{

    enum class matrix_order
    {
        row_major,
        column_major
    };

    // ---------------------------------------------------------------------
    // Concepts
    // ---------------------------------------------------------------------

    // Anything with a numeric value_type, compile-time rows and cols, and
    // const (row, column) element access. Storage order is deliberately not
    // part of the concept: generic code goes through m(i, j) and never
    // needs to know how the elements are laid out. Satisfied by
    // cv/ref-qualified matrices too, so it can constrain forwarding
    // references.
    template <typename M>
    concept matrix_like =
        requires {
            typename detail::bare<M>::value_type;
            { detail::bare<M>::rows } -> std::convertible_to<std::size_t>;
            { detail::bare<M>::cols } -> std::convertible_to<std::size_t>;
            requires(detail::bare<M>::rows > 0 && detail::bare<M>::cols > 0);
        } &&
        requires(const detail::bare<M> &m, std::size_t i, std::size_t j) {
            { m(i, j) } -> std::convertible_to<typename detail::bare<M>::value_type>;
        } &&
        scalar_like<typename detail::bare<M>::value_type>;

    namespace detail
    {
        // The only thing that differs between the two orders is which axis
        // the stored lines run along. Specializing this trait, rather than
        // matrix itself, keeps every other member in one place: a partial
        // specialization of matrix would be a brand-new class that shares
        // nothing with the primary template.
        template <scalar_like T, std::size_t Rows, std::size_t Cols, matrix_order Order>
        struct matrix_storage;

        template <scalar_like T, std::size_t Rows, std::size_t Cols>
        struct matrix_storage<T, Rows, Cols, matrix_order::row_major>
        {
            using line_type = T[Cols];
            static constexpr std::size_t line_count = Rows;
        };

        template <scalar_like T, std::size_t Rows, std::size_t Cols>
        struct matrix_storage<T, Rows, Cols, matrix_order::column_major>
        {
            using line_type = T[Rows];
            static constexpr std::size_t line_count = Cols;
        };

        // Storage order of a matrix_like, or row_major when it does not
        // advertise one. Results of binary operations take the left
        // operand's order.
        template <matrix_like M>
        constexpr matrix_order order_of() noexcept
        {
            if constexpr (requires { { bare<M>::order } -> std::convertible_to<matrix_order>; })
                return bare<M>::order;
            else
                return matrix_order::row_major;
        }
    } // namespace detail

    // ---------------------------------------------------------------------
    // matrix
    // ---------------------------------------------------------------------

    template <scalar_like T, std::size_t Rows, std::size_t Cols, matrix_order Order = matrix_order::row_major>
        requires(Rows > 0 && Cols > 0)
    struct matrix
    {
        static constexpr matrix_order order = Order;
        static constexpr std::size_t rows = Rows;
        static constexpr std::size_t cols = Cols;
        static constexpr std::size_t size = Rows * Cols;

        using storage = detail::matrix_storage<T, Rows, Cols, Order>;

        // A "line" is a row for row-major storage and a column for
        // column-major storage. line_type is the raw T[N] a line is stored
        // as; m[i] hands back a vector_view over the i-th one.
        using line_type = typename storage::line_type;
        static constexpr std::size_t line_count = storage::line_count;
        static constexpr std::size_t line_length = std::extent_v<line_type>;

        using value_type = T;
        using order_type = matrix_order;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using pointer = T *;
        using const_pointer = const T *;

        using line_view = vector_view<T, line_length>;
        using const_line_view = vector_view<const T, line_length>;
        using line_value = vector<T, line_length>;

        // Rows and columns as values, independent of the storage order.
        // One of them is the same type as line_value.
        using row_type = vector<T, Cols>;
        using column_type = vector<T, Rows>;
        static constexpr std::size_t diagonal_length = Rows < Cols ? Rows : Cols;
        using diagonal_type = vector<T, diagonal_length>;

        using iterator = matrix_iterator<T, line_length>;
        using const_iterator = matrix_iterator<const T, line_length>;
        using reverse_iterator = std::reverse_iterator<iterator>;
        using const_reverse_iterator = std::reverse_iterator<const_iterator>;

        // The only data member. Same reasoning as vector::values: the type
        // stays an aggregate, trivially copyable and standard-layout, and
        // sizeof(matrix<T, R, C>) == R * C * sizeof(T).
        line_type elements[line_count]{};

        // ---- factories ---------------------------------------------------

        static constexpr matrix zero() noexcept { return matrix{}; }

        static constexpr matrix filled(T value) noexcept
        {
            matrix result{};
            for (auto &line : result.elements)
                for (auto &e : line)
                    e = value;
            return result;
        }

        static constexpr matrix identity() noexcept
            requires(Rows == Cols)
        {
            matrix result{};
            for (size_type i = 0; i < Rows; ++i)
                result(i, i) = T{1};
            return result;
        }

        // Whatever the storage order, these take rows as rows and columns
        // as columns, so a matrix can be written the way it is printed:
        //
        //   matrix<int, 2, 3>::from_rows({{1, 2, 3}, {4, 5, 6}});
        //   matrix<int, 2, 3>::from_rows(vector{1, 2, 3}, vector{4, 5, 6});
        //
        // The array overload is what a braced list binds to; the pack
        // overload takes any vector_like values, including views.

        static constexpr matrix from_rows(const row_type (&lines)[Rows]) noexcept
        {
            matrix result{};
            for (size_type i = 0; i < Rows; ++i)
                for (size_type j = 0; j < Cols; ++j)
                    result(i, j) = lines[i][j];
            return result;
        }

        template <vector_like... V>
            requires(sizeof...(V) == Rows && ((V::length == Cols) && ...))
        static constexpr matrix from_rows(const V &...lines) noexcept
        {
            matrix result{};
            size_type i = 0;
            ((result.set_row(i++, lines)), ...);
            return result;
        }

        static constexpr matrix from_columns(const column_type (&lines)[Cols]) noexcept
        {
            matrix result{};
            for (size_type j = 0; j < Cols; ++j)
                for (size_type i = 0; i < Rows; ++i)
                    result(i, j) = lines[j][i];
            return result;
        }

        template <vector_like... V>
            requires(sizeof...(V) == Cols && ((V::length == Rows) && ...))
        static constexpr matrix from_columns(const V &...lines) noexcept
        {
            matrix result{};
            size_type j = 0;
            ((result.set_column(j++, lines)), ...);
            return result;
        }

        // Zero everywhere except the main diagonal.
        template <vector_like V>
            requires(V::length == diagonal_length)
        static constexpr matrix from_diagonal(const V &values) noexcept
        {
            matrix result{};
            for (size_type i = 0; i < diagonal_length; ++i)
                result(i, i) = static_cast<T>(values[i]);
            return result;
        }

        // ---- size / raw access -------------------------------------------

        static constexpr size_type row_count() noexcept { return Rows; }
        static constexpr size_type col_count() noexcept { return Cols; }
        static constexpr size_type element_count() noexcept { return size; }

        constexpr pointer data() noexcept { return &elements[0][0]; }
        constexpr const_pointer data() const noexcept { return &elements[0][0]; }

        // ---- iterators (over stored lines) -------------------------------

        constexpr iterator begin() noexcept { return iterator(elements); }
        constexpr iterator end() noexcept { return iterator(elements + line_count); }
        constexpr const_iterator begin() const noexcept { return const_iterator(elements); }
        constexpr const_iterator end() const noexcept { return const_iterator(elements + line_count); }
        constexpr const_iterator cbegin() const noexcept { return begin(); }
        constexpr const_iterator cend() const noexcept { return end(); }

        constexpr reverse_iterator rbegin() noexcept { return reverse_iterator(end()); }
        constexpr reverse_iterator rend() noexcept { return reverse_iterator(begin()); }
        constexpr const_reverse_iterator rbegin() const noexcept { return const_reverse_iterator(end()); }
        constexpr const_reverse_iterator rend() const noexcept { return const_reverse_iterator(begin()); }
        constexpr const_reverse_iterator crbegin() const noexcept { return rbegin(); }
        constexpr const_reverse_iterator crend() const noexcept { return rend(); }

        // ---- line access -------------------------------------------------
        //
        // An lvalue matrix hands out a view, mutable or const to match the
        // matrix. An rvalue matrix hands out a vector copy instead: a view
        // into a temporary would dangle the moment the expression ended.

      private:
        template <typename Self>
        static constexpr auto line(Self &&self, size_type index) noexcept
        {
            if constexpr (!std::is_lvalue_reference_v<Self>)
                return const_line_view(self.elements[index]).to_vector();
            else if constexpr (std::is_const_v<std::remove_reference_t<Self>>)
                return const_line_view(self.elements[index]);
            else
                return line_view(self.elements[index]);
        }

      public:
        constexpr auto operator[](this auto &&self, size_type index) noexcept
        {
            MATH_ASSERT(index < line_count, "math::matrix::operator[]: line index out of range");
            return line(std::forward<decltype(self)>(self), index);
        }

        constexpr auto at(this auto &&self, size_type index)
        {
            if (index >= line_count)
                throw std::out_of_range("math::matrix::at: index out of range");
            return line(std::forward<decltype(self)>(self), index);
        }

        // Single-index get<I> is a view of the I-th line, for structured
        // bindings: `auto &[r0, r1] = m;` gives two mutable line views.
        //
        // Unlike operator[], this always returns a view, even on an rvalue.
        // Structured bindings call get<I>() on an xvalue whenever the
        // binding is by value (`auto [a, b] = m;`), and the result must
        // match tuple_element, which is fixed per type. That is safe: the
        // hidden copy the binding makes lives as long as the bindings do.
        template <std::size_t I>
            requires(I < line_count)
        constexpr line_view get() noexcept
        {
            return line_view(elements[I]);
        }

        template <std::size_t I>
            requires(I < line_count)
        constexpr const_line_view get() const noexcept
        {
            return const_line_view(elements[I]);
        }

        // ---- element access ----------------------------------------------
        //
        // Always (row, column), whatever the storage order, so callers never
        // have to know how the matrix is laid out. std::forward_like keeps
        // the value category of the matrix, as vector::operator[] does.

        constexpr decltype(auto) operator()(this auto &&self, size_type row, size_type col) noexcept
        {
            MATH_ASSERT(row < Rows && col < Cols, "math::matrix::operator(): index out of range");
            if constexpr (Order == matrix_order::row_major)
                return std::forward_like<decltype(self)>(self.elements[row][col]);
            else
                return std::forward_like<decltype(self)>(self.elements[col][row]);
        }

        constexpr decltype(auto) at(this auto &&self, size_type row, size_type col)
        {
            if (row >= Rows || col >= Cols)
                throw std::out_of_range("math::matrix::at: index out of range");
            return std::forward<decltype(self)>(self)(row, col);
        }

        template <std::size_t R, std::size_t C>
            requires(R < Rows && C < Cols)
        constexpr decltype(auto) get(this auto &&self) noexcept
        {
            return std::forward<decltype(self)>(self)(R, C);
        }

        // ---- rows and columns --------------------------------------------
        //
        // Unlike operator[], which hands out the i-th *stored line*, these
        // always mean row and column, so generic code can use them without
        // knowing the storage order. They copy: for a write-through view of
        // a stored line use operator[].

        constexpr row_type row(size_type i) const noexcept
        {
            row_type result{};
            for (size_type j = 0; j < Cols; ++j)
                result.values[j] = (*this)(i, j);
            return result;
        }

        constexpr column_type column(size_type j) const noexcept
        {
            column_type result{};
            for (size_type i = 0; i < Rows; ++i)
                result.values[i] = (*this)(i, j);
            return result;
        }

        constexpr diagonal_type diagonal() const noexcept
        {
            diagonal_type result{};
            for (size_type i = 0; i < diagonal_length; ++i)
                result.values[i] = (*this)(i, i);
            return result;
        }

        template <vector_like V>
            requires(V::length == Cols)
        constexpr matrix &set_row(size_type i, const V &values) noexcept
        {
            for (size_type j = 0; j < Cols; ++j)
                (*this)(i, j) = static_cast<T>(values[j]);
            return *this;
        }

        template <vector_like V>
            requires(V::length == Rows)
        constexpr matrix &set_column(size_type j, const V &values) noexcept
        {
            for (size_type i = 0; i < Rows; ++i)
                (*this)(i, j) = static_cast<T>(values[i]);
            return *this;
        }

        // ---- conversion --------------------------------------------------

        template <scalar_like U, matrix_order O = Order>
        constexpr matrix<U, Rows, Cols, O> cast() const noexcept
        {
            matrix<U, Rows, Cols, O> result{};
            for (size_type i = 0; i < Rows; ++i)
                for (size_type j = 0; j < Cols; ++j)
                    result(i, j) = static_cast<U>((*this)(i, j));
            return result;
        }

        // ---- comparison --------------------------------------------------

        constexpr bool operator==(const matrix &) const noexcept = default;

        // ---- compound assignment -----------------------------------------
        //
        // Mixed element types are allowed; the result is narrowed back to T
        // exactly as `int i; i += 2.5;` would. *= and /= take scalars only:
        // for the matrix product use `m = m * n`, which cannot be done in
        // place without a temporary anyway.

        template <matrix_like M>
            requires(M::rows == Rows && M::cols == Cols)
        constexpr matrix &operator+=(const M &other) noexcept
        {
            for (size_type i = 0; i < Rows; ++i)
                for (size_type j = 0; j < Cols; ++j)
                    (*this)(i, j) = static_cast<T>((*this)(i, j) + other(i, j));
            return *this;
        }

        template <matrix_like M>
            requires(M::rows == Rows && M::cols == Cols)
        constexpr matrix &operator-=(const M &other) noexcept
        {
            for (size_type i = 0; i < Rows; ++i)
                for (size_type j = 0; j < Cols; ++j)
                    (*this)(i, j) = static_cast<T>((*this)(i, j) - other(i, j));
            return *this;
        }

        constexpr matrix &operator+=(scalar_like auto s) noexcept
        {
            for (auto &line : elements)
                for (auto &e : line)
                    e = static_cast<T>(e + s);
            return *this;
        }

        constexpr matrix &operator-=(scalar_like auto s) noexcept
        {
            for (auto &line : elements)
                for (auto &e : line)
                    e = static_cast<T>(e - s);
            return *this;
        }

        constexpr matrix &operator*=(scalar_like auto s) noexcept
        {
            for (auto &line : elements)
                for (auto &e : line)
                    e = static_cast<T>(e * s);
            return *this;
        }

        constexpr matrix &operator/=(scalar_like auto s) noexcept
        {
            for (auto &line : elements)
                for (auto &e : line)
                    e = static_cast<T>(e / s);
            return *this;
        }
    };

    // ---- aliases ---------------------------------------------------------

    template <scalar_like T>
    using mat2 = matrix<T, 2, 2>;
    template <scalar_like T>
    using mat3 = matrix<T, 3, 3>;
    template <scalar_like T>
    using mat4 = matrix<T, 4, 4>;

    using mat2f = mat2<float>;
    using mat3f = mat3<float>;
    using mat4f = mat4<float>;
    using mat2d = mat2<double>;
    using mat3d = mat3<double>;
    using mat4d = mat4<double>;
    using mat2i = mat2<int>;
    using mat3i = mat3<int>;
    using mat4i = mat4<int>;

    // ---------------------------------------------------------------------
    // Element-wise machinery
    // ---------------------------------------------------------------------

    namespace detail
    {
        // Teaches common_element_t (detail/concepts.hpp) about matrices, so
        // matrix<int> op matrix<float> -> float, and matrix op scalar works.
        template <matrix_like M>
        struct element<M>
        {
            using type = typename bare<M>::value_type;
        };

        // Builds matrix<T, R, C, Order> with element (i, j) = f(i, j). A
        // constexpr loop rather than a pack expansion: T[R][C] would need a
        // nested expansion, which the language cannot spell in one
        // initializer, and a flat expansion relying on brace elision trips
        // -Wmissing-braces. The optimizer unrolls fixed-size loops anyway.
        template <scalar_like T, std::size_t R, std::size_t C, matrix_order Order = matrix_order::row_major>
        constexpr matrix<T, R, C, Order> generate(auto &&f)
        {
            matrix<T, R, C, Order> result{};
            for (std::size_t i = 0; i < R; ++i)
                for (std::size_t j = 0; j < C; ++j)
                    result(i, j) = static_cast<T>(f(i, j));
            return result;
        }

        template <matrix_like L, matrix_like R>
        constexpr auto zip(const L &lhs, const R &rhs, auto op)
        {
            return generate<common_element_t<L, R>, L::rows, L::cols, order_of<L>()>(
                [&](std::size_t i, std::size_t j) { return op(lhs(i, j), rhs(i, j)); });
        }

        template <matrix_like M, scalar_like S>
        constexpr auto zip(const M &m, S s, auto op)
        {
            return generate<common_element_t<M, S>, M::rows, M::cols, order_of<M>()>(
                [&](std::size_t i, std::size_t j) { return op(m(i, j), s); });
        }

        template <typename S, matrix_like M>
            requires scalar_like<S>
        constexpr auto zip(S s, const M &m, auto op)
        {
            return generate<common_element_t<S, M>, M::rows, M::cols, order_of<M>()>(
                [&](std::size_t i, std::size_t j) { return op(s, m(i, j)); });
        }

        template <matrix_like M>
        constexpr auto map(const M &m, auto op)
        {
            return generate<typename element<M>::type, M::rows, M::cols, order_of<M>()>(
                [&](std::size_t i, std::size_t j) { return op(m(i, j)); });
        }
    } // namespace detail

    // ---------------------------------------------------------------------
    // Arithmetic operators
    // ---------------------------------------------------------------------
    //
    // + and - are element-wise, and any operator with a scalar is
    // element-wise. Between two matrices, or a matrix and a vector, * is the
    // linear-algebra product (the GLSL convention, which is also what mat4 *
    // vec4 means to every graphics programmer); the element-wise product is
    // spelled hadamard().
    //
    // The scalar-first overloads check matrix_like<M> before scalar_like<S>
    // for the reason given above the vector operators: scalar_like is
    // defined in terms of the element type's own operators, and must not be
    // re-entered while resolving them.

    template <matrix_like M>
    constexpr auto operator+(const M &m)
    {
        return detail::map(m, [](auto e) { return +e; });
    }

    template <matrix_like M>
    constexpr auto operator-(const M &m)
    {
        return detail::map(m, [](auto e) { return -e; });
    }

    template <matrix_like L, matrix_like R>
        requires(L::rows == R::rows && L::cols == R::cols)
    constexpr auto operator+(const L &lhs, const R &rhs)
    {
        return detail::zip(lhs, rhs, std::plus<>{});
    }
    template <matrix_like M, scalar_like S>
    constexpr auto operator+(const M &m, S s)
    {
        return detail::zip(m, s, std::plus<>{});
    }
    template <typename S, matrix_like M>
        requires scalar_like<S>
    constexpr auto operator+(S s, const M &m)
    {
        return detail::zip(s, m, std::plus<>{});
    }

    template <matrix_like L, matrix_like R>
        requires(L::rows == R::rows && L::cols == R::cols)
    constexpr auto operator-(const L &lhs, const R &rhs)
    {
        return detail::zip(lhs, rhs, std::minus<>{});
    }
    template <matrix_like M, scalar_like S>
    constexpr auto operator-(const M &m, S s)
    {
        return detail::zip(m, s, std::minus<>{});
    }
    template <typename S, matrix_like M>
        requires scalar_like<S>
    constexpr auto operator-(S s, const M &m)
    {
        return detail::zip(s, m, std::minus<>{});
    }

    template <matrix_like M, scalar_like S>
    constexpr auto operator*(const M &m, S s)
    {
        return detail::zip(m, s, std::multiplies<>{});
    }
    template <typename S, matrix_like M>
        requires scalar_like<S>
    constexpr auto operator*(S s, const M &m)
    {
        return detail::zip(s, m, std::multiplies<>{});
    }

    template <matrix_like M, scalar_like S>
    constexpr auto operator/(const M &m, S s)
    {
        return detail::zip(m, s, std::divides<>{});
    }
    template <typename S, matrix_like M>
        requires scalar_like<S>
    constexpr auto operator/(S s, const M &m)
    {
        return detail::zip(s, m, std::divides<>{});
    }

    // Element-wise (Hadamard) product.
    template <matrix_like L, matrix_like R>
        requires(L::rows == R::rows && L::cols == R::cols)
    constexpr auto hadamard(const L &lhs, const R &rhs)
    {
        return detail::zip(lhs, rhs, std::multiplies<>{});
    }

    // ---- products --------------------------------------------------------

    // (R x K) * (K x C) -> (R x C), in the left operand's storage order.
    template <matrix_like L, matrix_like R>
        requires(L::cols == R::rows)
    constexpr auto operator*(const L &lhs, const R &rhs)
    {
        using T = detail::common_element_t<L, R>;
        return detail::generate<T, L::rows, R::cols, detail::order_of<L>()>([&](std::size_t i, std::size_t j) {
            T total{};
            for (std::size_t k = 0; k < L::cols; ++k)
                total = static_cast<T>(total + lhs(i, k) * rhs(k, j));
            return total;
        });
    }

    // Matrix times column vector: (R x C) * C -> R.
    template <matrix_like M, vector_like V>
        requires(M::cols == V::length)
    constexpr auto operator*(const M &m, const V &v)
    {
        using T = detail::common_element_t<M, V>;
        return detail::generate<T, M::rows>([&](std::size_t i) {
            T total{};
            for (std::size_t k = 0; k < M::cols; ++k)
                total = static_cast<T>(total + m(i, k) * v[k]);
            return total;
        });
    }

    // Row vector times matrix: R * (R x C) -> C.
    template <vector_like V, matrix_like M>
        requires(V::length == M::rows)
    constexpr auto operator*(const V &v, const M &m)
    {
        using T = detail::common_element_t<V, M>;
        return detail::generate<T, M::cols>([&](std::size_t j) {
            T total{};
            for (std::size_t k = 0; k < M::rows; ++k)
                total = static_cast<T>(total + v[k] * m(k, j));
            return total;
        });
    }
    // ---------------------------------------------------------------------
    // Comparison
    // ---------------------------------------------------------------------
    //
    // Equality is by (row, col) content, so a row-major and a column-major
    // matrix holding the same values compare equal, as their std::hash
    // values do. Same-type matrix comparisons still go to the defaulted
    // member, which as a non-template beats this on a tie; matrix_view has
    // no member and relies on this for every comparison.

    template <matrix_like L, matrix_like R>
        requires(L::rows == R::rows && L::cols == R::cols)
    constexpr bool operator==(const L &lhs, const R &rhs)
    {
        for (std::size_t i = 0; i < L::rows; ++i)
            for (std::size_t j = 0; j < L::cols; ++j)
                if (!(lhs(i, j) == rhs(i, j)))
                    return false;
        return true;
    }

} // namespace math

// -------------------------------------------------------------------------
// std integration: structured bindings, hashing, std::format
// -------------------------------------------------------------------------
//
// Structured bindings decompose a matrix into its stored lines:
//   auto &[r0, r1] = m;        // two mutable line views into m
//   const auto &[a, b] = m;    // two const line views
//   auto [x, y] = m;           // views into the hidden copy the binding made
// tuple_element is specialised for const separately because the library's
// generic const forwarding would produce `const vector_view<T, N>`, which a
// `vector_view<const T, N>` does not convert to.

namespace std
{
    template <typename T, std::size_t R, std::size_t C, math::matrix_order Order>
    struct tuple_size<math::matrix<T, R, C, Order>>
        : std::integral_constant<std::size_t, math::matrix<T, R, C, Order>::line_count>
    {
    };

    template <std::size_t I, typename T, std::size_t R, std::size_t C, math::matrix_order Order>
    struct tuple_element<I, math::matrix<T, R, C, Order>>
    {
        using type = typename math::matrix<T, R, C, Order>::line_view;
    };

    template <std::size_t I, typename T, std::size_t R, std::size_t C, math::matrix_order Order>
    struct tuple_element<I, const math::matrix<T, R, C, Order>>
    {
        using type = typename math::matrix<T, R, C, Order>::const_line_view;
    };

    template <typename T, std::size_t R, std::size_t C, math::matrix_order Order>
    struct hash<math::matrix<T, R, C, Order>>
    {
        std::size_t operator()(const math::matrix<T, R, C, Order> &m) const noexcept
        {
            std::size_t seed = 0;
            for (std::size_t i = 0; i < R; ++i)
                for (std::size_t j = 0; j < C; ++j)
                    math::detail::hash_combine(seed, m(i, j));
            return seed;
        }
    };

    // Formats as "[a, b; c, d]": rows separated by "; ", in (row, column)
    // order whatever the storage. The format spec applies to every element.
    template <typename T, std::size_t R, std::size_t C, math::matrix_order Order, typename CharT>
    struct formatter<math::matrix<T, R, C, Order>, CharT> : formatter<T, CharT>
    {
        template <typename FormatContext>
        auto format(const math::matrix<T, R, C, Order> &m, FormatContext &ctx) const
        {
            auto out = ctx.out();
            *out++ = CharT('[');
            for (std::size_t i = 0; i < R; ++i)
            {
                if (i != 0)
                {
                    *out++ = CharT(';');
                    *out++ = CharT(' ');
                }
                for (std::size_t j = 0; j < C; ++j)
                {
                    if (j != 0)
                    {
                        *out++ = CharT(',');
                        *out++ = CharT(' ');
                    }
                    ctx.advance_to(out);
                    out = formatter<T, CharT>::format(m(i, j), ctx);
                }
            }
            *out++ = CharT(']');
            return out;
        }
    };
} // namespace std

