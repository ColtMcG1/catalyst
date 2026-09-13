/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Tier 4 of the Vulkan backend: the staging ring, `transfer_batch`, and downloads.
 * @details What this replaces, in one sentence: `write_buffer` on GPU-only memory used to be
 * `vkQueueSubmit` followed by `vkWaitForFences(..., UINT64_MAX)`, so one buffer write in the middle
 * of a frame stalled the CPU until the GPU had drained everything ahead of it.
 *
 * The three pieces:
 *
 *   - **The ring.** One host-visible allocation, carved front to back with absolute counters, and
 *     recycled behind the copy queue's timeline. No `vkAllocateMemory` on the transfer path at all,
 *     and no growing: the budget is fixed at device creation so that running out is a value a
 *     caller can see (`error_code::staging_exhausted`) rather than a stall it cannot.
 *   - **The batch.** A command buffer from a small recycled pool, into which any number of copies
 *     are recorded, submitted once on the copy queue. Loading a level is one submission.
 *   - **The ordering.** A transfer runs on a queue with no order relative to the others, so two
 *     hazards have to be closed. Read-after-write - a draw that reads what was just uploaded - is
 *     closed by `device_state::last_transfer`, which every later submission on another queue waits
 *     on (see `submit_batch`). Write-after-read - an upload overwriting a buffer an earlier frame is
 *     still reading - is closed here, by waiting on the queues that could be reading it. Both waits
 *     are GPU-side; neither costs the calling thread anything.
 *
 * The write-after-read wait is skipped for a resource nothing has been submitted against since it
 * was created, which is exactly the streaming case: `create_buffer(..., initial_data)` and
 * `create_texture(..., initial_data)` upload into memory no queue has ever seen, so they overlap
 * with the frame completely.
 */

#include "vulkan_backend.hpp"
#include "vulkan_convert.hpp"

#include <algorithm>
#include <cstring>

namespace catalyst::rendering::detail::vulkan
{

    namespace
    {
        /** What `device_desc::staging_ring_bytes == 0` resolves to. */
        constexpr VkDeviceSize default_ring_bytes = 16ull * 1024ull * 1024ull;

        /** Small enough to be free, large enough that a caller who asked for a silly number gets something usable. */
        constexpr VkDeviceSize min_ring_bytes = 64ull * 1024ull;

        constexpr std::uint64_t align_up(std::uint64_t value, std::uint64_t alignment) noexcept
        {
            return (value + alignment - 1) & ~(alignment - 1);
        }

        /** How many transfer command buffers may be in flight before a batch has to wait for one. */
        constexpr std::size_t max_transfer_buffers = 16;
    } // namespace

    // -------------------------------------------------------------------------
    // The ring
    // -------------------------------------------------------------------------

    bool create_staging_ring(device_state &dev, std::uint64_t bytes) noexcept
    {
        VkDeviceSize capacity = bytes != 0 ? static_cast<VkDeviceSize>(bytes) : default_ring_bytes;
        capacity = std::max(capacity, min_ring_bytes);

        staging_ring ring;
        // Copy offsets have a device-specific alignment requirement, and the buffer-to-image path
        // has a stricter one still. Taking the largest of the three once means no per-copy branch.
        ring.alignment = std::max<VkDeviceSize>({16, dev.properties.limits.optimalBufferCopyOffsetAlignment,
                                                 dev.properties.limits.optimalBufferCopyRowPitchAlignment,
                                                 dev.properties.limits.nonCoherentAtomSize});
        capacity = align_up(capacity, ring.alignment);

        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = capacity;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE; // Only ever read by the copy queue.

        const VkResult result = vkCreateBuffer(dev.device, &info, nullptr, &ring.buffer);
        if (result != VK_SUCCESS)
        {
            logging::error<detail::render_log>("staging ring: vkCreateBuffer failed ({})", result_string(result));
            return false;
        }

        void *mapped = nullptr;
        if (!allocate_buffer_memory(dev, ring.buffer, memory_access::cpu_to_gpu, ring.memory, mapped, ring.coherent) ||
            mapped == nullptr)
        {
            vkDestroyBuffer(dev.device, ring.buffer, nullptr);
            logging::error<detail::render_log>("staging ring: {} MiB of host-visible memory could not be allocated",
                                               capacity / (1024 * 1024));
            return false;
        }

        ring.mapped = static_cast<std::byte *>(mapped);
        ring.capacity = capacity;
        dev.ring = ring;

        set_debug_name(dev, VK_OBJECT_TYPE_BUFFER, handle_bits(ring.buffer), "catalyst staging ring");
        logging::info<detail::render_log>("staging ring: {} KiB, {}-byte alignment", capacity / 1024, ring.alignment);
        return true;
    }

    void release_staging_ring(device_state &dev) noexcept
    {
        if (dev.ring.buffer)
            vkDestroyBuffer(dev.device, dev.ring.buffer, nullptr);
        if (dev.ring.memory)
            vkFreeMemory(dev.device, dev.ring.memory, nullptr); // Implicitly unmaps.
        dev.ring = {};
    }

    void retire_staging_blocks(device_state &dev) noexcept
    {
        staging_ring &ring = dev.ring;
        if (ring.in_flight.empty())
            return;

        refresh_completed(dev);
        const std::uint64_t completed = dev.lost ? UINT64_MAX : queue_for(dev, queue_kind::copy).completed;

        // A prefix, not a sweep: `tail` is a single boundary, so a block can only be released once
        // everything below it has been. Blocks are kept sorted by `end` to make that the common case.
        std::size_t retired = 0;
        while (retired < ring.in_flight.size() && ring.in_flight[retired].point <= completed)
        {
            ring.tail = std::max(ring.tail, ring.in_flight[retired].end);
            ++retired;
        }
        if (retired != 0)
            ring.in_flight.erase(ring.in_flight.begin(), ring.in_flight.begin() + static_cast<std::ptrdiff_t>(retired));
    }

    std::optional<VkDeviceSize> allocate_staging(device_state &dev, VkDeviceSize size, std::uint64_t &out_end) noexcept
    {
        staging_ring &ring = dev.ring;
        if (!ring.buffer || size == 0)
            return std::nullopt;

        const VkDeviceSize needed = align_up(size, ring.alignment);
        if (needed > ring.capacity)
            return std::nullopt; // Bigger than the whole ring: waiting would never help.

        retire_staging_blocks(dev);

        std::uint64_t start = align_up(ring.head, ring.alignment);
        VkDeviceSize offset = static_cast<VkDeviceSize>(start % ring.capacity);
        if (offset + needed > ring.capacity)
        {
            // A copy region has to be contiguous, so skip the tail of the buffer rather than wrap
            // across it. The skipped bytes are accounted for, and freed with everything else.
            start += ring.capacity - offset;
            offset = 0;
        }

        if (start + needed - ring.tail > ring.capacity)
            return std::nullopt; // Full of bytes the GPU has not read yet.

        ring.head = start + needed;
        out_end = ring.head;
        return offset;
    }

    creation_marks marks_now(const device_state &dev) noexcept
    {
        creation_marks marks{};
        for (std::size_t i = 0; i < queue_kind_count; ++i)
            marks[i] = dev.queues[i].last_submitted;
        return marks;
    }

    void retain_staging(device_state &dev, std::uint64_t end, std::uint64_t point)
    {
        staging_ring &ring = dev.ring;
        if (end <= ring.tail)
            return;

        const staging_ring::block entry{end, point};
        const auto at = std::lower_bound(ring.in_flight.begin(), ring.in_flight.end(), entry,
                                         [](const staging_ring::block &a, const staging_ring::block &b) {
                                             return a.end < b.end;
                                         });
        ring.in_flight.insert(at, entry);
    }

    // -------------------------------------------------------------------------
    // Transfer command buffers
    // -------------------------------------------------------------------------

    namespace
    {
        bool ensure_transfer_pool(device_state &dev) noexcept
        {
            if (dev.transfers.pool)
                return true;

            VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            // Per-buffer reset, not pool reset: several transfers are in flight at once and each
            // has to be recycled on its own schedule.
            info.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
            info.queueFamilyIndex = queue_for(dev, queue_kind::copy).family;

            const VkResult result = vkCreateCommandPool(dev.device, &info, nullptr, &dev.transfers.pool);
            if (result != VK_SUCCESS)
            {
                logging::error<detail::render_log>("transfers: vkCreateCommandPool failed ({})",
                                                   result_string(result));
                return false;
            }
            set_debug_name(dev, VK_OBJECT_TYPE_COMMAND_POOL, handle_bits(dev.transfers.pool),
                           "catalyst transfer pool");
            return true;
        }

        /** A free transfer command buffer, growing the pool if every one of them is still busy. */
        bool acquire_transfer_buffer(device_state &dev, std::size_t &out_index) noexcept
        {
            if (!ensure_transfer_pool(dev))
                return false;

            refresh_completed(dev);
            const std::uint64_t completed = queue_for(dev, queue_kind::copy).completed;

            for (std::size_t i = 0; i < dev.transfers.buffers.size(); ++i)
            {
                transfer_context::entry &entry = dev.transfers.buffers[i];
                if (!entry.busy && (entry.point == 0 || entry.point <= completed || dev.lost))
                {
                    entry.busy = true;
                    out_index = i;
                    return true;
                }
            }

            if (dev.transfers.buffers.size() >= max_transfer_buffers)
            {
                logging::warn<detail::render_log>("transfers: all {} transfer command buffers are still in flight",
                                                  max_transfer_buffers);
                return false;
            }

            VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            alloc.commandPool = dev.transfers.pool;
            alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc.commandBufferCount = 1;

            VkCommandBuffer cmd = VK_NULL_HANDLE;
            const VkResult result = vkAllocateCommandBuffers(dev.device, &alloc, &cmd);
            if (result != VK_SUCCESS)
            {
                logging::error<detail::render_log>("transfers: vkAllocateCommandBuffers failed ({})",
                                                   result_string(result));
                return false;
            }

            dev.transfers.buffers.push_back({cmd, 0, true});
            out_index = dev.transfers.buffers.size() - 1;
            return true;
        }
    } // namespace

    void release_transfer_context(device_state &dev) noexcept
    {
        if (dev.transfers.pool)
            vkDestroyCommandPool(dev.device, dev.transfers.pool, nullptr); // Frees its buffers too.
        dev.transfers = {};
    }

    void release_download_objects(device_state &dev, download_state &d) noexcept
    {
        if (d.buffer)
            vkDestroyBuffer(dev.device, d.buffer, nullptr);
        if (d.memory)
            vkFreeMemory(dev.device, d.memory, nullptr);
        d.buffer = VK_NULL_HANDLE;
        d.memory = VK_NULL_HANDLE;
        d.mapped = nullptr;
    }

    namespace
    {
        /**
         * Begins the batch's command buffer on first use. Deferred so that an empty batch - opened
         * and dropped without an upload - costs nothing at all.
         */
        bool ensure_recording(transfer_batch_state &batch) noexcept
        {
            if (batch.recording)
                return true;
            if (batch.cmd == VK_NULL_HANDLE)
                return false; // The last submission could not get a fresh command buffer.

            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            const VkResult result = vkBeginCommandBuffer(batch.cmd, &begin);
            if (result != VK_SUCCESS)
            {
                logging::error<detail::render_log>("transfer batch: vkBeginCommandBuffer failed ({})",
                                                   result_string(result));
                return false;
            }
            batch.recording = true;
            return true;
        }

        /** Folds a target's creation marks into the batch's write-after-read hazard set. */
        void note_hazard(device_state &dev, transfer_batch_state &batch, const creation_marks &created) noexcept
        {
            for (std::size_t i = 0; i < queue_kind_count; ++i)
            {
                // Nothing has been submitted to this queue since the resource was created, so
                // nothing on it can be reading the memory we are about to overwrite.
                if (dev.queues[i].last_submitted > created[i])
                    batch.hazard[i] = true;
            }
        }

        /** Copies `data` into the ring and hands back the source range for a copy command. */
        std::expected<VkDeviceSize, error> stage_into_ring(device_state &dev, transfer_batch_state &batch,
                                                           std::span<const std::byte> data) noexcept
        {
            const std::uint64_t before = dev.ring.head;

            std::uint64_t end = 0;
            const auto offset = allocate_staging(dev, data.size(), end);
            if (!offset)
                return std::unexpected(make_error(error_code::staging_exhausted, "transfer_batch::upload"));

            std::memcpy(dev.ring.mapped + *offset, data.data(), data.size());
            // The low-water mark is where the ring stood before the first allocation, alignment
            // padding included: `discard` rewinds to it, and giving back a few unused bytes too is
            // the harmless direction.
            if (batch.end_offset == 0)
                batch.begin_offset = before;
            batch.end_offset = end;
            batch.staged_bytes += data.size();
            return *offset;
        }
    } // namespace

    std::expected<std::uint64_t, error> transfer_once(
        device_state &dev, resource_id device_id, std::span<const std::byte> data, const creation_marks &created,
        const std::function<void(VkCommandBuffer, VkBuffer, VkDeviceSize)> &record) noexcept
    {
        if (dev.lost)
            return std::unexpected(make_error(error_code::device_lost, "transfer"));

        std::size_t entry = 0;
        if (!acquire_transfer_buffer(dev, entry))
            return std::unexpected(make_error(error_code::not_ready, "transfer"));
        const VkCommandBuffer cmd = dev.transfers.buffers[entry].cmd;

        // Everything below releases the command buffer on the way out; there is no path that
        // leaves it marked busy.
        const auto release_entry = [&](std::uint64_t point) {
            dev.transfers.buffers[entry].busy = false;
            dev.transfers.buffers[entry].point = point;
        };

        VkDeviceSize source_offset = 0;
        std::uint64_t ring_end = 0;
        if (!data.empty())
        {
            const auto offset = allocate_staging(dev, data.size(), ring_end);
            if (!offset)
            {
                release_entry(0);
                return std::unexpected(make_error(error_code::staging_exhausted, "transfer"));
            }
            source_offset = *offset;
            std::memcpy(dev.ring.mapped + source_offset, data.data(), data.size());
            if (!dev.ring.coherent)
                flush_host_writes(dev, dev.ring.memory);
        }

        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        VkResult result = vkBeginCommandBuffer(cmd, &begin);
        if (result != VK_SUCCESS)
        {
            release_entry(0);
            return std::unexpected(to_error(dev, result, "vkBeginCommandBuffer"));
        }

        record(cmd, dev.ring.buffer, source_offset);

        result = vkEndCommandBuffer(cmd);
        if (result != VK_SUCCESS)
        {
            release_entry(0);
            return std::unexpected(to_error(dev, result, "vkEndCommandBuffer"));
        }

        // Write-after-read: only against queues that have been given work since the destination was
        // created. For a resource created moments ago that is none of them, which is what lets a
        // level load overlap the frame instead of queueing behind it.
        timeline_point waits[queue_kind_count];
        std::size_t wait_count = 0;
        for (std::size_t i = 0; i < queue_kind_count; ++i)
        {
            const auto kind = static_cast<queue_kind>(i);
            if (kind == queue_kind::copy)
                continue;
            const std::uint64_t value = dev.queues[i].last_submitted;
            if (value > created[i])
                waits[wait_count++] = timeline_point{rendering::device{device_id}, kind, value};
        }

        const VkCommandBuffer commands[] = {cmd};
        const auto value = submit_batch(dev, {.kind = queue_kind::copy,
                                              .commands = commands,
                                              .timeline_waits = std::span{waits, wait_count}});
        if (!value)
        {
            release_entry(queue_for(dev, queue_kind::copy).last_submitted);
            return std::unexpected(value.error());
        }

        release_entry(*value);
        if (ring_end != 0)
            retain_staging(dev, ring_end, *value);
        dev.last_transfer = std::max(dev.last_transfer, *value);
        return *value;
    }

} // namespace catalyst::rendering::detail::vulkan

namespace catalyst::rendering::detail
{

    using namespace vulkan;

    staging_info get_staging_info(resource_id id) noexcept
    {
        staging_info info;
        device_state *dev = find_device(id);
        if (!dev)
            return info;

        retire_staging_blocks(*dev);
        info.capacity_bytes = dev->ring.capacity;
        info.in_use_bytes = dev->ring.head - dev->ring.tail;
        info.largest_transfer_bytes = dev->ring.capacity;
        return info;
    }

    // -------------------------------------------------------------------------
    // Batches
    // -------------------------------------------------------------------------

    resource_id begin_transfer_batch(resource_id device)
    {
        device_state *dev = find_device(device);
        if (!dev || dev->lost || !dev->ring.buffer)
            return 0;

        transfer_batch_state batch;
        batch.owner = device;
        if (!acquire_transfer_buffer(*dev, batch.entry))
            return 0;
        batch.cmd = dev->transfers.buffers[batch.entry].cmd;

        const resource_id id = allocate_id();
        reg().transfer_batches.emplace(id, batch);
        return id;
    }

    void discard_transfer_batch(resource_id id) noexcept
    {
        transfer_batch_state *batch = find(reg().transfer_batches, id);
        if (!batch)
            return;

        if (device_state *dev = find_device(batch->owner))
        {
            if (batch->recording)
                vkEndCommandBuffer(batch->cmd); // Never submitted; ending it just makes it resettable.
            if (batch->entry < dev->transfers.buffers.size())
                dev->transfers.buffers[batch->entry].busy = false;

            if (batch->end_offset != 0)
            {
                if (dev->ring.head == batch->end_offset)
                {
                    // The batch is the top of the ring, so its space comes straight back.
                    dev->ring.head = batch->begin_offset;
                }
                else
                {
                    // Something staged above it since. Release the range as an already-complete
                    // block instead: it is freed as soon as everything below it is.
                    retain_staging(*dev, batch->end_offset, 0);
                    retire_staging_blocks(*dev);
                }
            }
        }
        reg().transfer_batches.erase(id);
    }

    std::size_t transfer_batch_staged_bytes(resource_id id) noexcept
    {
        const transfer_batch_state *batch = find(reg().transfer_batches, id);
        return batch ? batch->staged_bytes : 0;
    }

    std::size_t transfer_batch_size(resource_id id) noexcept
    {
        const transfer_batch_state *batch = find(reg().transfer_batches, id);
        return batch ? batch->count : 0;
    }

    std::expected<void, error> transfer_upload_buffer(resource_id id, resource_id dst, std::size_t offset,
                                                      std::span<const std::byte> data)
    {
        transfer_batch_state *batch = find(reg().transfer_batches, id);
        if (!batch)
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::upload"));

        device_state *dev = find_device(batch->owner);
        buffer_state *b = find(reg().buffers, dst);
        if (!dev || !b || b->owner != batch->owner)
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::upload"));
        if (dev->lost)
            return std::unexpected(make_error(error_code::device_lost, "transfer_batch::upload"));
        if (!range_in_bounds(b->desc.size_bytes, offset, data.size()))
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::upload"));
        if (b->desc.access == memory_access::gpu_to_cpu)
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::upload"));

        // A host-visible destination needs no GPU involvement at all: write it and be done. Worth
        // the branch because it is most buffers on an integrated adapter, and all of them on a
        // unified-memory one.
        if (b->mapped)
        {
            std::memcpy(static_cast<std::byte *>(b->mapped) + offset, data.data(), data.size());
            if (!b->coherent)
                flush_host_writes(*dev, b->memory);
            ++batch->count;
            return {};
        }

        if (!ensure_recording(*batch))
            return std::unexpected(make_error(error_code::platform_error, "transfer_batch::upload"));

        const auto source = stage_into_ring(*dev, *batch, data);
        if (!source)
            return std::unexpected(source.error());

        VkBufferCopy region{};
        region.srcOffset = *source;
        region.dstOffset = offset;
        region.size = data.size();
        vkCmdCopyBuffer(batch->cmd, dev->ring.buffer, b->buffer, 1, &region);

        note_hazard(*dev, *batch, b->created);
        ++batch->count;
        return {};
    }

    std::expected<void, error> transfer_upload_texture(resource_id id, resource_id dst,
                                                       std::span<const std::byte> data)
    {
        transfer_batch_state *batch = find(reg().transfer_batches, id);
        if (!batch)
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::upload"));

        device_state *dev = find_device(batch->owner);
        texture_state *t = find(reg().textures, dst);
        if (!dev || !t || t->owner != batch->owner || t->presentable)
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::upload"));
        if (dev->lost)
            return std::unexpected(make_error(error_code::device_lost, "transfer_batch::upload"));
        if (data.size() != mip0_bytes(t->desc))
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::upload"));

        if (!ensure_recording(*batch))
            return std::unexpected(make_error(error_code::platform_error, "transfer_batch::upload"));

        const auto source = stage_into_ring(*dev, *batch, data);
        if (!source)
            return std::unexpected(source.error());

        VkBufferImageCopy region{};
        region.bufferOffset = *source;
        region.imageSubresource.aspectMask =
            (t->aspect & VK_IMAGE_ASPECT_COLOR_BIT) ? VK_IMAGE_ASPECT_COLOR_BIT : VK_IMAGE_ASPECT_DEPTH_BIT;
        region.imageSubresource.mipLevel = 0;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = image_extent(t->desc);
        // Textures live in GENERAL, which is a legal transfer destination, so there is no layout
        // transition here and therefore no queue-family ownership question either.
        vkCmdCopyBufferToImage(batch->cmd, dev->ring.buffer, t->image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);

        note_hazard(*dev, *batch, t->created);
        ++batch->count;
        return {};
    }

    std::expected<std::uint64_t, error> submit_transfer_batch(resource_id id)
    {
        transfer_batch_state *batch = find(reg().transfer_batches, id);
        if (!batch)
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::submit"));

        device_state *dev = find_device(batch->owner);
        if (!dev)
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::submit"));
        if (dev->lost)
            return std::unexpected(make_error(error_code::device_lost, "transfer_batch::submit"));

        // Nothing was recorded - every upload went straight into mapped memory, or there were none.
        // The honest answer is the "no work" value, and the batch stays open for reuse.
        if (!batch->recording)
        {
            batch->count = 0;
            return 0ull;
        }

        // One flush for the whole batch rather than one per upload; the ring's alignment already
        // covers `nonCoherentAtomSize`, so a whole-range flush is exact rather than merely legal.
        if (!dev->ring.coherent)
            flush_host_writes(*dev, dev->ring.memory);

        const VkResult ended = vkEndCommandBuffer(batch->cmd);
        batch->recording = false;
        if (ended != VK_SUCCESS)
            return std::unexpected(to_error(*dev, ended, "vkEndCommandBuffer"));

        // Write-after-read: order the copies after whatever might still be reading the targets.
        // GPU-side, so this costs the calling thread nothing, and it is empty for the case that
        // matters most - uploading into a resource created moments ago.
        timeline_point waits[queue_kind_count];
        std::size_t wait_count = 0;
        for (std::size_t i = 0; i < queue_kind_count; ++i)
        {
            const auto kind = static_cast<queue_kind>(i);
            if (kind == queue_kind::copy || !batch->hazard[i])
                continue;
            const std::uint64_t value = dev->queues[i].last_submitted;
            if (value != 0)
                waits[wait_count++] = timeline_point{rendering::device{batch->owner}, kind, value};
        }

        const VkCommandBuffer commands[] = {batch->cmd};
        const auto value = submit_batch(*dev, {.kind = queue_kind::copy,
                                               .commands = commands,
                                               .timeline_waits = std::span{waits, wait_count}});
        if (!value)
        {
            // The command buffer is spent either way; let it be recycled once the queue drains.
            dev->transfers.buffers[batch->entry].busy = false;
            dev->transfers.buffers[batch->entry].point = dev->queues[static_cast<std::size_t>(queue_kind::copy)]
                                                             .last_submitted;
            batch->end_offset = 0;
            return std::unexpected(value.error());
        }

        retain_staging(*dev, batch->end_offset, *value);
        dev->transfers.buffers[batch->entry].busy = false;
        dev->transfers.buffers[batch->entry].point = *value;

        // Read-after-write, for everything submitted from here on: see `submit_batch`.
        dev->last_transfer = std::max(dev->last_transfer, *value);

        // The batch may be filled again; it needs a fresh command buffer and a fresh ring range.
        batch->begin_offset = 0;
        batch->end_offset = 0;
        batch->staged_bytes = 0;
        batch->count = 0;
        batch->hazard = {};
        if (acquire_transfer_buffer(*dev, batch->entry))
            batch->cmd = dev->transfers.buffers[batch->entry].cmd;
        else
            batch->cmd = VK_NULL_HANDLE;

        return *value;
    }

    // -------------------------------------------------------------------------
    // Downloads
    // -------------------------------------------------------------------------

    std::expected<resource_id, error> begin_download(resource_id device, resource_id src, std::size_t offset,
                                                     std::size_t size, std::span<const timeline_point> after,
                                                     std::uint64_t &out_value)
    {
        device_state *dev = find_device(device);
        buffer_state *b = find(reg().buffers, src);
        if (!dev || !b || b->owner != device)
            return std::unexpected(make_error(error_code::invalid_argument, "download"));
        if (dev->lost)
            return std::unexpected(make_error(error_code::device_lost, "download"));
        if (!range_in_bounds(b->desc.size_bytes, offset, size))
            return std::unexpected(make_error(error_code::invalid_argument, "download"));
        if (!has_flag(b->desc.usage, buffer_usage::transfer_src))
            return std::unexpected(make_error(error_code::invalid_argument, "download"));

        // A dedicated host-visible buffer rather than a slice of the ring: a readback is owned by
        // the caller and lives until they have read it, which is the one lifetime the ring's
        // recycle-behind-the-timeline rule cannot express.
        download_state d;
        d.owner = device;
        d.size = size;

        VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        info.size = size;
        info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkResult result = vkCreateBuffer(dev->device, &info, nullptr, &d.buffer);
        if (result != VK_SUCCESS)
            return std::unexpected(to_error(*dev, result, "vkCreateBuffer"));

        void *mapped = nullptr;
        if (!allocate_buffer_memory(*dev, d.buffer, memory_access::gpu_to_cpu, d.memory, mapped, d.coherent) ||
            mapped == nullptr)
        {
            vkDestroyBuffer(dev->device, d.buffer, nullptr);
            return std::unexpected(make_error(error_code::out_of_device_memory, "download"));
        }
        d.mapped = mapped;

        std::size_t entry = 0;
        if (!acquire_transfer_buffer(*dev, entry))
        {
            release_download_objects(*dev, d);
            return std::unexpected(make_error(error_code::not_ready, "download"));
        }
        const VkCommandBuffer cmd = dev->transfers.buffers[entry].cmd;

        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        result = vkBeginCommandBuffer(cmd, &begin);
        if (result != VK_SUCCESS)
        {
            dev->transfers.buffers[entry].busy = false;
            release_download_objects(*dev, d);
            return std::unexpected(to_error(*dev, result, "vkBeginCommandBuffer"));
        }

        VkBufferCopy region{};
        region.srcOffset = offset;
        region.dstOffset = 0;
        region.size = size;
        vkCmdCopyBuffer(cmd, b->buffer, d.buffer, 1, &region);
        result = vkEndCommandBuffer(cmd);
        if (result != VK_SUCCESS)
        {
            dev->transfers.buffers[entry].busy = false;
            release_download_objects(*dev, d);
            return std::unexpected(to_error(*dev, result, "vkEndCommandBuffer"));
        }

        // Nothing named: order the copy behind everything currently submitted, which is what
        // `read_buffer` does by blocking and is the only safe default when the caller has not said
        // which submission produced the data.
        timeline_point implicit[queue_kind_count];
        std::span<const timeline_point> waits = after;
        if (waits.empty())
        {
            std::size_t count = 0;
            for (std::size_t i = 0; i < queue_kind_count; ++i)
            {
                const std::uint64_t value = dev->queues[i].last_submitted;
                if (value != 0)
                    implicit[count++] = timeline_point{rendering::device{device}, static_cast<queue_kind>(i), value};
            }
            waits = std::span{implicit, count};
        }

        const VkCommandBuffer commands[] = {cmd};
        const auto value =
            submit_batch(*dev, {.kind = queue_kind::copy, .commands = commands, .timeline_waits = waits});

        dev->transfers.buffers[entry].busy = false;
        if (!value)
        {
            release_download_objects(*dev, d);
            return std::unexpected(value.error());
        }
        dev->transfers.buffers[entry].point = *value;

        out_value = *value;
        d.point = timeline_point{rendering::device{device}, queue_kind::copy, *value};

        const resource_id id = allocate_id();
        reg().downloads.emplace(id, d);
        return id;
    }

    std::expected<std::span<const std::byte>, error> download_bytes(resource_id id)
    {
        download_state *d = find(reg().downloads, id);
        if (!d)
            return std::unexpected(make_error(error_code::invalid_argument, "readback::bytes"));

        device_state *dev = find_device(d->owner);
        if (!dev)
            return std::unexpected(make_error(error_code::invalid_argument, "readback::bytes"));
        if (dev->lost)
            return std::unexpected(make_error(error_code::device_lost, "readback::bytes"));

        // Asked of the backend directly. `timeline_point::is_complete` would take the module lock,
        // which the public layer is already holding on this thread's behalf.
        refresh_completed(*dev);
        if (queue_for(*dev, queue_kind::copy).completed < d->point.value())
            return std::unexpected(make_error(error_code::not_ready, "readback::bytes"));

        if (!d->coherent && !d->invalidated)
        {
            invalidate_host_reads(*dev, d->memory);
            d->invalidated = true;
        }
        return std::span<const std::byte>{static_cast<const std::byte *>(d->mapped), d->size};
    }

    void destroy_download(resource_id id) noexcept
    {
        download_state *d = find(reg().downloads, id);
        if (!d)
            return;

        if (device_state *dev = find_device(d->owner))
        {
            const VkDevice device = dev->device;
            const VkBuffer buffer = d->buffer;
            const VkDeviceMemory memory = d->memory;
            // Deferred, so destroying a readback whose copy is still running is legal and free.
            defer_release(*dev, [device, buffer, memory] {
                vkDestroyBuffer(device, buffer, nullptr);
                vkFreeMemory(device, memory, nullptr);
            });
        }
        reg().downloads.erase(id);
    }

} // namespace catalyst::rendering::detail
