/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Public shader API; validates handles and arguments, then forwards to the active backend.
 */

#include <catalyst/rendering/shader.hpp>

#include "detail_backend.hpp"
#include "detail_sync.hpp"

namespace catalyst::rendering
{

    shader create_shader(const device &dev, const shader_desc &desc)
    {
        if (!dev || desc.bytecode.empty())
            return {};
        const detail::exclusive_guard guard;
        return shader{detail::create_shader(dev.id(), desc)};
    }

    void destroy_shader(shader &s) noexcept
    {
        if (!s)
            return;
        {
            const detail::exclusive_guard guard;
            detail::destroy_shader(s.id());
        }
        s = shader{};
    }

    bool is_valid(const shader &s) noexcept
    {
        if (!s)
            return false;
        const detail::shared_guard guard;
        return detail::is_shader_valid(s.id());
    }

    std::span<const std::byte> get_bytecode(const shader &s) noexcept
    {
        if (!s)
            return {};
        const detail::shared_guard guard;
        return detail::get_bytecode(s.id());
    }

    shader_stage get_shader_stage(const shader &s) noexcept
    {
        if (!s)
            return shader_stage::vertex;
        const detail::shared_guard guard;
        return detail::get_shader_stage(s.id());
    }

} // namespace catalyst::rendering
