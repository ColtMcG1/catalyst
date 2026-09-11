/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Memory layer of the Vulkan backend: memory-type selection, one `VkDeviceMemory` allocation per resource
 * (sub-allocation is a later optimisation), persistent mapping of host-visible buffers and a reused staging buffer.
 */

#include "vulkan_backend.hpp"

#include <algorithm>
#include <cstring>

namespace catalyst::rendering::detail::vulkan
{

    bool find_memory_type(const device_state &dev, std::uint32_t type_bits, VkMemoryPropertyFlags required,
                          VkMemoryPropertyFlags preferred, std::uint32_t &out_index) noexcept
    {
        const VkPhysicalDeviceMemoryProperties &props = dev.memory_properties;
        const VkMemoryPropertyFlags passes[] = {required | preferred, required};
        for (VkMemoryPropertyFlags wanted : passes)
        {
            for (std::uint32_t i = 0; i < props.memoryTypeCount; ++i)
            {
                if ((type_bits & (1u << i)) == 0)
                    continue;
                if ((props.memoryTypes[i].propertyFlags & wanted) != wanted)
                    continue;
                out_index = i;
                return true;
            }
        }
        return false;
    }

    namespace
    {
        bool allocate(device_state &dev, const VkMemoryRequirements &req, VkMemoryPropertyFlags required,
                      VkMemoryPropertyFlags preferred, VkDeviceMemory &out_memory,
                      VkMemoryPropertyFlags &out_flags) noexcept
        {
            std::uint32_t index = 0;
            if (!find_memory_type(dev, req.memoryTypeBits, required, preferred, index))
                return false;

            VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
            info.allocationSize = req.size;
            info.memoryTypeIndex = index;

            const VkResult result = vkAllocateMemory(dev.device, &info, nullptr, &out_memory);
            if (result != VK_SUCCESS)
            {
                logging::error<detail::render_log>("vkAllocateMemory({} bytes, type {}) failed ({})",
                                                   req.size, index, result_string(result));
                return false;
            }
            out_flags = dev.memory_properties.memoryTypes[index].propertyFlags;
            return true;
        }
    } // namespace

    bool allocate_buffer_memory(device_state &dev, VkBuffer buffer, memory_access access, VkDeviceMemory &out_memory,
                                void *&out_mapped, bool &out_coherent) noexcept
    {
        VkMemoryRequirements req{};
        vkGetBufferMemoryRequirements(dev.device, buffer, &req);

        VkMemoryPropertyFlags required = 0;
        VkMemoryPropertyFlags preferred = 0;
        switch (access)
        {
        case memory_access::gpu_only:
            required = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            // On a unified-memory adapter the device-local heap is system RAM and a host-visible type covers it, so
            // asking for one turns write_buffer into a memcpy. Only a preference: discrete adapters expose a small
            // host-visible device-local window (the PCI BAR) that GPU-only resources must not exhaust.
            if (dev.unified_memory)
                preferred = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            break;
        case memory_access::cpu_to_gpu:
            required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            preferred = VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
            break;
        case memory_access::gpu_to_cpu:
            required = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
            preferred = VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
            break;
        }

        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkMemoryPropertyFlags flags = 0;
        if (!allocate(dev, req, required, preferred, memory, flags))
        {
            // Relax: any memory for GPU-only, any host-visible memory (we flush manually) otherwise.
            const VkMemoryPropertyFlags fallback =
                access == memory_access::gpu_only ? 0 : VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT;
            if (!allocate(dev, req, fallback, 0, memory, flags))
                return false;
        }

        VkResult result = vkBindBufferMemory(dev.device, buffer, memory, 0);
        if (result != VK_SUCCESS)
        {
            logging::error<detail::render_log>("vkBindBufferMemory failed ({})", result_string(result));
            vkFreeMemory(dev.device, memory, nullptr);
            return false;
        }

        out_mapped = nullptr;
        out_coherent = true;
        // Map on the property we actually got rather than on the requested access: a gpu_only allocation that landed
        // in host-visible memory is written directly, and one that did not still falls back to a staged copy.
        if (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)
        {
            result = vkMapMemory(dev.device, memory, 0, VK_WHOLE_SIZE, 0, &out_mapped);
            if (result != VK_SUCCESS)
            {
                logging::error<detail::render_log>("vkMapMemory failed ({})", result_string(result));
                vkFreeMemory(dev.device, memory, nullptr);
                return false;
            }
            out_coherent = (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) != 0;
        }

        out_memory = memory;
        return true;
    }

    bool allocate_image_memory(device_state &dev, VkImage image, VkDeviceMemory &out_memory) noexcept
    {
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(dev.device, image, &req);

        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkMemoryPropertyFlags flags = 0;
        if (!allocate(dev, req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0, memory, flags) &&
            !allocate(dev, req, 0, 0, memory, flags))
            return false;

        const VkResult result = vkBindImageMemory(dev.device, image, memory, 0);
        if (result != VK_SUCCESS)
        {
            logging::error<detail::render_log>("vkBindImageMemory failed ({})", result_string(result));
            vkFreeMemory(dev.device, memory, nullptr);
            return false;
        }
        out_memory = memory;
        return true;
    }

    void flush_host_writes(device_state &dev, VkDeviceMemory memory) noexcept
    {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkFlushMappedMemoryRanges(dev.device, 1, &range);
    }

    void invalidate_host_reads(device_state &dev, VkDeviceMemory memory) noexcept
    {
        VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
        range.memory = memory;
        range.offset = 0;
        range.size = VK_WHOLE_SIZE;
        vkInvalidateMappedMemoryRanges(dev.device, 1, &range);
    }

    namespace
    {
        /** Smallest staging allocation; sized so the common small upload never reallocates. */
        constexpr VkDeviceSize min_staging_bytes = 64u * 1024u;

        VkDeviceSize staging_capacity_for(VkDeviceSize size) noexcept
        {
            VkDeviceSize capacity = min_staging_bytes;
            while (capacity < size)
                capacity *= 2; // Geometric growth: a stream of growing uploads reallocates O(log n) times, not O(n).
            return capacity;
        }

        /** Grows the device staging buffer to hold at least `size` bytes. Existing contents are not preserved. */
        bool reserve_staging(device_state &dev, VkDeviceSize size) noexcept
        {
            if (dev.staging.buffer && dev.staging.capacity >= size)
                return true;

            // Nothing can still be reading the old buffer: every staged transfer waits on its submission before
            // returning, so the previous upload has completed by the time we get here.
            release_staging(dev);

            const VkDeviceSize capacity = staging_capacity_for(size);

            VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            info.size = capacity;
            info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

            staging_buffer staging;
            const VkResult result = vkCreateBuffer(dev.device, &info, nullptr, &staging.buffer);
            if (result != VK_SUCCESS)
            {
                logging::error<detail::render_log>("staging: vkCreateBuffer failed ({})", result_string(result));
                return false;
            }

            if (!allocate_buffer_memory(dev, staging.buffer, memory_access::cpu_to_gpu, staging.memory, staging.mapped,
                                        staging.coherent))
            {
                vkDestroyBuffer(dev.device, staging.buffer, nullptr);
                return false;
            }

            staging.capacity = capacity;
            dev.staging = staging;
            return true;
        }
    } // namespace

    bool stage_upload(device_state &dev, std::span<const std::byte> data, VkBuffer &out_source) noexcept
    {
        if (!reserve_staging(dev, std::max<VkDeviceSize>(data.size(), 1)))
            return false;

        if (!data.empty())
            std::memcpy(dev.staging.mapped, data.data(), data.size());
        if (!dev.staging.coherent)
            flush_host_writes(dev, dev.staging.memory);

        out_source = dev.staging.buffer;
        return true;
    }

    void release_staging(device_state &dev) noexcept
    {
        if (dev.staging.buffer)
            vkDestroyBuffer(dev.device, dev.staging.buffer, nullptr);
        if (dev.staging.memory)
            vkFreeMemory(dev.device, dev.staging.memory, nullptr); // Implicitly unmaps.
        dev.staging = {};
    }

} // namespace catalyst::rendering::detail::vulkan
