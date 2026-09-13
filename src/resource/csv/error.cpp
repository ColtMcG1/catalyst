/**
 * @file error.cpp
 * @brief Renders a @ref catalyst::resource::csv::parse_error as the sentence an error message wants.
 * License: MIT (see LICENSE).
 */

#include <catalyst/resource/csv/error.hpp>

#include <string>

namespace catalyst::resource::csv
{

    std::string parse_error::message() const
    {
        std::string what = "CSV parse error at line ";
        what += std::to_string(line);
        what += ", column ";
        what += std::to_string(column);
        what += " (offset ";
        what += std::to_string(offset);
        what += "): ";
        what += to_string(code);
        return what;
    }

} // namespace catalyst::resource::csv
