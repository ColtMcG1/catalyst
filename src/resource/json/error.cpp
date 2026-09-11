/**
 * @file error.cpp
 * @brief Renders a @ref catalyst::resource::json::parse_error as the sentence an error message wants.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/resource/json/error.hpp>

#include <string>

namespace catalyst::resource::json
{

    std::string parse_error::message() const
    {
        std::string what = "JSON parse error at offset ";
        what += std::to_string(offset);
        what += ": ";
        what += to_string(code);
        return what;
    }

} // namespace catalyst::resource::json
