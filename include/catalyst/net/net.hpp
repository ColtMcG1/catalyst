/*
 * @file net.hpp
 * @brief Main header for Catalyst Net library, including all network components.
 * @details This header serves as the primary include for the Catalyst Net library, which provides a collection of networking utilities and types commonly used in game development and real-time applications. By including this single header, users can access all the functionality provided by the Catalyst Net library without needing to include individual component headers. The library is designed to be efficient, easy to use, and compatible with modern C++ standards.
 * License: MIT (see LICENSE).
 */

#pragma once

/**
 * @namespace catalyst::net
 * @brief The catalyst::net namespace contains all the network-related types and functions provided by the Catalyst Net library. This includes various networking utilities, socket management tools, and more. By organizing all network-related functionality within this namespace, we can avoid naming conflicts and provide a clear structure for users of the library to access the various networking tools they need for their applications. The Catalyst Net library is designed to be efficient, easy to use, and compatible with modern C++ standards, making it a valuable resource for developers building real-time applications and games that require networking capabilities.
 * @details The Catalyst Net library provides a collection of types and functions that are commonly used in networking applications. By including this header, users can access all the functionality provided by the Catalyst Net library without needing to include individual component headers. This allows developers to focus on building their applications while still having access to powerful tools for managing network connections, performing data transmission, and interacting with various networking systems effectively.
 */
namespace catalyst::net {

    /**
     * @fn module_name
     * @brief Returns the name of this module as a string. This can be used for logging, debugging, or any situation where you want to identify the module by name.
     * @return A string literal representing the name of this module.
     */
    const char* module_name();

} // namespace catalyst::net
