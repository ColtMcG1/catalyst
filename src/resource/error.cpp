/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file error.cpp
 * @brief Definitions for catalyst::resource::error.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/resource/error.hpp>

namespace catalyst::resource
{

    std::string error::message() const
    {
        std::string out{to_string(code)};

        if (!uri.empty())
        {
            out += " [";
            out += uri;
            out += ']';
        }

        if (!detail.empty())
        {
            out += ": ";
            out += detail;
        }

        return out;
    }

    error make_error(error_code code, std::string_view uri, std::string_view detail)
    {
        return error{code, std::string{uri}, std::string{detail}};
    }

} // namespace catalyst::resource
