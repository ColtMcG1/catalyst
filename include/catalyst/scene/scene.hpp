/*
 * @file scene.hpp
 * @brief Main header for Catalyst Scene library, including all scene components.
 * @details This header serves as the primary include for the Catalyst Scene library, which provides a collection of types and functions commonly used in scene management and rendering. By including this single header, users can access all the functionality provided by the Catalyst Scene library without needing to include individual component headers. The library is designed to be efficient, easy to use, and compatible with modern C++ standards.
 * License: MIT (see LICENSE).
 */

#pragma once

/**
 * @namespace catalyst::scene
 * @brief The catalyst::scene namespace contains all the types and functions related to scene management and rendering. This includes various components such as scene graphs, renderable objects, and other utilities commonly used in scene management. By organizing all scene-related functionality within this namespace, we can avoid naming conflicts and provide a clear structure for users of the library to access the various tools they need for building scenes in their applications.
 * @details The Catalyst Scene library provides a collection of types and functions that are commonly used in scene management and rendering. By including this header, users can access all the functionality provided by the Catalyst Scene library without needing to include individual component headers. This allows developers to focus on building their scenes while still having access to powerful tools for managing and rendering scenes effectively.
 */
namespace catalyst::scene
{

    /**
     * @fn module_name
     * @brief Returns the name of this module as a string. This can be used for logging, debugging, or any situation where you want to identify the module by name.
     * @return A string literal representing the name of this module.
     */
    const char *module_name();

} // namespace catalyst::scene
