/**
 * @file resource.hpp
 * @brief Main header for Catalyst Resource library, including all resource components.
 * @details This header serves as the primary include for the Catalyst Resource library, which provides a collection of resource management utilities and types commonly used in game development and real-time applications. By including this single header, users can access all the functionality provided by the Catalyst Resource library without needing to include individual component headers. The library is designed to be efficient, easy to use, and compatible with modern C++ standards.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/resource/json/json.hpp>

/**
 * @namespace catalyst::resource
 * @brief The catalyst::resource namespace contains all the resource management types and functions provided by the Catalyst Resource library. This includes various utilities for managing resources such as textures, models, shaders, and other assets commonly used in game development and real-time applications. By organizing all resource-related functionality within this namespace, we can avoid naming conflicts and provide a clear structure for users of the library to access the various tools they need for managing resources in their projects. The Catalyst Resource library is designed to be efficient, easy to use, and compatible with modern C++ standards, making it a valuable resource for developers building complex systems that require effective resource management.
 * @details The Catalyst Resource library provides a collection of resource management utilities and types that are commonly used in game development and real-time applications. By including the appropriate headers from the Catalyst Resource library, users can access all the functionality they need for managing resources in their projects without having to include individual component headers. This allows developers to focus on building their applications while still having access to powerful tools for managing various aspects of their resources effectively.
 */
namespace catalyst::resource
{
    /**
     * @fn module_name
     * @brief Returns the name of this module as a string. This can be used for logging, debugging, or any situation where you want to identify the module by name.
     * @return A string literal representing the name of this module.
     */
    const char *module_name();

} // namespace catalyst::resource
