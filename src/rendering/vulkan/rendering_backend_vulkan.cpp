/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Identity of the Vulkan rendering backend and the storage behind its resource registry. The resource contract
 * itself is implemented per resource kind in the sibling vulkan_*.cpp files.
 */

#include "vulkan_backend.hpp"

namespace catalyst::rendering::detail
{

    const char *backend_name()
    {
        return "vulkan";
    }

    backend_kind backend_type() noexcept
    {
        return backend_kind::vulkan;
    }

} // namespace catalyst::rendering::detail

namespace catalyst::rendering::detail::vulkan
{

    registry &reg() noexcept
    {
        static registry instance;
        return instance;
    }

    resource_id allocate_id() noexcept
    {
        return reg().next_id++;
    }

} // namespace catalyst::rendering::detail::vulkan
