#pragma once

#include "../vector_view.hpp"
#include "concepts.hpp"

#include <compare>
#include <cstddef>
#include <iterator>
#include <type_traits>

// -------------------------------------------------------------------------
// matrix_iterator
// -------------------------------------------------------------------------
//
// Walks the lines of a T[Count][N] array and dereferences to a
// vector_view<T, N> by value. Because the "reference" is a prvalue proxy
// this cannot be a legacy ForwardIterator (which demands a true
// reference), but it fully models the C++20 random_access_iterator
// concept, which is what <ranges> and range-for care about.
//
// It steps a pointer-to-array rather than a flat T*, so the arithmetic
// never crosses a subobject boundary and stays valid in constant
// evaluation.

namespace math
{

    template <typename T, std::size_t N>
    struct matrix_iterator
    {
        using value_type = vector_view<T, N>;
        using reference = vector_view<T, N>;
        using pointer = void;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::input_iterator_tag;
        using iterator_concept = std::random_access_iterator_tag;

        using line_pointer = T (*)[N];

        line_pointer ptr{};

        constexpr matrix_iterator() = default;
        constexpr explicit matrix_iterator(line_pointer p) noexcept : ptr(p) {}

        // iterator -> const_iterator conversion, same trick as vector_iterator.
        template <typename U>
            requires(std::is_const_v<T> && std::same_as<U, std::remove_cv_t<T>>)
        constexpr matrix_iterator(const matrix_iterator<U, N> &other) noexcept : ptr(other.ptr)
        {
        }

        // Dereference
        constexpr reference operator*() const noexcept { return reference(*ptr); }
        constexpr reference operator[](difference_type n) const noexcept { return reference(ptr[n]); }

        // Increment / decrement
        constexpr matrix_iterator &operator++() noexcept
        {
            ++ptr;
            return *this;
        }
        constexpr matrix_iterator operator++(int) noexcept
        {
            auto tmp = *this;
            ++ptr;
            return tmp;
        }
        constexpr matrix_iterator &operator--() noexcept
        {
            --ptr;
            return *this;
        }
        constexpr matrix_iterator operator--(int) noexcept
        {
            auto tmp = *this;
            --ptr;
            return tmp;
        }

        // Arithmetic
        constexpr matrix_iterator &operator+=(difference_type n) noexcept
        {
            ptr += n;
            return *this;
        }
        constexpr matrix_iterator &operator-=(difference_type n) noexcept
        {
            ptr -= n;
            return *this;
        }
        constexpr matrix_iterator operator+(difference_type n) const noexcept { return matrix_iterator(ptr + n); }
        constexpr matrix_iterator operator-(difference_type n) const noexcept { return matrix_iterator(ptr - n); }
        friend constexpr matrix_iterator operator+(difference_type n, const matrix_iterator &it) noexcept
        {
            return it + n;
        }

        // Difference
        constexpr difference_type operator-(const matrix_iterator &other) const noexcept { return ptr - other.ptr; }

        // Comparison
        constexpr bool operator==(const matrix_iterator &) const noexcept = default;
        constexpr auto operator<=>(const matrix_iterator &) const noexcept = default;
    };

} // namespace math
