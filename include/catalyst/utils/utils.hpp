/*
 * @file utils.hpp
 * @brief Main header for Catalyst Utils library, including all utility components.
 * @details This header serves as the primary include for the Catalyst Utils library, which provides a collection of utility functions and types commonly used in game development and real-time applications. By including this single header, users can access all the functionality provided by the Catalyst Utils library without needing to include individual component headers. The library is designed to be efficient, easy to use, and compatible with modern C++ standards.
 * License: MIT (see LICENSE).
 */

#pragma once

/**
 * @namespace catalyst::utils
 * @brief The catalyst::utils namespace contains all the utility types and functions provided by the Catalyst Utils library. This includes various helper functions, type traits, and other utilities that are commonly used in game development and real-time applications. By organizing all utility-related functionality within this namespace, we can avoid naming conflicts and provide a clear structure for users of the library to access the various tools they need for their projects. The Catalyst Utils library is designed to be efficient, easy to use, and compatible with modern C++ standards, making it a valuable resource for developers building complex systems.
 * @details The Catalyst Utils library provides a collection of utility functions and types that are commonly used in game development and real-time applications. By including the appropriate headers from the Catalyst Utils library, users can access all the functionality they need for their projects without having to include individual component headers. This allows developers to focus on building their applications while still having access to powerful tools for managing various aspects of their code effectively.
 */
namespace catalyst::utils
{

    /**
     * @fn module_name
     * @brief Returns the name of this module as a string. This can be used for logging, debugging, or any situation where you want to identify the module by name.
     * @return A string literal representing the name of this module.
     */
    const char *module_name();

} // namespace catalyst::utils
