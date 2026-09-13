/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Renders an `error` as the sentence a log line wants.
 */

#include <catalyst/rendering/error.hpp>
#include <catalyst/rendering/rendering.hpp>

#include <string>

namespace catalyst::rendering
{

    std::string error::message() const
    {
        std::string out;
        out.reserve(96);

        // The backend first, because "vulkan could not do this" and "d3d12 could not do this" are
        // different problems with different fixes, and the code alone does not say which build this
        // came from. Unlike audio there is no field to read: exactly one backend is compiled in.
        out += to_string(backend());
        out += ": ";
        out += name(code);

        // The driver's own account of it, when there is one. Either half may be missing: a failure
        // the module detected itself has neither, and a backend that mapped a result it has no name
        // for has the number but not the spelling.
        const bool has_operation = operation != nullptr;
        const bool has_result = backend_result != 0 || backend_result_name != nullptr;

        if (has_operation || has_result)
        {
            out += " (";

            if (has_operation)
            {
                out += operation;
                if (has_result)
                    out += " returned ";
            }

            if (has_result)
            {
                if (backend_result_name != nullptr)
                    out += backend_result_name;
                else
                    out += std::to_string(backend_result);
            }

            out += ')';
        }

        return out;
    }

} // namespace catalyst::rendering
