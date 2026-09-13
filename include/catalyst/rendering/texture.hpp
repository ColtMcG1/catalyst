/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Textures (images sampled by shaders or drawn into as render targets) and samplers (the filtering / addressing
 * state used when a shader reads a texture).
 */

#pragma once

#include <catalyst/rendering/device.hpp>
#include <catalyst/rendering/types.hpp>

#include <cstddef>
#include <cstdint>
#include <span>

namespace catalyst::rendering
{

    /**
     * @enum texture_usage
     * @brief Bit flags declaring how a texture will be used. Combine with `|`.
     */
    enum class texture_usage : std::uint8_t
    {
        none = 0,
        sampled = 1u << 0,       ///< Read through a sampler in shaders.
        storage = 1u << 1,       ///< Read / written as a storage image in shaders.
        render_target = 1u << 2, ///< Colour attachment of a render pass.
        depth_stencil = 1u << 3, ///< Depth / stencil attachment of a render pass.
        transfer_src = 1u << 4,
        transfer_dst = 1u << 5,
    };

    template <>
    inline constexpr bool is_flags_enum_v<texture_usage> = true;

    enum class texture_dimension : std::uint8_t
    {
        texture_1d,
        texture_2d,
        texture_3d,
    };

    /**
     * @struct texture_desc
     * @brief Creation parameters for a texture.
     */
    struct texture_desc
    {
        texture_dimension dimension = texture_dimension::texture_2d;
        extent3d extent{};
        std::uint32_t mip_levels = 1;
        std::uint32_t array_layers = 1;
        std::uint32_t sample_count = 1;
        format pixel_format = format::rgba8_unorm;
        texture_usage usage = texture_usage::sampled;
        /** Optional label shown in graphics debuggers. Not owned; may be null. */
        const char *debug_name = nullptr;
    };

    struct texture_tag
    {
    };

    /**
     * @brief Handle to a texture. See `create_texture`. Swapchain back buffers are also exposed as textures
     * (see `acquire_next_image` in swapchain.hpp).
     */
    using texture = resource_handle<texture_tag>;

    /**
     * @brief Creates a texture, optionally uploading `initial_data` for mip 0 / layer 0 (tightly packed rows).
     * Returns an invalid handle for an invalid device, a zero extent or an `unknown` format.
     */
    [[nodiscard]] texture create_texture(const device &dev, const texture_desc &desc,
                                         std::span<const std::byte> initial_data = {});

    /**
     * @brief Releases the texture and resets `t` to an invalid handle. Must not be called on swapchain images.
     */
    void destroy_texture(texture &t) noexcept;

    [[nodiscard]] bool is_valid(const texture &t) noexcept;

    [[nodiscard]] texture_desc get_texture_desc(const texture &t) noexcept;

    // -----------------------------------------------------------------------------
    // Samplers
    // -----------------------------------------------------------------------------

    enum class filter_mode : std::uint8_t
    {
        nearest,
        linear,
    };

    enum class address_mode : std::uint8_t
    {
        repeat,
        mirrored_repeat,
        clamp_to_edge,
        clamp_to_border,
    };

    /**
     * @struct sampler_desc
     * @brief Creation parameters for a sampler.
     */
    struct sampler_desc
    {
        filter_mode min_filter = filter_mode::linear;
        filter_mode mag_filter = filter_mode::linear;
        filter_mode mip_filter = filter_mode::linear;
        address_mode address_u = address_mode::repeat;
        address_mode address_v = address_mode::repeat;
        address_mode address_w = address_mode::repeat;
        /** 1.0 disables anisotropic filtering. */
        float max_anisotropy = 1.0f;
        float mip_lod_bias = 0.0f;
        const char *debug_name = nullptr;
    };

    struct sampler_tag
    {
    };

    /**
     * @brief Handle to a sampler. See `create_sampler`.
     */
    using sampler = resource_handle<sampler_tag>;

    [[nodiscard]] sampler create_sampler(const device &dev, const sampler_desc &desc = {});

    void destroy_sampler(sampler &s) noexcept;

    [[nodiscard]] bool is_valid(const sampler &s) noexcept;

} // namespace catalyst::rendering
