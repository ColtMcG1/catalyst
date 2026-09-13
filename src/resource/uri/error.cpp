/**
 * @file error.cpp
 * @brief Renders a @ref catalyst::resource::uri_error as the sentence an error message wants.
 * License: MIT (see LICENSE).
 */

#include <catalyst/resource/uri/error.hpp>

#include <string>

namespace catalyst::resource
{

    std::string uri_error::message() const
    {
        std::string what = "URI parse error at offset ";
        what += std::to_string(offset);
        what += ": ";
        what += to_string(code);
        return what;
    }

} // namespace catalyst::resource
