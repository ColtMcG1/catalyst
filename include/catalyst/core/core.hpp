/**
 * @file core.hpp
 * @brief Main header for the catalyst::core module.
 * License: MIT (see LICENSE).
 */

#pragma once

namespace catalyst::core
{

    /**
     * @fn module_name
     * @brief Returns the name of this module as a string. This can be used for logging, debugging, or any situation where you want to identify the module by name.
     * @return A string literal representing the name of this module.
     */
    const char *module_name();

} // namespace catalyst::core
