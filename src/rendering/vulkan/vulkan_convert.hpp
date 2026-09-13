/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Conversions from the public rendering enums to their Vulkan equivalents. Every function is a total mapping;
 * unknown inputs fall back to a safe default rather than asserting. Not part of the public API.
 */

#pragma once

#include "vulkan_backend.hpp"

namespace catalyst::rendering::detail::vulkan
{

    // -------------------------------------------------------------------------
    // Formats
    // -------------------------------------------------------------------------

    [[nodiscard]] constexpr VkFormat to_vk_format(format f) noexcept
    {
        switch (f)
        {
        case format::unknown:           return VK_FORMAT_UNDEFINED;
        case format::r8_unorm:          return VK_FORMAT_R8_UNORM;
        case format::rg8_unorm:         return VK_FORMAT_R8G8_UNORM;
        case format::rgba8_unorm:       return VK_FORMAT_R8G8B8A8_UNORM;
        case format::rgba8_unorm_srgb:  return VK_FORMAT_R8G8B8A8_SRGB;
        case format::bgra8_unorm:       return VK_FORMAT_B8G8R8A8_UNORM;
        case format::bgra8_unorm_srgb:  return VK_FORMAT_B8G8R8A8_SRGB;
        case format::r16_float:         return VK_FORMAT_R16_SFLOAT;
        case format::rg16_float:        return VK_FORMAT_R16G16_SFLOAT;
        case format::rgba16_float:      return VK_FORMAT_R16G16B16A16_SFLOAT;
        case format::r16_uint:          return VK_FORMAT_R16_UINT;
        case format::r32_uint:          return VK_FORMAT_R32_UINT;
        case format::r32_sint:          return VK_FORMAT_R32_SINT;
        case format::r32_float:         return VK_FORMAT_R32_SFLOAT;
        case format::rg32_float:        return VK_FORMAT_R32G32_SFLOAT;
        case format::rgb32_float:       return VK_FORMAT_R32G32B32_SFLOAT;
        case format::rgba32_float:      return VK_FORMAT_R32G32B32A32_SFLOAT;
        case format::d16_unorm:         return VK_FORMAT_D16_UNORM;
        case format::d32_float:         return VK_FORMAT_D32_SFLOAT;
        case format::d24_unorm_s8_uint: return VK_FORMAT_D24_UNORM_S8_UINT;
        case format::d32_float_s8_uint: return VK_FORMAT_D32_SFLOAT_S8_UINT;

        // Block-compressed. `bc1_rgba_*` maps to Vulkan's RGBA spelling rather than its RGB one: the two encode
        // identical blocks and differ only in the alpha a sampler is promised, so the RGBA form is correct for both and
        // the RGB form is the one that would lose information.
        case format::bc1_rgba_unorm:      return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case format::bc1_rgba_unorm_srgb: return VK_FORMAT_BC1_RGBA_SRGB_BLOCK;
        case format::bc2_unorm:           return VK_FORMAT_BC2_UNORM_BLOCK;
        case format::bc2_unorm_srgb:      return VK_FORMAT_BC2_SRGB_BLOCK;
        case format::bc3_unorm:           return VK_FORMAT_BC3_UNORM_BLOCK;
        case format::bc3_unorm_srgb:      return VK_FORMAT_BC3_SRGB_BLOCK;
        case format::bc4_unorm:           return VK_FORMAT_BC4_UNORM_BLOCK;
        case format::bc4_snorm:           return VK_FORMAT_BC4_SNORM_BLOCK;
        case format::bc5_unorm:           return VK_FORMAT_BC5_UNORM_BLOCK;
        case format::bc5_snorm:           return VK_FORMAT_BC5_SNORM_BLOCK;
        case format::bc6h_ufloat:         return VK_FORMAT_BC6H_UFLOAT_BLOCK;
        case format::bc6h_sfloat:         return VK_FORMAT_BC6H_SFLOAT_BLOCK;
        case format::bc7_unorm:           return VK_FORMAT_BC7_UNORM_BLOCK;
        case format::bc7_unorm_srgb:      return VK_FORMAT_BC7_SRGB_BLOCK;
        }
        return VK_FORMAT_UNDEFINED;
    }

    /** Inverse of `to_vk_format` for the formats a surface may report; `format::unknown` otherwise. */
    [[nodiscard]] constexpr format from_vk_format(VkFormat f) noexcept
    {
        switch (f)
        {
        case VK_FORMAT_R8_UNORM:            return format::r8_unorm;
        case VK_FORMAT_R8G8_UNORM:          return format::rg8_unorm;
        case VK_FORMAT_R8G8B8A8_UNORM:      return format::rgba8_unorm;
        case VK_FORMAT_R8G8B8A8_SRGB:       return format::rgba8_unorm_srgb;
        case VK_FORMAT_B8G8R8A8_UNORM:      return format::bgra8_unorm;
        case VK_FORMAT_B8G8R8A8_SRGB:       return format::bgra8_unorm_srgb;
        case VK_FORMAT_R16_SFLOAT:          return format::r16_float;
        case VK_FORMAT_R16G16_SFLOAT:       return format::rg16_float;
        case VK_FORMAT_R16G16B16A16_SFLOAT: return format::rgba16_float;
        case VK_FORMAT_R16_UINT:            return format::r16_uint;
        case VK_FORMAT_R32_UINT:            return format::r32_uint;
        case VK_FORMAT_R32_SINT:            return format::r32_sint;
        case VK_FORMAT_R32_SFLOAT:          return format::r32_float;
        case VK_FORMAT_R32G32_SFLOAT:       return format::rg32_float;
        case VK_FORMAT_R32G32B32_SFLOAT:    return format::rgb32_float;
        case VK_FORMAT_R32G32B32A32_SFLOAT: return format::rgba32_float;
        case VK_FORMAT_D16_UNORM:           return format::d16_unorm;
        case VK_FORMAT_D32_SFLOAT:          return format::d32_float;
        case VK_FORMAT_D24_UNORM_S8_UINT:   return format::d24_unorm_s8_uint;
        case VK_FORMAT_D32_SFLOAT_S8_UINT:  return format::d32_float_s8_uint;
        default:                            return format::unknown;
        }
    }

    [[nodiscard]] constexpr VkImageAspectFlags aspect_mask(format f) noexcept
    {
        if (!is_depth_format(f))
            return VK_IMAGE_ASPECT_COLOR_BIT;
        VkImageAspectFlags aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
        if (is_stencil_format(f))
            aspect |= VK_IMAGE_ASPECT_STENCIL_BIT;
        return aspect;
    }

    [[nodiscard]] constexpr VkIndexType to_vk_index_type(index_type t) noexcept
    {
        return t == index_type::uint16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    }

    // -------------------------------------------------------------------------
    // Buffers and textures
    // -------------------------------------------------------------------------

    [[nodiscard]] constexpr VkBufferUsageFlags to_vk_buffer_usage(buffer_usage usage) noexcept
    {
        // Every buffer can be a transfer destination so `write_buffer` works on GPU-only memory.
        VkBufferUsageFlags flags = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        if (has_flag(usage, buffer_usage::vertex))
            flags |= VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
        if (has_flag(usage, buffer_usage::index))
            flags |= VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
        if (has_flag(usage, buffer_usage::uniform))
            flags |= VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
        if (has_flag(usage, buffer_usage::storage))
            flags |= VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
        if (has_flag(usage, buffer_usage::indirect))
            flags |= VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT;
        if (has_flag(usage, buffer_usage::transfer_src))
            flags |= VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        return flags;
    }

    [[nodiscard]] constexpr VkImageUsageFlags to_vk_image_usage(texture_usage usage) noexcept
    {
        // Every image can be a transfer destination so initial data can be uploaded.
        VkImageUsageFlags flags = VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (has_flag(usage, texture_usage::sampled))
            flags |= VK_IMAGE_USAGE_SAMPLED_BIT;
        if (has_flag(usage, texture_usage::storage))
            flags |= VK_IMAGE_USAGE_STORAGE_BIT;
        if (has_flag(usage, texture_usage::render_target))
            flags |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        if (has_flag(usage, texture_usage::depth_stencil))
            flags |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
        if (has_flag(usage, texture_usage::transfer_src))
            flags |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
        return flags;
    }

    [[nodiscard]] constexpr VkImageType to_vk_image_type(texture_dimension d) noexcept
    {
        switch (d)
        {
        case texture_dimension::texture_1d: return VK_IMAGE_TYPE_1D;
        case texture_dimension::texture_2d: return VK_IMAGE_TYPE_2D;
        case texture_dimension::texture_3d: return VK_IMAGE_TYPE_3D;
        }
        return VK_IMAGE_TYPE_2D;
    }

    [[nodiscard]] constexpr VkImageViewType to_vk_image_view_type(texture_dimension d, std::uint32_t layers) noexcept
    {
        switch (d)
        {
        case texture_dimension::texture_1d: return layers > 1 ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        case texture_dimension::texture_2d: return layers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
        case texture_dimension::texture_3d: return VK_IMAGE_VIEW_TYPE_3D;
        }
        return VK_IMAGE_VIEW_TYPE_2D;
    }

    [[nodiscard]] constexpr VkSampleCountFlagBits to_vk_sample_count(std::uint32_t count) noexcept
    {
        switch (count)
        {
        case 2:  return VK_SAMPLE_COUNT_2_BIT;
        case 4:  return VK_SAMPLE_COUNT_4_BIT;
        case 8:  return VK_SAMPLE_COUNT_8_BIT;
        case 16: return VK_SAMPLE_COUNT_16_BIT;
        case 32: return VK_SAMPLE_COUNT_32_BIT;
        case 64: return VK_SAMPLE_COUNT_64_BIT;
        default: return VK_SAMPLE_COUNT_1_BIT;
        }
    }

    // -------------------------------------------------------------------------
    // Samplers
    // -------------------------------------------------------------------------

    [[nodiscard]] constexpr VkFilter to_vk_filter(filter_mode m) noexcept
    {
        return m == filter_mode::nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
    }

    [[nodiscard]] constexpr VkSamplerMipmapMode to_vk_mipmap_mode(filter_mode m) noexcept
    {
        return m == filter_mode::nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST : VK_SAMPLER_MIPMAP_MODE_LINEAR;
    }

    [[nodiscard]] constexpr VkSamplerAddressMode to_vk_address_mode(address_mode m) noexcept
    {
        switch (m)
        {
        case address_mode::repeat:          return VK_SAMPLER_ADDRESS_MODE_REPEAT;
        case address_mode::mirrored_repeat: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case address_mode::clamp_to_edge:   return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case address_mode::clamp_to_border: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        }
        return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }

    // -------------------------------------------------------------------------
    // Pipelines
    // -------------------------------------------------------------------------

    [[nodiscard]] constexpr VkShaderStageFlagBits to_vk_shader_stage(shader_stage s) noexcept
    {
        switch (s)
        {
        case shader_stage::vertex:   return VK_SHADER_STAGE_VERTEX_BIT;
        case shader_stage::fragment: return VK_SHADER_STAGE_FRAGMENT_BIT;
        case shader_stage::compute:  return VK_SHADER_STAGE_COMPUTE_BIT;
        }
        return VK_SHADER_STAGE_VERTEX_BIT;
    }

    [[nodiscard]] constexpr VkPrimitiveTopology to_vk_topology(primitive_topology t) noexcept
    {
        switch (t)
        {
        case primitive_topology::point_list:     return VK_PRIMITIVE_TOPOLOGY_POINT_LIST;
        case primitive_topology::line_list:      return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case primitive_topology::line_strip:     return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case primitive_topology::triangle_list:  return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        case primitive_topology::triangle_strip: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        }
        return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }

    [[nodiscard]] constexpr VkPolygonMode to_vk_polygon_mode(fill_mode m) noexcept
    {
        return m == fill_mode::wireframe ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    }

    [[nodiscard]] constexpr VkCullModeFlags to_vk_cull_mode(cull_mode m) noexcept
    {
        switch (m)
        {
        case cull_mode::none:  return VK_CULL_MODE_NONE;
        case cull_mode::front: return VK_CULL_MODE_FRONT_BIT;
        case cull_mode::back:  return VK_CULL_MODE_BACK_BIT;
        }
        return VK_CULL_MODE_NONE;
    }

    /**
     * The backend flips the viewport (negative height) so clip-space +Y points up as on D3D12 / Metal; with that flip
     * Vulkan's winding matches the public convention directly.
     */
    [[nodiscard]] constexpr VkFrontFace to_vk_front_face(front_face f) noexcept
    {
        return f == front_face::clockwise ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
    }

    [[nodiscard]] constexpr VkCompareOp to_vk_compare_op(compare_op op) noexcept
    {
        switch (op)
        {
        case compare_op::never:         return VK_COMPARE_OP_NEVER;
        case compare_op::less:          return VK_COMPARE_OP_LESS;
        case compare_op::equal:         return VK_COMPARE_OP_EQUAL;
        case compare_op::less_equal:    return VK_COMPARE_OP_LESS_OR_EQUAL;
        case compare_op::greater:       return VK_COMPARE_OP_GREATER;
        case compare_op::not_equal:     return VK_COMPARE_OP_NOT_EQUAL;
        case compare_op::greater_equal: return VK_COMPARE_OP_GREATER_OR_EQUAL;
        case compare_op::always:        return VK_COMPARE_OP_ALWAYS;
        }
        return VK_COMPARE_OP_ALWAYS;
    }

    [[nodiscard]] constexpr VkBlendFactor to_vk_blend_factor(blend_factor f) noexcept
    {
        switch (f)
        {
        case blend_factor::zero:                     return VK_BLEND_FACTOR_ZERO;
        case blend_factor::one:                      return VK_BLEND_FACTOR_ONE;
        case blend_factor::src_color:                return VK_BLEND_FACTOR_SRC_COLOR;
        case blend_factor::one_minus_src_color:      return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case blend_factor::dst_color:                return VK_BLEND_FACTOR_DST_COLOR;
        case blend_factor::one_minus_dst_color:      return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case blend_factor::src_alpha:                return VK_BLEND_FACTOR_SRC_ALPHA;
        case blend_factor::one_minus_src_alpha:      return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case blend_factor::dst_alpha:                return VK_BLEND_FACTOR_DST_ALPHA;
        case blend_factor::one_minus_dst_alpha:      return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case blend_factor::constant_color:           return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case blend_factor::one_minus_constant_color: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        }
        return VK_BLEND_FACTOR_ONE;
    }

    [[nodiscard]] constexpr VkBlendOp to_vk_blend_op(blend_op op) noexcept
    {
        switch (op)
        {
        case blend_op::add:              return VK_BLEND_OP_ADD;
        case blend_op::subtract:         return VK_BLEND_OP_SUBTRACT;
        case blend_op::reverse_subtract: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case blend_op::min:              return VK_BLEND_OP_MIN;
        case blend_op::max:              return VK_BLEND_OP_MAX;
        }
        return VK_BLEND_OP_ADD;
    }

    [[nodiscard]] constexpr VkColorComponentFlags to_vk_color_mask(color_write_mask m) noexcept
    {
        VkColorComponentFlags flags = 0;
        if (has_flag(m, color_write_mask::r))
            flags |= VK_COLOR_COMPONENT_R_BIT;
        if (has_flag(m, color_write_mask::g))
            flags |= VK_COLOR_COMPONENT_G_BIT;
        if (has_flag(m, color_write_mask::b))
            flags |= VK_COLOR_COMPONENT_B_BIT;
        if (has_flag(m, color_write_mask::a))
            flags |= VK_COLOR_COMPONENT_A_BIT;
        return flags;
    }

    [[nodiscard]] constexpr VkVertexInputRate to_vk_input_rate(vertex_input_rate r) noexcept
    {
        return r == vertex_input_rate::per_instance ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
    }

    // -------------------------------------------------------------------------
    // Render passes
    // -------------------------------------------------------------------------

    [[nodiscard]] constexpr VkAttachmentLoadOp to_vk_load_op(load_op op) noexcept
    {
        switch (op)
        {
        case load_op::load:      return VK_ATTACHMENT_LOAD_OP_LOAD;
        case load_op::clear:     return VK_ATTACHMENT_LOAD_OP_CLEAR;
        case load_op::dont_care: return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        }
        return VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    }

    [[nodiscard]] constexpr VkAttachmentStoreOp to_vk_store_op(store_op op) noexcept
    {
        return op == store_op::store ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
    }

} // namespace catalyst::rendering::detail::vulkan
