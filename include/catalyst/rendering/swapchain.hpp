/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Swapchains: the ring of presentable back buffers attached to a platform window.
 * @details The per-frame flow is `acquire_next_image` → record a render pass targeting the returned texture → `submit` →
 * `present`. The swapchain owns its images; never pass them to `destroy_texture`.
 */

#pragma once

#include <catalyst/platform/window.hpp>
#include <catalyst/rendering/device.hpp>
#include <catalyst/rendering/texture.hpp>
#include <catalyst/rendering/types.hpp>

#include <cstdint>

namespace catalyst::rendering
{

    /**
     * @struct swapchain_desc
     * @brief Creation parameters for a swapchain.
     */
    struct swapchain_desc
    {
        /** Native window to present into; obtain with `platform::get_native_handle`. */
        platform::native_handle window{};
        /** Back-buffer size in pixels; normally the window's client rect. */
        extent2d extent{};
        format pixel_format = format::bgra8_unorm_srgb;
        /** Number of back buffers; 2 = double buffering, 3 = triple. */
        std::uint32_t image_count = 3;
        bool vsync = true;
        const char *debug_name = nullptr;
    };

    struct swapchain_tag
    {
    };

    /**
     * @brief Handle to a swapchain. See `create_swapchain`.
     */
    using swapchain = resource_handle<swapchain_tag>;

    /**
     * @brief Creates a swapchain for `desc.window`. Returns an invalid handle for an invalid device, a zero extent or an
     * `image_count` of 0.
     */
    [[nodiscard]] swapchain create_swapchain(const device &dev, const swapchain_desc &desc);

    /**
     * @brief Destroys the swapchain and its images, then resets `sc` to an invalid handle.
     */
    void destroy_swapchain(swapchain &sc) noexcept;

    [[nodiscard]] bool is_valid(const swapchain &sc) noexcept;

    /** @brief The description the swapchain currently matches (extent reflects the last successful resize). */
    [[nodiscard]] swapchain_desc get_swapchain_desc(const swapchain &sc) noexcept;

    /**
     * @brief Recreates the back buffers at a new size, e.g. in response to `platform::window_resized_event`.
     * Textures previously returned by `acquire_next_image` become invalid.
     * @return False if the swapchain is invalid or the extent is zero.
     */
    bool resize_swapchain(const swapchain &sc, extent2d extent);

    /**
     * @brief Acquires the next back buffer as a render-target texture. Returns an invalid handle when no image is
     * available (window minimised, swapchain out of date); skip rendering that frame.
     */
    [[nodiscard]] texture acquire_next_image(const swapchain &sc);

    /**
     * @brief Presents the most recently acquired image. Work submitted before this call is guaranteed to finish first.
     * @return False if nothing was acquired or presentation failed.
     */
    bool present(const swapchain &sc);

} // namespace catalyst::rendering
