#pragma once

#include "detail/config.hpp"
#include "vector.hpp"

namespace math
{

    // ---------------------------------------------------------------------
    // vector_view
    // ---------------------------------------------------------------------
    //
    // A non-owning window onto N contiguous T. It models the same
    // vector_like concept as vector, so every operator and free function
    // written against it (operator+, dot, cross, ...) accepts it and returns
    // a real vector.
    //
    // Semantics follow std::span, not a reference:
    //   * copying or assigning a view rebinds it; it never copies elements.
    //     Use assign() to write elements through the view.
    //   * constness is shallow. A const vector_view<int, 3> still writes
    //     through; a vector_view<const int, 3> does not. Owners hand out the
    //     right one, see matrix::operator[].
    //   * assignment is lvalue-only, so `m[0] = other` is a compile error
    //     instead of a silent rebind of a temporary.

    template <typename T, std::size_t N>
        requires scalar_like<std::remove_cv_t<T>> && (N > 0)
    struct vector_view
    {
        static constexpr std::size_t length = N;

        using element_type = T;
        using value_type = std::remove_cv_t<T>;
        using size_type = std::size_t;
        using difference_type = std::ptrdiff_t;
        using reference = T &;
        using const_reference = const T &;
        using pointer = T *;
        using const_pointer = const T *;

        using iterator = vector_iterator<T>;
        using const_iterator = vector_iterator<const T>;
        using reverse_iterator = std::reverse_iterator<iterator>;
        using const_reverse_iterator = std::reverse_iterator<const_iterator>;

        using owner_type = vector<value_type, N>;

        pointer ptr;

        // ---- construction ------------------------------------------------

        // The pointer constructor is a template so that a raw array, which
        // matches the array constructor exactly and this one after decay,
        // is not ambiguous: a non-template beats a template on a tie.
        template <std::same_as<pointer> P>
        constexpr explicit vector_view(P p) noexcept : ptr(p)
        {
        }

        constexpr vector_view(T (&array)[N]) noexcept : ptr(array) {}

        constexpr vector_view(owner_type &v) noexcept
            requires(!std::is_const_v<T>)
            : ptr(v.values)
        {
        }

        constexpr vector_view(const owner_type &v) noexcept
            requires std::is_const_v<T>
            : ptr(v.values)
        {
        }

        // view -> view-of-const conversion. A template so it can never be
        // mistaken for the copy constructor (same trick as vector_iterator).
        template <typename U>
            requires(std::is_const_v<T> && std::same_as<U, value_type>)
        constexpr vector_view(const vector_view<U, N> &other) noexcept : ptr(other.ptr)
        {
        }

        constexpr vector_view(const vector_view &) noexcept = default;
        constexpr vector_view &operator=(const vector_view &) & noexcept = default;

        // ---- size / raw access -------------------------------------------

        static constexpr size_type size() noexcept { return N; }

        constexpr pointer data() const noexcept { return ptr; }

        // ---- iterators ---------------------------------------------------

        constexpr iterator begin() const noexcept { return iterator(ptr); }
        constexpr iterator end() const noexcept { return iterator(ptr + N); }
        constexpr const_iterator cbegin() const noexcept { return const_iterator(ptr); }
        constexpr const_iterator cend() const noexcept { return const_iterator(ptr + N); }

        constexpr reverse_iterator rbegin() const noexcept { return reverse_iterator(end()); }
        constexpr reverse_iterator rend() const noexcept { return reverse_iterator(begin()); }
        constexpr const_reverse_iterator crbegin() const noexcept { return const_reverse_iterator(cend()); }
        constexpr const_reverse_iterator crend() const noexcept { return const_reverse_iterator(cbegin()); }

        // ---- element access ----------------------------------------------

        constexpr reference operator[](size_type index) const noexcept
        {
            MATH_ASSERT(index < N, "math::vector_view::operator[]: index out of range");
            return ptr[index];
        }

        constexpr reference at(size_type index) const
        {
            if (index >= N)
                throw std::out_of_range("math::vector_view::at: index out of range");
            return ptr[index];
        }

        template <std::size_t I>
            requires(I < N)
        constexpr reference get() const noexcept
        {
            return ptr[I];
        }

        constexpr reference x() const noexcept
            requires(N > 0)
        {
            return ptr[0];
        }
        constexpr reference y() const noexcept
            requires(N > 1)
        {
            return ptr[1];
        }
        constexpr reference z() const noexcept
            requires(N > 2)
        {
            return ptr[2];
        }
        constexpr reference w() const noexcept
            requires(N > 3)
        {
            return ptr[3];
        }

        // ---- copy out ----------------------------------------------------

        constexpr owner_type to_vector() const noexcept
        {
            owner_type result{};
            for (size_type i = 0; i < N; ++i)
                result.values[i] = ptr[i];
            return result;
        }

        constexpr operator owner_type() const noexcept { return to_vector(); }

        // ---- write through -----------------------------------------------

        template <vector_like V>
            requires(V::length == N && !std::is_const_v<T>)
        constexpr const vector_view &assign(const V &other) const noexcept
        {
            for (size_type i = 0; i < N; ++i)
                ptr[i] = static_cast<value_type>(other[i]);
            return *this;
        }

        constexpr const vector_view &fill(value_type value) const noexcept
            requires(!std::is_const_v<T>)
        {
            for (size_type i = 0; i < N; ++i)
                ptr[i] = value;
            return *this;
        }

        // ---- comparison --------------------------------------------------
        //
        // Compares elements, not identity. vector == view also works through
        // the C++20 reversed candidate.

        template <vector_like V>
            requires(V::length == N)
        constexpr bool operator==(const V &other) const noexcept
        {
            for (size_type i = 0; i < N; ++i)
                if (!(ptr[i] == other[i]))
                    return false;
            return true;
        }

        // ---- compound assignment -----------------------------------------
        //
        // Same narrowing rules as vector. Const-qualified because the view
        // itself does not change; only the viewed elements do.

        template <vector_like V>
            requires(V::length == N && !std::is_const_v<T>)
        constexpr const vector_view &operator+=(const V &other) const noexcept
        {
            for (size_type i = 0; i < N; ++i)
                ptr[i] = static_cast<value_type>(ptr[i] + other[i]);
            return *this;
        }

        template <vector_like V>
            requires(V::length == N && !std::is_const_v<T>)
        constexpr const vector_view &operator-=(const V &other) const noexcept
        {
            for (size_type i = 0; i < N; ++i)
                ptr[i] = static_cast<value_type>(ptr[i] - other[i]);
            return *this;
        }

        template <vector_like V>
            requires(V::length == N && !std::is_const_v<T>)
        constexpr const vector_view &operator*=(const V &other) const noexcept
        {
            for (size_type i = 0; i < N; ++i)
                ptr[i] = static_cast<value_type>(ptr[i] * other[i]);
            return *this;
        }

        template <vector_like V>
            requires(V::length == N && !std::is_const_v<T>)
        constexpr const vector_view &operator/=(const V &other) const noexcept
        {
            for (size_type i = 0; i < N; ++i)
                ptr[i] = static_cast<value_type>(ptr[i] / other[i]);
            return *this;
        }

        constexpr const vector_view &operator+=(scalar_like auto s) const noexcept
            requires(!std::is_const_v<T>)
        {
            for (size_type i = 0; i < N; ++i)
                ptr[i] = static_cast<value_type>(ptr[i] + s);
            return *this;
        }

        constexpr const vector_view &operator-=(scalar_like auto s) const noexcept
            requires(!std::is_const_v<T>)
        {
            for (size_type i = 0; i < N; ++i)
                ptr[i] = static_cast<value_type>(ptr[i] - s);
            return *this;
        }

        constexpr const vector_view &operator*=(scalar_like auto s) const noexcept
            requires(!std::is_const_v<T>)
        {
            for (size_type i = 0; i < N; ++i)
                ptr[i] = static_cast<value_type>(ptr[i] * s);
            return *this;
        }

        constexpr const vector_view &operator/=(scalar_like auto s) const noexcept
            requires(!std::is_const_v<T>)
        {
            for (size_type i = 0; i < N; ++i)
                ptr[i] = static_cast<value_type>(ptr[i] / s);
            return *this;
        }
    };

    // vector_view(arr) and vector_view(vec) deduce T and N.
    template <typename T, std::size_t N>
    vector_view(T (&)[N]) -> vector_view<T, N>;

    template <typename T, std::size_t N>
    vector_view(vector<T, N> &) -> vector_view<T, N>;

    template <typename T, std::size_t N>
    vector_view(const vector<T, N> &) -> vector_view<const T, N>;

} // namespace math

// -------------------------------------------------------------------------
// std integration: structured bindings and std::format
// -------------------------------------------------------------------------
//
// `auto [x, y, z] = view;` copies the view (a rebind), then each binding is
// a T& into the viewed storage, so writes go through. No hash: hashing a
// view by identity or by contents are both surprising, so neither is chosen.

namespace std
{
    template <typename T, std::size_t N>
    struct tuple_size<math::vector_view<T, N>> : std::integral_constant<std::size_t, N>
    {
    };

    template <std::size_t I, typename T, std::size_t N>
    struct tuple_element<I, math::vector_view<T, N>>
    {
        using type = T;
    };

    template <typename T, std::size_t N, typename CharT>
    struct formatter<math::vector_view<T, N>, CharT> : formatter<math::vector<std::remove_cv_t<T>, N>, CharT>
    {
        template <typename FormatContext>
        auto format(const math::vector_view<T, N> &v, FormatContext &ctx) const
        {
            return formatter<math::vector<std::remove_cv_t<T>, N>, CharT>::format(v.to_vector(), ctx);
        }
    };
} // namespace std
