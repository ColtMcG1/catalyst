/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Public texture and sampler API; validates handles and arguments, then forwards to the active backend.
 */

#include <catalyst/rendering/texture.hpp>

#include "detail_backend.hpp"
#include "detail_sync.hpp"

namespace catalyst::rendering
{

    texture create_texture(const device &dev, const texture_desc &desc, std::span<const std::byte> initial_data)
    {
        if (!dev)
            return {};
        if (desc.extent.width == 0 || desc.extent.height == 0 || desc.extent.depth == 0)
            return {};
        if (desc.pixel_format == format::unknown || desc.mip_levels == 0 || desc.array_layers == 0 ||
            desc.sample_count == 0)
            return {};
        const detail::exclusive_guard guard;
        return texture{detail::create_texture(dev.id(), desc, initial_data)};
    }

    void destroy_texture(texture &t) noexcept
    {
        if (!t)
            return;
        {
            const detail::exclusive_guard guard;
            detail::destroy_texture(t.id());
        }
        t = texture{};
    }

    bool is_valid(const texture &t) noexcept
    {
        if (!t)
            return false;
        const detail::shared_guard guard;
        return detail::is_texture_valid(t.id());
    }

    texture_desc get_texture_desc(const texture &t) noexcept
    {
        if (!t)
            return {};
        const detail::shared_guard guard;
        return detail::get_texture_desc(t.id());
    }

    sampler create_sampler(const device &dev, const sampler_desc &desc)
    {
        if (!dev || desc.max_anisotropy < 1.0f)
            return {};
        const detail::exclusive_guard guard;
        return sampler{detail::create_sampler(dev.id(), desc)};
    }

    void destroy_sampler(sampler &s) noexcept
    {
        if (!s)
            return;
        {
            const detail::exclusive_guard guard;
            detail::destroy_sampler(s.id());
        }
        s = sampler{};
    }

    bool is_valid(const sampler &s) noexcept
    {
        if (!s)
            return false;
        const detail::shared_guard guard;
        return detail::is_sampler_valid(s.id());
    }

} // namespace catalyst::rendering
