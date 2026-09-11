/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Buffer resources of the Vulkan backend. Host-visible buffers stay persistently mapped so `write_buffer` and
 * `read_buffer` are memcpys; GPU-only buffers that could not be mapped are written through the staging ring and a copy
 * submitted on the copy queue.
 * @details **What Tier 4 changed here.** `write_buffer` on GPU-only memory used to be a `vkQueueSubmit` followed by
 * `vkWaitForFences(..., UINT64_MAX)`: one buffer write in the middle of a frame drained the queue. It now stages into
 * the ring and returns, and the ordering that the wait used to provide comes from the copy queue's timeline instead -
 * every later submission on another queue waits on the transfer, on the GPU. See vulkan_transfer.cpp.
 *
 * `read_buffer` still blocks, and still waits for every queue, because it hands back bytes and so has nowhere to put
 * the waiting. `download` in transfer.hpp is the form that does not.
 */

#include "vulkan_backend.hpp"
#include "vulkan_convert.hpp"

#include <cstring>

namespace catalyst::rendering::detail::vulkan
{

    void release_buffer_objects(device_state &dev, buffer_state &b) noexcept
    {
        if (b.buffer)
            vkDestroyBuffer(dev.device, b.buffer, nullptr);
        if (b.memory)
            vkFreeMemory(dev.device, b.memory, nullptr);
        b.buffer = VK_NULL_HANDLE;
        b.memory = VK_NULL_HANDLE;
        b.mapped = nullptr;
    }

    namespace
    {
        /** Stages `data` and submits the copy on the copy queue. Does not wait for it. */
        bool upload_via_staging(device_state &dev, resource_id device_id, VkBuffer dst, VkDeviceSize offset,
                                std::span<const std::byte> data, const creation_marks &created) noexcept
        {
            const auto point = transfer_once(dev, device_id, data, created,
                                             [&](VkCommandBuffer cmd, VkBuffer source, VkDeviceSize source_offset) {
                                                 VkBufferCopy region{};
                                                 region.srcOffset = source_offset;
                                                 region.dstOffset = offset;
                                                 region.size = data.size();
                                                 vkCmdCopyBuffer(cmd, source, dst, 1, &region);
                                             });
            if (!point)
            {
                logging::error<detail::render_log>("buffer upload of {} bytes failed: {}", data.size(),
                                                   point.error());
                return false;
            }
            return true;
        }
    } // namespace

} // namespace catalyst::rendering::detail::vulkan

namespace catalyst::rendering::detail
{

    using namespace vulkan;

    resource_id create_buffer(resource_id device, const buffer_desc &desc, std::span<const std::byte> initial_data)
    {
        device_state *dev = find_device(device);
        if (!dev)
            return 0;

        buffer_state b;
        b.owner = device;
        b.desc = desc;
        b.desc.debug_name = nullptr;
        b.debug_name = copy_name(desc.debug_name);
        // Taken before anything is uploaded: nothing submitted at or before these values can be
        // reading memory that does not exist yet, so the upload below needs no ordering against it.
        b.created = marks_now(*dev);

        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = desc.size_bytes;
        info.usage = to_vk_buffer_usage(desc.usage);
        apply_sharing(*dev, info);

        const VkResult result = vkCreateBuffer(dev->device, &info, nullptr, &b.buffer);
        if (result != VK_SUCCESS)
        {
            logging::error<detail::render_log>("create_buffer: vkCreateBuffer failed ({})", result_string(result));
            return 0;
        }

        if (!allocate_buffer_memory(*dev, b.buffer, desc.access, b.memory, b.mapped, b.coherent))
        {
            release_buffer_objects(*dev, b);
            return 0;
        }

        if (!initial_data.empty())
        {
            if (b.mapped)
            {
                std::memcpy(b.mapped, initial_data.data(), initial_data.size());
                if (!b.coherent)
                    flush_host_writes(*dev, b.memory);
            }
            else if (!upload_via_staging(*dev, device, b.buffer, 0, initial_data, b.created))
            {
                release_buffer_objects(*dev, b);
                return 0;
            }
        }

        set_debug_name(*dev, VK_OBJECT_TYPE_BUFFER, handle_bits(b.buffer), b.debug_name);

        const resource_id id = allocate_id();
        reg().buffers.emplace(id, std::move(b));
        return id;
    }

    void destroy_buffer(resource_id id) noexcept
    {
        buffer_state *b = find(reg().buffers, id);
        if (!b)
            return;

        if (device_state *dev = find_device(b->owner))
        {
            const VkDevice d = dev->device;
            const VkBuffer buffer = b->buffer;
            const VkDeviceMemory memory = b->memory;
            defer_release(*dev, [d, buffer, memory] {
                vkDestroyBuffer(d, buffer, nullptr);
                vkFreeMemory(d, memory, nullptr);
            });
        }
        reg().buffers.erase(id);
    }

    bool is_buffer_valid(resource_id id) noexcept
    {
        return find(reg().buffers, id) != nullptr;
    }

    buffer_desc get_buffer_desc(resource_id id) noexcept
    {
        const buffer_state *b = find(reg().buffers, id);
        if (!b)
            return {};
        buffer_desc d = b->desc;
        d.debug_name = name_or_null(b->debug_name);
        return d;
    }

    bool write_buffer(resource_id id, std::size_t offset, std::span<const std::byte> data) noexcept
    {
        buffer_state *b = find(reg().buffers, id);
        if (!b || b->desc.access == memory_access::gpu_to_cpu)
            return false;
        if (!range_in_bounds(b->desc.size_bytes, offset, data.size()))
            return false;
        if (data.empty())
            return true;

        device_state *dev = find_device(b->owner);
        if (!dev)
            return false;

        if (b->mapped)
        {
            std::memcpy(static_cast<std::byte *>(b->mapped) + offset, data.data(), data.size());
            if (!b->coherent)
                flush_host_writes(*dev, b->memory);
            return true;
        }
        return upload_via_staging(*dev, b->owner, b->buffer, offset, data, b->created);
    }

    bool read_buffer(resource_id id, std::size_t offset, std::span<std::byte> out) noexcept
    {
        buffer_state *b = find(reg().buffers, id);
        if (!b || b->desc.access != memory_access::gpu_to_cpu || !b->mapped)
            return false;
        if (!range_in_bounds(b->desc.size_bytes, offset, out.size()))
            return false;
        if (out.empty())
            return true;

        device_state *dev = find_device(b->owner);
        if (!dev)
            return false;

        // The GPU writes, the CPU reads: make sure every submission issued so far has finished.
        // All three queues, because nothing here records which of them wrote the buffer. This is
        // the blocking form, kept deliberately; `download` is the one keyed on a point.
        for (std::size_t i = 0; i < queue_kind_count; ++i)
        {
            const auto kind = static_cast<queue_kind>(i);
            (void)wait_timeline(*dev, kind, queue_for(*dev, kind).last_submitted,
                                std::chrono::nanoseconds::max());
        }
        if (!b->coherent)
            invalidate_host_reads(*dev, b->memory);

        std::memcpy(out.data(), static_cast<const std::byte *>(b->mapped) + offset, out.size());
        return true;
    }

} // namespace catalyst::rendering::detail
