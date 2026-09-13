/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Textures and samplers of the Vulkan backend. User textures are created in, and stay in,
 * `VK_IMAGE_LAYOUT_GENERAL`; swapchain images are registered here too so command lists can treat every attachment
 * uniformly.
 */

#include "vulkan_backend.hpp"
#include "vulkan_convert.hpp"

#include <algorithm>

namespace catalyst::rendering::detail::vulkan
{

    void release_texture_objects(device_state &dev, texture_state &t) noexcept
    {
        if (t.sampled_view && t.sampled_view != t.view)
            vkDestroyImageView(dev.device, t.sampled_view, nullptr);
        if (t.view)
            vkDestroyImageView(dev.device, t.view, nullptr);
        if (!t.presentable)
        {
            if (t.image)
                vkDestroyImage(dev.device, t.image, nullptr);
            if (t.memory)
                vkFreeMemory(dev.device, t.memory, nullptr);
        }
        t.view = VK_NULL_HANDLE;
        t.sampled_view = VK_NULL_HANDLE;
        t.image = VK_NULL_HANDLE;
        t.memory = VK_NULL_HANDLE;
    }

    namespace
    {
        // mip0_bytes / image_extent / layer_count now live in vulkan_backend.hpp, because
        // vulkan_transfer.cpp needs them too.

        VkImageSubresourceRange full_range(const texture_state &t) noexcept
        {
            VkImageSubresourceRange range{};
            range.aspectMask = t.aspect;
            range.baseMipLevel = 0;
            range.levelCount = VK_REMAINING_MIP_LEVELS;
            range.baseArrayLayer = 0;
            range.layerCount = VK_REMAINING_ARRAY_LAYERS;
            return range;
        }

        bool create_views(device_state &dev, texture_state &t) noexcept
        {
            VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            info.image = t.image;
            info.viewType = to_vk_image_view_type(t.desc.dimension, layer_count(t.desc));
            info.format = t.vk_format;
            info.subresourceRange = full_range(t);

            VkResult result = vkCreateImageView(dev.device, &info, nullptr, &t.view);
            if (result != VK_SUCCESS)
            {
                logging::error<detail::render_log>("vkCreateImageView failed ({})", result_string(result));
                return false;
            }

            if (t.aspect & VK_IMAGE_ASPECT_STENCIL_BIT)
            {
                // Sampling a combined depth/stencil image requires a single-aspect view.
                info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                result = vkCreateImageView(dev.device, &info, nullptr, &t.sampled_view);
                if (result != VK_SUCCESS)
                {
                    logging::error<detail::render_log>("vkCreateImageView (depth aspect) failed ({})",
                                                       result_string(result));
                    return false;
                }
            }
            else
            {
                t.sampled_view = t.view;
            }
            return true;
        }

        resource_id register_texture(device_state &dev, texture_state &&t)
        {
            set_debug_name(dev, VK_OBJECT_TYPE_IMAGE, handle_bits(t.image), t.debug_name);
            set_debug_name(dev, VK_OBJECT_TYPE_IMAGE_VIEW, handle_bits(t.view), t.debug_name);
            const resource_id id = allocate_id();
            reg().textures.emplace(id, std::move(t));
            return id;
        }
    } // namespace

    resource_id create_texture_internal(resource_id device_id, const texture_desc &desc,
                                        std::span<const std::byte> initial_data, resource_id swapchain_id)
    {
        device_state *dev = find_device(device_id);
        if (!dev)
            return 0;

        texture_state t;
        t.owner = device_id;
        t.desc = desc;
        t.desc.pixel_format = resolve_format(*dev, desc.pixel_format);
        t.desc.debug_name = nullptr;
        t.debug_name = copy_name(desc.debug_name);
        t.swapchain = swapchain_id;
        t.vk_format = to_vk_format(t.desc.pixel_format);
        t.aspect = aspect_mask(t.desc.pixel_format);
        t.created = marks_now(*dev);

        if (!initial_data.empty() && initial_data.size() != mip0_bytes(t.desc))
            return 0;

        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = to_vk_image_type(t.desc.dimension);
        info.format = t.vk_format;
        info.extent = image_extent(t.desc);
        info.mipLevels = std::max(t.desc.mip_levels, 1u);
        info.arrayLayers = layer_count(t.desc);
        info.samples = to_vk_sample_count(t.desc.sample_count);
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = to_vk_image_usage(t.desc.usage);
        apply_sharing(*dev, info);
        info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        VkImageFormatProperties supported{};
        VkResult result = vkGetPhysicalDeviceImageFormatProperties(dev->physical_device, info.format, info.imageType,
                                                                   info.tiling, info.usage, info.flags, &supported);
        if (result != VK_SUCCESS)
        {
            logging::error<detail::render_log>(
                "create_texture: format {} is not supported with the requested usage ({})",
                static_cast<unsigned>(t.desc.pixel_format), result_string(result));
            return 0;
        }
        if ((supported.sampleCounts & info.samples) == 0)
        {
            logging::error<detail::render_log>("create_texture: sample count {} is not supported for this format",
                                               t.desc.sample_count);
            return 0;
        }

        result = vkCreateImage(dev->device, &info, nullptr, &t.image);
        if (result != VK_SUCCESS)
        {
            logging::error<detail::render_log>("create_texture: vkCreateImage failed ({})", result_string(result));
            return 0;
        }

        if (!allocate_image_memory(*dev, t.image, t.memory) || !create_views(*dev, t))
        {
            release_texture_objects(*dev, t);
            return 0;
        }

        // Move to GENERAL (the layout user textures live in) and upload mip 0 / layer 0 if
        // requested - both on the copy queue, in one submission that nothing waits for. The image
        // was created moments ago, so no other queue can be reading it and the transfer overlaps
        // whatever the frame is doing.
        //
        // The layout transition being on the copy queue is what `VK_SHARING_MODE_CONCURRENT` buys:
        // an exclusive image would need an ownership release here and an acquire on the graphics
        // queue, which nothing in this backend tracks.
        const auto uploaded = transfer_once(
            *dev, device_id, initial_data, t.created,
            [&](VkCommandBuffer cmd, VkBuffer source, VkDeviceSize source_offset) {
                VkImageMemoryBarrier to_general{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                to_general.srcAccessMask = 0;
                to_general.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                to_general.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                to_general.newLayout = VK_IMAGE_LAYOUT_GENERAL;
                to_general.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to_general.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                to_general.image = t.image;
                to_general.subresourceRange = full_range(t);
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                                     nullptr, 0, nullptr, 1, &to_general);

                if (initial_data.empty())
                    return;

                VkBufferImageCopy region{};
                region.bufferOffset = source_offset;
                region.imageSubresource.aspectMask =
                    (t.aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
                region.imageSubresource.mipLevel = 0;
                region.imageSubresource.baseArrayLayer = 0;
                region.imageSubresource.layerCount = 1;
                region.imageExtent = info.extent;
                vkCmdCopyBufferToImage(cmd, source, t.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
            });

        if (!uploaded)
        {
            logging::error<detail::render_log>("create_texture: initial upload failed: {}", uploaded.error());
            release_texture_objects(*dev, t);
            return 0;
        }

        return register_texture(*dev, std::move(t));
    }

    resource_id register_presentable_image(resource_id device_id, resource_id swapchain_id, std::uint32_t index,
                                           VkImage image, VkFormat vk_format, const texture_desc &desc,
                                           const std::string &debug_name)
    {
        device_state *dev = find_device(device_id);
        if (!dev)
            return 0;

        texture_state t;
        t.owner = device_id;
        t.desc = desc;
        t.desc.debug_name = nullptr;
        t.debug_name = debug_name;
        t.image = image;
        t.vk_format = vk_format;
        t.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
        t.swapchain = swapchain_id;
        t.presentable = true;
        t.image_index = index;

        if (!create_views(*dev, t))
        {
            release_texture_objects(*dev, t);
            return 0;
        }
        return register_texture(*dev, std::move(t));
    }

    void destroy_swapchain_texture(device_state &dev, resource_id texture_id) noexcept
    {
        texture_state *t = find(reg().textures, texture_id);
        if (!t)
            return;
        release_texture_objects(dev, *t);
        reg().textures.erase(texture_id);
    }

} // namespace catalyst::rendering::detail::vulkan

namespace catalyst::rendering::detail
{

    using namespace vulkan;

    resource_id create_texture(resource_id device, const texture_desc &desc, std::span<const std::byte> initial_data)
    {
        return create_texture_internal(device, desc, initial_data, 0);
    }

    void destroy_texture(resource_id id) noexcept
    {
        texture_state *t = find(reg().textures, id);
        if (!t || t->swapchain != 0)
            return; // Swapchain images are owned by their swapchain.

        if (device_state *dev = find_device(t->owner))
        {
            const VkDevice d = dev->device;
            const VkImageView view = t->view;
            const VkImageView sampled_view = t->sampled_view != t->view ? t->sampled_view : VK_NULL_HANDLE;
            const VkImage image = t->image;
            const VkDeviceMemory memory = t->memory;
            defer_release(*dev, [d, view, sampled_view, image, memory] {
                if (sampled_view)
                    vkDestroyImageView(d, sampled_view, nullptr);
                vkDestroyImageView(d, view, nullptr);
                vkDestroyImage(d, image, nullptr);
                vkFreeMemory(d, memory, nullptr);
            });
        }
        reg().textures.erase(id);
    }

    bool is_texture_valid(resource_id id) noexcept
    {
        return find(reg().textures, id) != nullptr;
    }

    texture_desc get_texture_desc(resource_id id) noexcept
    {
        const texture_state *t = find(reg().textures, id);
        if (!t)
            return {};
        texture_desc d = t->desc;
        d.debug_name = name_or_null(t->debug_name);
        return d;
    }

    // -------------------------------------------------------------------------
    // Samplers
    // -------------------------------------------------------------------------

    resource_id create_sampler(resource_id device, const sampler_desc &desc)
    {
        device_state *dev = find_device(device);
        if (!dev)
            return 0;

        VkSamplerCreateInfo info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        info.magFilter = to_vk_filter(desc.mag_filter);
        info.minFilter = to_vk_filter(desc.min_filter);
        info.mipmapMode = to_vk_mipmap_mode(desc.mip_filter);
        info.addressModeU = to_vk_address_mode(desc.address_u);
        info.addressModeV = to_vk_address_mode(desc.address_v);
        info.addressModeW = to_vk_address_mode(desc.address_w);
        info.mipLodBias = desc.mip_lod_bias;
        info.anisotropyEnable = (desc.max_anisotropy > 1.0f && dev->features.sampler_anisotropy) ? VK_TRUE : VK_FALSE;
        info.maxAnisotropy = std::min(desc.max_anisotropy, dev->properties.limits.maxSamplerAnisotropy);
        info.compareEnable = VK_FALSE;
        info.compareOp = VK_COMPARE_OP_ALWAYS;
        info.minLod = 0.0f;
        info.maxLod = VK_LOD_CLAMP_NONE;
        info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
        info.unnormalizedCoordinates = VK_FALSE;

        sampler_state s;
        s.owner = device;
        s.desc = desc;
        s.desc.debug_name = nullptr;

        const VkResult result = vkCreateSampler(dev->device, &info, nullptr, &s.sampler);
        if (result != VK_SUCCESS)
        {
            logging::error<detail::render_log>("create_sampler: vkCreateSampler failed ({})", result_string(result));
            return 0;
        }
        if (desc.debug_name)
            set_debug_name(*dev, VK_OBJECT_TYPE_SAMPLER, handle_bits(s.sampler), desc.debug_name);

        const resource_id id = allocate_id();
        reg().samplers.emplace(id, s);
        return id;
    }

    void destroy_sampler(resource_id id) noexcept
    {
        sampler_state *s = find(reg().samplers, id);
        if (!s)
            return;
        if (device_state *dev = find_device(s->owner))
        {
            const VkDevice d = dev->device;
            const VkSampler sampler = s->sampler;
            defer_release(*dev, [d, sampler] { vkDestroySampler(d, sampler, nullptr); });
        }
        reg().samplers.erase(id);
    }

    bool is_sampler_valid(resource_id id) noexcept
    {
        return find(reg().samplers, id) != nullptr;
    }

} // namespace catalyst::rendering::detail

namespace catalyst::rendering::detail::vulkan
{

    void release_sampler_objects(device_state &dev, sampler_state &s) noexcept
    {
        if (s.sampler)
            vkDestroySampler(dev.device, s.sampler, nullptr);
        s.sampler = VK_NULL_HANDLE;
    }

} // namespace catalyst::rendering::detail::vulkan
