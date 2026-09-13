/*
 * @file physics.hpp
 * @brief Main header for Catalyst Physics library, including all physics components.
 * @details This header serves as the primary include for the Catalyst Physics library, which provides a collection of physics utilities and types commonly used in game development and real-time applications. By including this single header, users can access all the functionality provided by the Catalyst Physics library without needing to include individual component headers. The library is designed to be efficient, easy to use, and compatible with modern C++ standards.
 * License: MIT (see LICENSE).
 */

#pragma once

/**
 * @namespace catalyst::physics
 * @brief The catalyst::physics namespace contains all the physics-related types and functions provided by the Catalyst Physics library. This includes various physics utilities, collision detection tools, rigid body dynamics, and more. By organizing all physics-related functionality within this namespace, we can avoid naming conflicts and provide a clear structure for users of the library to access the various physics tools they need for their applications. The Catalyst Physics library is designed to be efficient, easy to use, and compatible with modern C++ standards, making it a valuable resource for developers building real-time applications and games that require physics simulations.
 * @details The Catalyst Physics library provides a collection of types and functions that are commonly used in physics simulations. By including this header, users can access all the functionality provided by the Catalyst Physics library without needing to include individual component headers. This allows developers to focus on building their applications while still having access to powerful tools for managing physics simulations, performing collision detection, and interacting with various physics systems effectively.
 */
namespace catalyst::physics
{

    /**
     * @fn module_name
     * @brief Returns the name of this module as a string. This can be used for logging, debugging, or any situation where you want to identify the module by name.
     * @return A string literal representing the name of this module.
     */
    const char *module_name();

} // namespace catalyst::physics
