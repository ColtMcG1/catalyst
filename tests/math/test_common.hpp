#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <string_view>

namespace catalyst::tests
{

[[noreturn]] inline void fail(std::string_view expr, std::string_view file, int line)
{
    std::cerr << "TEST FAILED: " << expr << " (" << file << ":" << line << ")\n";
    std::exit(1);
}

#define CT_REQUIRE(expr) \
    do                 \
    {                  \
        if (!(expr))   \
        {              \
            ::catalyst::tests::fail(#expr, __FILE__, __LINE__); \
        }              \
    } while (false)

template <class T>
inline bool nearly_equal(T a, T b, T eps = static_cast<T>(1e-5)) noexcept
{
    const T diff = std::fabs(a - b);
    if (diff <= eps)
        return true;

    const T scale = (std::max)(std::fabs(a), std::fabs(b));
    return diff <= eps * (std::max)(static_cast<T>(1), scale);
}

} // namespace catalyst::tests
