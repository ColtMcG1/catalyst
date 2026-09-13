#pragma once

#include <cstddef>
#include <functional>

// -------------------------------------------------------------------------
// Hashing
// -------------------------------------------------------------------------
//
// The one step every std::hash specialization in this library is built from.

namespace math
{
    namespace detail
    {
        // Folds one more element into a running hash (the boost::hash_combine
        // recipe). Element order matters, so two containers holding the same
        // values in a different order hash differently.
        template <typename T>
        constexpr void hash_combine(std::size_t &seed, const T &value) noexcept
        {
            seed ^= std::hash<T>{}(value) + 0x9e3779b97f4a7c15ull + (seed << 6) + (seed >> 2);
        }
    } // namespace detail

} // namespace math
