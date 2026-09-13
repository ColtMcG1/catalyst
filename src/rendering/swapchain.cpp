/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Public swapchain API; validates handles and arguments, then forwards to the active backend.
 */

#include <catalyst/rendering/swapchain.hpp>

#include "detail_backend.hpp"
#include "detail_sync.hpp"

namespace catalyst::rendering
{

    swapchain create_swapchain(const device &dev, const swapchain_desc &desc)
    {
        if (!dev || desc.extent.width == 0 || desc.extent.height == 0 || desc.image_count == 0)
            return {};
        if (desc.pixel_format == format::unknown || is_depth_format(desc.pixel_format))
            return {};
        const detail::exclusive_guard guard;
        return swapchain{detail::create_swapchain(dev.id(), desc)};
    }

    void destroy_swapchain(swapchain &sc) noexcept
    {
        if (!sc)
            return;
        {
            const detail::exclusive_guard guard;
            detail::destroy_swapchain(sc.id());
        }
        sc = swapchain{};
    }

    bool is_valid(const swapchain &sc) noexcept
    {
        if (!sc)
            return false;
        const detail::shared_guard guard;
        return detail::is_swapchain_valid(sc.id());
    }

    swapchain_desc get_swapchain_desc(const swapchain &sc) noexcept
    {
        if (!sc)
            return {};
        const detail::shared_guard guard;
        return detail::get_swapchain_desc(sc.id());
    }

    bool resize_swapchain(const swapchain &sc, extent2d extent)
    {
        if (!sc || extent.width == 0 || extent.height == 0)
            return false;
        const detail::exclusive_guard guard;
        return detail::resize_swapchain(sc.id(), extent);
    }

    texture acquire_next_image(const swapchain &sc)
    {
        if (!sc)
            return {};
        const detail::exclusive_guard guard;
        return texture{detail::acquire_next_image(sc.id())};
    }

    bool present(const swapchain &sc)
    {
        if (!sc)
            return false;
        const detail::exclusive_guard guard;
        return detail::present(sc.id());
    }

} // namespace catalyst::rendering
