#pragma once

#include "detail/config.hpp"
#include "matrix.hpp"
#include "vector_view.hpp"

namespace math
{

    // ---------------------------------------------------------------------
    // matrix_view
    // ---------------------------------------------------------------------
    //
    // A non-owning window onto Rows x Cols elements laid out as a matrix
    // would store them: line_count contiguous lines of line_length T. It
    // models matrix_like, so every operator and free function written
    // against it (+, *, hadamard, transpose, ...) accepts it and returns a
    // real matrix.
    //
    // Semantics follow vector_view, and therefore std::span:
    //   * copying or assigning a view rebinds it; it never copies elements.
    //     Use assign() to write elements through the view.
    //   * constness is shallow. A const matrix_view<int, ...> still writes
    //     through; a matrix_view<const int, ...> does not.
    //   * assignment is lvalue-only, so assigning to a temporary view is a
    //     compile error instead of a silent rebind.
    //
    // Order says how to read the viewed lines: for row_major each line is a
    // row, for column_major each line is a column. A raw T[2][3] can thus be
    // viewed as a 2x3 row-major or a 3x2 column-major matrix.

    template <typename T, std::size_t Rows, std::size_t Cols, matrix_order Order = matrix_order::row_major>
        requires scalar_like<std::remove_cv_t<T>> && (Rows > 0 && Cols > 0)
    struct matrix_view
    {
        static constexpr matrix_order order = Order;
        static constexpr std::size_t rows = Rows;
        static constexpr std::size_t cols = Cols;
        static constexpr std::size_t size = Rows * Cols;

        using element_type = T;
        using value_type = std::remove_cv_t<T>;
        using order_type = matrix_order;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using pointer = T *;
        using reference = T &;

        using storage = detail::matrix_storage<value_type, Rows, Cols, Order>;
        static constexpr std::size_t line_count = storage::line_count;
        static constexpr std::size_t line_length = std::extent_v<typename storage::line_type>;

        using line_type = T[line_length];
        using line_pointer = T (*)[line_length];
        using line_view = vector_view<T, line_length>;
        using const_line_view = vector_view<const T, line_length>;

        using row_type = vector<value_type, Cols>;
        using column_type = vector<value_type, Rows>;
        static constexpr std::size_t diagonal_length = Rows < Cols ? Rows : Cols;
        using diagonal_type = vector<value_type, diagonal_length>;

        using iterator = matrix_iterator<T, line_length>;
        using const_iterator = matrix_iterator<const T, line_length>;
        using reverse_iterator = std::reverse_iterator<iterator>;
        using const_reverse_iterator = std::reverse_iterator<const_iterator>;

        using owner_type = matrix<value_type, Rows, Cols, Order>;

        line_pointer ptr;

        // ---- construction ------------------------------------------------

        // A template for the same reason as vector_view: a raw 2D array
        // decays to line_pointer, and a non-template beats a template on a
        // tie, so the array constructor wins instead of being ambiguous.
        template <std::same_as<line_pointer> P>
        constexpr explicit matrix_view(P p) noexcept : ptr(p)
        {
        }

        constexpr matrix_view(T (&array)[line_count][line_length]) noexcept : ptr(array) {}

        constexpr matrix_view(owner_type &m) noexcept
            requires(!std::is_const_v<T>)
            : ptr(m.elements)
        {
        }

        constexpr matrix_view(const owner_type &m) noexcept
            requires std::is_const_v<T>
            : ptr(m.elements)
        {
        }

        // view -> view-of-const conversion.
        template <typename U>
            requires(std::is_const_v<T> && std::same_as<U, value_type>)
        constexpr matrix_view(const matrix_view<U, Rows, Cols, Order> &other) noexcept : ptr(other.ptr)
        {
        }

        constexpr matrix_view(const matrix_view &) noexcept = default;
        constexpr matrix_view &operator=(const matrix_view &) & noexcept = default;

        // ---- size / raw access -------------------------------------------

        static constexpr size_type row_count() noexcept { return Rows; }
        static constexpr size_type col_count() noexcept { return Cols; }
        static constexpr size_type element_count() noexcept { return size; }

        constexpr pointer data() const noexcept { return &ptr[0][0]; }

        // ---- iterators (over viewed lines) -------------------------------

        constexpr iterator begin() const noexcept { return iterator(ptr); }
        constexpr iterator end() const noexcept { return iterator(ptr + line_count); }
        constexpr const_iterator cbegin() const noexcept { return const_iterator(ptr); }
        constexpr const_iterator cend() const noexcept { return const_iterator(ptr + line_count); }

        constexpr reverse_iterator rbegin() const noexcept { return reverse_iterator(end()); }
        constexpr reverse_iterator rend() const noexcept { return reverse_iterator(begin()); }
        constexpr const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(cend()); }
        constexpr const_reverse_iterator crend() const noexcept { return const_reverse_iterator(cbegin()); }

        // ---- line access -------------------------------------------------

        constexpr line_view operator[](size_type index) const noexcept
        {
            MATH_ASSERT(index < line_count, "math::matrix_view::operator[]: line index out of range");
            return line_view(ptr[index]);
        }

        constexpr line_view at(size_type index) const
        {
            if (index >= line_count)
                throw std::out_of_range("math::matrix_view::at: index out of range");
            return line_view(ptr[index]);
        }

        template <std::size_t I>
            requires(I < line_count)
        constexpr line_view get() const noexcept
        {
            return line_view(ptr[I]);
        }

        // ---- element access ----------------------------------------------
        //
        // Always (row, column), whatever the storage order.

        constexpr reference operator()(size_type row, size_type col) const noexcept
        {
            MATH_ASSERT(row < Rows && col < Cols, "math::matrix_view::operator(): index out of range");
            if constexpr (Order == matrix_order::row_major)
                return ptr[row][col];
            else
                return ptr[col][row];
        }

        constexpr reference at(size_type row, size_type col) const
        {
            if (row >= Rows || col >= Cols)
                throw std::out_of_range("math::matrix_view::at: index out of range");
            return (*this)(row, col);
        }

        template <std::size_t R, std::size_t C>
            requires(R < Rows && C < Cols)
        constexpr reference get() const noexcept
        {
            return (*this)(R, C);
        }

        // ---- rows and columns --------------------------------------------
        //
        // Same contract as matrix: always row / column whatever the storage
        // order, copies out, and the setters write through.

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
            requires(V::length == Cols && !std::is_const_v<T>)
        constexpr const matrix_view &set_row(size_type i, const V &values) const noexcept
        {
            for (size_type j = 0; j < Cols; ++j)
                (*this)(i, j) = static_cast<value_type>(values[j]);
            return *this;
        }

        template <vector_like V>
            requires(V::length == Rows && !std::is_const_v<T>)
        constexpr const matrix_view &set_column(size_type j, const V &values) const noexcept
        {
            for (size_type i = 0; i < Rows; ++i)
                (*this)(i, j) = static_cast<value_type>(values[i]);
            return *this;
        }

        // ---- copy out ----------------------------------------------------

        constexpr owner_type to_matrix() const noexcept
        {
            owner_type result{};
            for (size_type i = 0; i < Rows; ++i)
                for (size_type j = 0; j < Cols; ++j)
                    result(i, j) = (*this)(i, j);
            return result;
        }

        constexpr operator owner_type() const noexcept { return to_matrix(); }

        // ---- write through -----------------------------------------------

        template <matrix_like M>
            requires(M::rows == Rows && M::cols == Cols && !std::is_const_v<T>)
        constexpr const matrix_view &assign(const M &other) const noexcept
        {
            for (size_type i = 0; i < Rows; ++i)
                for (size_type j = 0; j < Cols; ++j)
                    (*this)(i, j) = static_cast<value_type>(other(i, j));
            return *this;
        }

        constexpr const matrix_view &fill(value_type value) const noexcept
            requires(!std::is_const_v<T>)
        {
            for (size_type i = 0; i < line_count; ++i)
                for (size_type j = 0; j < line_length; ++j)
                    ptr[i][j] = value;
            return *this;
        }

        // ---- compound assignment -----------------------------------------
        //
        // Same narrowing rules as matrix. Const-qualified because the view
        // itself does not change; only the viewed elements do.

        template <matrix_like M>
            requires(M::rows == Rows && M::cols == Cols && !std::is_const_v<T>)
        constexpr const matrix_view &operator+=(const M &other) const noexcept
        {
            for (size_type i = 0; i < Rows; ++i)
                for (size_type j = 0; j < Cols; ++j)
                    (*this)(i, j) = static_cast<value_type>((*this)(i, j) + other(i, j));
            return *this;
        }

        template <matrix_like M>
            requires(M::rows == Rows && M::cols == Cols && !std::is_const_v<T>)
        constexpr const matrix_view &operator-=(const M &other) const noexcept
        {
            for (size_type i = 0; i < Rows; ++i)
                for (size_type j = 0; j < Cols; ++j)
                    (*this)(i, j) = static_cast<value_type>((*this)(i, j) - other(i, j));
            return *this;
        }

        constexpr const matrix_view &operator+=(scalar_like auto s) const noexcept
            requires(!std::is_const_v<T>)
        {
            for (size_type i = 0; i < line_count; ++i)
                for (size_type j = 0; j < line_length; ++j)
                    ptr[i][j] = static_cast<value_type>(ptr[i][j] + s);
            return *this;
        }

        constexpr const matrix_view &operator-=(scalar_like auto s) const noexcept
            requires(!std::is_const_v<T>)
        {
            for (size_type i = 0; i < line_count; ++i)
                for (size_type j = 0; j < line_length; ++j)
                    ptr[i][j] = static_cast<value_type>(ptr[i][j] - s);
            return *this;
        }

        constexpr const matrix_view &operator*=(scalar_like auto s) const noexcept
            requires(!std::is_const_v<T>)
        {
            for (size_type i = 0; i < line_count; ++i)
                for (size_type j = 0; j < line_length; ++j)
                    ptr[i][j] = static_cast<value_type>(ptr[i][j] * s);
            return *this;
        }

        constexpr const matrix_view &operator/=(scalar_like auto s) const noexcept
            requires(!std::is_const_v<T>)
        {
            for (size_type i = 0; i < line_count; ++i)
                for (size_type j = 0; j < line_length; ++j)
                    ptr[i][j] = static_cast<value_type>(ptr[i][j] / s);
            return *this;
        }
    };

    // matrix_view(m) deduces everything from the matrix. A raw T[R][C] is
    // read as R x C row-major; spell the template arguments out to view it
    // as C x R column-major instead.
    template <typename T, std::size_t R, std::size_t C, matrix_order O>
    matrix_view(matrix<T, R, C, O> &) -> matrix_view<T, R, C, O>;

    template <typename T, std::size_t R, std::size_t C, matrix_order O>
    matrix_view(const matrix<T, R, C, O> &) -> matrix_view<const T, R, C, O>;

    template <typename T, std::size_t R, std::size_t C>
    matrix_view(T (&)[R][C]) -> matrix_view<T, R, C, matrix_order::row_major>;

} // namespace math

// -------------------------------------------------------------------------
// std integration: structured bindings and std::format
// -------------------------------------------------------------------------
//
// `auto [a, b] = view;` copies the view (a rebind), then each binding is a
// line view into the viewed storage, so writes go through. No hash, for
// the same reason as vector_view.

namespace std
{
    template <typename T, std::size_t R, std::size_t C, math::matrix_order O>
    struct tuple_size<math::matrix_view<T, R, C, O>>
        : std::integral_constant<std::size_t, math::matrix_view<T, R, C, O>::line_count>
    {
    };

    template <std::size_t I, typename T, std::size_t R, std::size_t C, math::matrix_order O>
    struct tuple_element<I, math::matrix_view<T, R, C, O>>
    {
        using type = typename math::matrix_view<T, R, C, O>::line_view;
    };

    template <typename T, std::size_t R, std::size_t C, math::matrix_order O, typename CharT>
    struct formatter<math::matrix_view<T, R, C, O>, CharT> : formatter<math::matrix<std::remove_cv_t<T>, R, C, O>, CharT>
    {
        template <typename FormatContext>
        auto format(const math::matrix_view<T, R, C, O> &v, FormatContext &ctx) const
        {
            return formatter<math::matrix<std::remove_cv_t<T>, R, C, O>, CharT>::format(v.to_matrix(), ctx);
        }
    };
} // namespace std
