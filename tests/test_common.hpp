#pragma once

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

#define CT_REQUIRE(expr)                                                \
    do                                                                  \
    {                                                                   \
        if (!(expr))                                                    \
        {                                                               \
            ::catalyst::tests::fail(#expr, __FILE__, __LINE__);         \
        }                                                               \
    } while (false)

} // namespace catalyst::tests
