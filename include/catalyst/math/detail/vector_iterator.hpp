#pragma once

#include "concepts.hpp"

#include <compare>
#include <cstddef>
#include <iterator>
#include <type_traits>

// -------------------------------------------------------------------------
// vector_iterator
// -------------------------------------------------------------------------
//
// A contiguous iterator over a vector's elements. It is a thin wrapper over
// a pointer rather than the pointer itself so that vector<T, N>::iterator is
// a distinct type from const T*, which keeps overloads and diagnostics
// honest.

namespace math
{

    template <typename T>
    struct vector_iterator
    {
        using value_type = std::remove_cv_t<T>;
        using pointer = T *;
        using reference = T &;
        using difference_type = std::ptrdiff_t;
        using iterator_category = std::random_access_iterator_tag;
        using iterator_concept = std::contiguous_iterator_tag;

        pointer ptr{};

        constexpr vector_iterator() = default;
        constexpr explicit vector_iterator(pointer p) noexcept : ptr(p) {}

        // iterator -> const_iterator conversion. A template so it can never be
        // mistaken for the copy constructor.
        template <typename U>
            requires(std::is_const_v<T> && std::same_as<U, value_type>)
        constexpr vector_iterator(const vector_iterator<U> &other) noexcept : ptr(other.ptr)
        {
        }

        // Dereference
        constexpr reference operator*() const noexcept { return *ptr; }
        constexpr pointer operator->() const noexcept { return ptr; }
        constexpr reference operator[](difference_type n) const noexcept { return ptr[n]; }

        // Increment / decrement
        constexpr vector_iterator &operator++() noexcept
        {
            ++ptr;
            return *this;
        }
        constexpr vector_iterator operator++(int) noexcept
        {
            auto tmp = *this;
            ++ptr;
            return tmp;
        }
        constexpr vector_iterator &operator--() noexcept
        {
            --ptr;
            return *this;
        }
        constexpr vector_iterator operator--(int) noexcept
        {
            auto tmp = *this;
            --ptr;
            return tmp;
        }

        // Arithmetic
        constexpr vector_iterator &operator+=(difference_type n) noexcept
        {
            ptr += n;
            return *this;
        }
        constexpr vector_iterator &operator-=(difference_type n) noexcept
        {
            ptr -= n;
            return *this;
        }
        constexpr vector_iterator operator+(difference_type n) const noexcept { return vector_iterator(ptr + n); }
        constexpr vector_iterator operator-(difference_type n) const noexcept { return vector_iterator(ptr - n); }
        friend constexpr vector_iterator operator+(difference_type n, const vector_iterator &it) noexcept
        {
            return it + n;
        }

        // Difference
        constexpr difference_type operator-(const vector_iterator &other) const noexcept { return ptr - other.ptr; }

        // Comparison
        constexpr bool operator==(const vector_iterator &) const noexcept = default;
        constexpr auto operator<=>(const vector_iterator &) const noexcept = default;
    };

} // namespace math
