/**
 * @file input.cpp
 * @brief Module-level entry points. Everything that used to be a global here now lives on input::context.
 * License: MIT (see LICENSE).
 */

#include <catalyst/input/input.hpp>

#include "detail_backend.hpp"

namespace catalyst::input
{

    const char *module_name()
    {
        return detail::backend_name();
    }

} // namespace catalyst::input
