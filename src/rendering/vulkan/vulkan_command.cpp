/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Command pools and command lists of the Vulkan backend: `VkCommandPool` allocation, per-list descriptor pools,
 * dynamic-rendering render passes, the slot -> descriptor-set translation and submission (which also performs the
 * swapchain semaphore hand-off).
 * @details **What Tier 3 changed here.** `begin_recording` used to call `wait_for_serial` on the list's previous
 * submission, because `vkResetCommandPool` on a pool the GPU is still reading is undefined. That wait is gone: a list
 * is refused while it is in flight, and discharging the precondition is the caller's - which in practice means
 * `frame_ring`, which waits once for a whole frame's pools instead of once per list.
 *
 * The pool is now a first-class object rather than a private detail of a list, for the reason Vulkan made it one: it
 * is the allocator, it is externally synchronised, and resetting it is how a command buffer is recycled. A list
 * created through `create_command_list(device, ...)` still gets a pool of its own, marked `implicit`, which
 * `begin_recording` may reset by itself because it is the only thing in it.
 */

#include "vulkan_backend.hpp"
#include "vulkan_convert.hpp"

#include <algorithm>
#include <array>
#include <vector>

namespace catalyst::rendering::detail::vulkan
{

    void release_command_list_objects(device_state &dev, command_list_state &cl) noexcept
    {
        for (VkDescriptorPool pool : cl.descriptor_pools)
            vkDestroyDescriptorPool(dev.device, pool, nullptr);
        cl.descriptor_pools.clear();

        // The command buffer belongs to the pool, and the pool is a separate object with its own
        // lifetime - so free the buffer and leave the pool alone. `release_command_pool_objects`
        // destroys the pool, which frees anything still allocated from it.
        if (cl.pool && cl.cmd)
            vkFreeCommandBuffers(dev.device, cl.pool, 1, &cl.cmd);
        cl.pool = VK_NULL_HANDLE;
        cl.cmd = VK_NULL_HANDLE;
    }

    void release_command_pool_objects(device_state &dev, command_pool_state &pool) noexcept
    {
        if (pool.pool)
            vkDestroyCommandPool(dev.device, pool.pool, nullptr); // Frees every buffer allocated from it.
        pool.pool = VK_NULL_HANDLE;
        pool.lists.clear();
    }

    namespace
    {
        command_list_state *recording_list(resource_id id) noexcept
        {
            command_list_state *cl = find(reg().command_lists, id);
            return (cl && cl->recording) ? cl : nullptr;
        }

        const buffer_state *usable_buffer(const command_list_state &cl, resource_id id, buffer_usage usage) noexcept
        {
            const buffer_state *b = find(reg().buffers, id);
            if (!b || b->owner != cl.owner || !has_flag(b->desc.usage, usage))
                return nullptr;
            return b;
        }

        void reset_bindings(command_list_state &cl) noexcept
        {
            cl.uniform_buffers.fill({});
            cl.storage_buffers.fill({});
            cl.textures.fill(VK_NULL_HANDLE);
            cl.samplers.fill(VK_NULL_HANDLE);
            cl.dirty.fill(false);
            cl.sets.fill(VK_NULL_HANDLE);
            cl.bound_graphics.fill(false);
            cl.bound_compute.fill(false);
            cl.bound_pipeline = 0;
            cl.in_render_pass = false;
            cl.pass_present_images.clear();
        }

        // ---------------------------------------------------------------------
        // Descriptor sets
        // ---------------------------------------------------------------------

        bool create_descriptor_pool(device_state &dev, command_list_state &cl) noexcept
        {
            const VkDescriptorPoolSize sizes[] = {
                {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, descriptor_sets_per_pool * max_uniform_buffer_slots},
                {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, descriptor_sets_per_pool * max_storage_buffer_slots},
                {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, descriptor_sets_per_pool * max_texture_slots},
                {VK_DESCRIPTOR_TYPE_SAMPLER, descriptor_sets_per_pool * max_sampler_slots},
            };

            VkDescriptorPoolCreateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
            info.maxSets = descriptor_sets_per_pool;
            info.poolSizeCount = 4;
            info.pPoolSizes = sizes;

            VkDescriptorPool pool = VK_NULL_HANDLE;
            const VkResult result = vkCreateDescriptorPool(dev.device, &info, nullptr, &pool);
            if (result != VK_SUCCESS)
            {
                logging::error<detail::render_log>("vkCreateDescriptorPool failed ({})", result_string(result));
                return false;
            }
            cl.descriptor_pools.push_back(pool);
            return true;
        }

        VkDescriptorSet allocate_set(device_state &dev, command_list_state &cl, std::uint32_t set_index) noexcept
        {
            for (;;)
            {
                if (cl.active_pool >= cl.descriptor_pools.size() && !create_descriptor_pool(dev, cl))
                    return VK_NULL_HANDLE;

                VkDescriptorSetAllocateInfo info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
                info.descriptorPool = cl.descriptor_pools[cl.active_pool];
                info.descriptorSetCount = 1;
                info.pSetLayouts = &dev.set_layouts[set_index];

                VkDescriptorSet set = VK_NULL_HANDLE;
                const VkResult result = vkAllocateDescriptorSets(dev.device, &info, &set);
                if (result == VK_SUCCESS)
                    return set;
                if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL)
                {
                    logging::error<detail::render_log>("vkAllocateDescriptorSets failed ({})", result_string(result));
                    return VK_NULL_HANDLE;
                }
                ++cl.active_pool; // This pool is full; move on (creating a new one if needed).
            }
        }

        void write_set(device_state &dev, const command_list_state &cl, std::uint32_t set_index,
                       VkDescriptorSet set) noexcept
        {
            std::array<VkWriteDescriptorSet, 16> writes{};
            std::array<VkDescriptorBufferInfo, 16> buffer_infos{};
            std::array<VkDescriptorImageInfo, 16> image_infos{};
            static_assert(max_uniform_buffer_slots <= 16 && max_storage_buffer_slots <= 16 &&
                          max_texture_slots <= 16 && max_sampler_slots <= 16);
            std::uint32_t count = 0;

            auto add = [&](std::uint32_t binding, VkDescriptorType type) -> VkWriteDescriptorSet & {
                VkWriteDescriptorSet &w = writes[count++];
                w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w.dstSet = set;
                w.dstBinding = binding;
                w.dstArrayElement = 0;
                w.descriptorCount = 1;
                w.descriptorType = type;
                return w;
            };

            switch (set_index)
            {
            case set_uniform_buffers:
            case set_storage_buffers:
            {
                const bool uniform = set_index == set_uniform_buffers;
                const std::uint32_t slots = uniform ? max_uniform_buffer_slots : max_storage_buffer_slots;
                for (std::uint32_t i = 0; i < slots; ++i)
                {
                    const buffer_binding &b = uniform ? cl.uniform_buffers[i] : cl.storage_buffers[i];
                    if (!b.buffer)
                        continue;
                    buffer_infos[i] = {b.buffer, b.offset, b.range};
                    add(i, uniform ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
                        .pBufferInfo = &buffer_infos[i];
                }
                break;
            }
            case set_textures:
                for (std::uint32_t i = 0; i < max_texture_slots; ++i)
                {
                    if (!cl.textures[i])
                        continue;
                    image_infos[i] = {VK_NULL_HANDLE, cl.textures[i], VK_IMAGE_LAYOUT_GENERAL};
                    add(i, VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE).pImageInfo = &image_infos[i];
                }
                break;
            default:
                for (std::uint32_t i = 0; i < max_sampler_slots; ++i)
                {
                    if (!cl.samplers[i])
                        continue;
                    image_infos[i] = {cl.samplers[i], VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
                    add(i, VK_DESCRIPTOR_TYPE_SAMPLER).pImageInfo = &image_infos[i];
                }
                break;
            }

            if (count)
                vkUpdateDescriptorSets(dev.device, count, writes.data(), 0, nullptr);
        }

        /** Re-allocates dirty sets and makes sure every populated set is bound to `bind_point`. */
        void flush_descriptors(device_state &dev, command_list_state &cl, VkPipelineBindPoint bind_point) noexcept
        {
            std::array<bool, descriptor_set_count> &bound =
                bind_point == VK_PIPELINE_BIND_POINT_COMPUTE ? cl.bound_compute : cl.bound_graphics;

            for (std::uint32_t i = 0; i < descriptor_set_count; ++i)
            {
                if (cl.dirty[i])
                {
                    VkDescriptorSet set = allocate_set(dev, cl, i);
                    if (!set)
                        continue;
                    write_set(dev, cl, i, set);
                    cl.sets[i] = set;
                    cl.bound_graphics[i] = false;
                    cl.bound_compute[i] = false;
                    cl.dirty[i] = false;
                }
                if (cl.sets[i] && !bound[i])
                {
                    vkCmdBindDescriptorSets(cl.cmd, bind_point, dev.pipeline_layout, i, 1, &cl.sets[i], 0, nullptr);
                    bound[i] = true;
                }
            }
        }

        // ---------------------------------------------------------------------
        // Fixed-function helpers
        // ---------------------------------------------------------------------

        /** Flips Y so clip-space +Y is up, matching D3D12 / Metal (VK_KHR_maintenance1 semantics, core since 1.1). */
        VkViewport flipped_viewport(const viewport &vp) noexcept
        {
            VkViewport v{};
            v.x = vp.x;
            v.y = vp.y + vp.height;
            v.width = vp.width;
            v.height = -vp.height;
            v.minDepth = vp.min_depth;
            v.maxDepth = vp.max_depth;
            return v;
        }

        void apply_viewport(command_list_state &cl, const viewport &vp) noexcept
        {
            if (vp.width <= 0.0f || vp.height <= 0.0f)
                return;
            const VkViewport v = flipped_viewport(vp);
            vkCmdSetViewport(cl.cmd, 0, 1, &v);
        }

        void apply_scissor(command_list_state &cl, const scissor_rect &rect) noexcept
        {
            VkRect2D r{};
            // Vulkan requires non-negative offsets; clip the rectangle against the framebuffer origin.
            const std::int32_t x = std::max(rect.x, 0);
            const std::int32_t y = std::max(rect.y, 0);
            const std::int64_t right = static_cast<std::int64_t>(rect.x) + rect.width;
            const std::int64_t bottom = static_cast<std::int64_t>(rect.y) + rect.height;
            r.offset = {x, y};
            r.extent.width = static_cast<std::uint32_t>(std::max<std::int64_t>(right - x, 0));
            r.extent.height = static_cast<std::uint32_t>(std::max<std::int64_t>(bottom - y, 0));
            vkCmdSetScissor(cl.cmd, 0, 1, &r);
        }

        void transition_presentable(VkCommandBuffer cmd, std::span<const VkImage> images, bool to_attachment) noexcept
        {
            if (images.empty())
                return;
            std::vector<VkImageMemoryBarrier> barriers(images.size(), VkImageMemoryBarrier{});
            for (std::size_t i = 0; i < images.size(); ++i)
            {
                VkImageMemoryBarrier &b = barriers[i];
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.image = images[i];
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                if (to_attachment)
                {
                    b.srcAccessMask = 0;
                    b.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    b.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                    b.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                }
                else
                {
                    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
                    b.dstAccessMask = 0;
                    b.oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
                    b.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                }
            }
            const VkPipelineStageFlags src = to_attachment ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                                           : VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
            const VkPipelineStageFlags dst = to_attachment ? VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT
                                                           : VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
            vkCmdPipelineBarrier(cmd, src, dst, 0, 0, nullptr, 0, nullptr, static_cast<std::uint32_t>(barriers.size()),
                                 barriers.data());
        }
    } // namespace

} // namespace catalyst::rendering::detail::vulkan

namespace catalyst::rendering::detail
{

    using namespace vulkan;

    // -------------------------------------------------------------------------
    // Lifetime and recording state
    // -------------------------------------------------------------------------

    // -------------------------------------------------------------------------
    // Command pools
    // -------------------------------------------------------------------------

    namespace
    {
        /** Creates the VkCommandPool for `kind` on `dev`, or VK_NULL_HANDLE. */
        VkCommandPool make_vk_pool(device_state &dev, queue_kind kind, const char *what) noexcept
        {
            VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
            // The family the kind resolves to on this adapter. A pool is tied to one family, which
            // is why the kind has to be fixed here rather than at submission.
            info.queueFamilyIndex = queue_for(dev, kind).family;

            VkCommandPool pool = VK_NULL_HANDLE;
            const VkResult result = vkCreateCommandPool(dev.device, &info, nullptr, &pool);
            if (result != VK_SUCCESS)
            {
                logging::error<detail::render_log>("{}: vkCreateCommandPool failed ({})", what,
                                                   result_string(result));
                return VK_NULL_HANDLE;
            }
            return pool;
        }

        /** Allocates a primary command buffer from `pool` and registers a list around it. */
        resource_id make_list(device_state &dev, resource_id device_id, resource_id pool_id, VkCommandPool pool,
                              queue_kind kind, const char *debug_name, const char *what)
        {
            VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
            alloc.commandPool = pool;
            alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            alloc.commandBufferCount = 1;

            VkCommandBuffer cmd = VK_NULL_HANDLE;
            const VkResult result = vkAllocateCommandBuffers(dev.device, &alloc, &cmd);
            if (result != VK_SUCCESS)
            {
                logging::error<detail::render_log>("{}: vkAllocateCommandBuffers failed ({})", what,
                                                   result_string(result));
                return 0;
            }

            command_list_state cl;
            cl.owner = device_id;
            cl.desc.queue = kind;
            cl.debug_name = copy_name(debug_name);
            cl.pool_id = pool_id;
            cl.pool = pool;
            cl.cmd = cmd;

            set_debug_name(dev, VK_OBJECT_TYPE_COMMAND_BUFFER, handle_bits(cmd), cl.debug_name);

            const resource_id id = allocate_id();
            reg().command_lists.emplace(id, std::move(cl));
            return id;
        }
    } // namespace

    resource_id create_command_pool(resource_id device, const command_pool_desc &desc)
    {
        device_state *dev = find_device(device);
        if (!dev)
            return 0;

        command_pool_state pool;
        pool.owner = device;
        pool.desc = desc;
        pool.desc.debug_name = nullptr;
        pool.debug_name = copy_name(desc.debug_name);
        pool.pool = make_vk_pool(*dev, desc.queue, "create_command_pool");
        if (!pool.pool)
            return 0;

        set_debug_name(*dev, VK_OBJECT_TYPE_COMMAND_POOL, handle_bits(pool.pool), pool.debug_name);

        const resource_id id = allocate_id();
        reg().command_pools.emplace(id, std::move(pool));
        return id;
    }

    void destroy_command_pool(resource_id id) noexcept
    {
        command_pool_state *pool = find(reg().command_pools, id);
        if (!pool)
            return;

        device_state *dev = find_device(pool->owner);
        // Copied, because erasing the lists below mutates `pool->lists` through `destroy_command_list`.
        const std::vector<resource_id> lists = pool->lists;
        for (const resource_id list_id : lists)
            destroy_command_list(list_id);

        if (dev)
            release_command_pool_objects(*dev, *pool);
        reg().command_pools.erase(id);
    }

    bool is_command_pool_valid(resource_id id) noexcept
    {
        return find(reg().command_pools, id) != nullptr;
    }

    command_pool_desc get_command_pool_desc(resource_id id) noexcept
    {
        const command_pool_state *pool = find(reg().command_pools, id);
        if (!pool)
            return {};
        command_pool_desc desc = pool->desc;
        desc.debug_name = name_or_null(pool->debug_name);
        return desc;
    }

    resource_id get_command_pool_device(resource_id id) noexcept
    {
        const command_pool_state *pool = find(reg().command_pools, id);
        return pool ? pool->owner : 0;
    }

    std::expected<void, error> reset_command_pool(resource_id id)
    {
        command_pool_state *pool = find(reg().command_pools, id);
        if (!pool)
            return std::unexpected(make_error(error_code::invalid_argument, "reset_command_pool"));

        device_state *dev = find_device(pool->owner);
        if (!dev)
            return std::unexpected(make_error(error_code::invalid_argument, "reset_command_pool"));
        if (dev->lost)
        {
            // Nothing will ever complete again, so refusing forever would be worse than useless.
            // The command buffers are dead either way.
            return {};
        }

        refresh_completed(*dev);

        // Checked before anything is reset, so a refusal leaves the pool exactly as it was. This is
        // the precondition that used to be discharged by a hidden wait inside `begin_recording`.
        for (const resource_id list_id : pool->lists)
        {
            const command_list_state *cl = find(reg().command_lists, list_id);
            if (!cl || !cl->last_submit.valid())
                continue;
            if (queue_for(*dev, cl->last_submit.queue()).completed < cl->last_submit.value())
                return std::unexpected(make_error(error_code::not_ready, "reset_command_pool"));
        }

        const VkResult result = vkResetCommandPool(dev->device, pool->pool, 0);
        if (result != VK_SUCCESS)
            return std::unexpected(to_error(*dev, result, "vkResetCommandPool"));

        for (const resource_id list_id : pool->lists)
        {
            command_list_state *cl = find(reg().command_lists, list_id);
            if (!cl)
                continue;
            for (VkDescriptorPool descriptors : cl->descriptor_pools)
                vkResetDescriptorPool(dev->device, descriptors, 0);
            cl->active_pool = 0;
            cl->recording = false;
            cl->ready = false;
            cl->recorded = false;
            cl->last_submit = {};
        }
        return {};
    }

    resource_id create_command_list_in_pool(resource_id pool_id, const char *debug_name)
    {
        command_pool_state *pool = find(reg().command_pools, pool_id);
        if (!pool)
            return 0;
        device_state *dev = find_device(pool->owner);
        if (!dev)
            return 0;

        const resource_id id = make_list(*dev, pool->owner, pool_id, pool->pool, pool->desc.queue, debug_name,
                                         "create_command_list");
        if (id != 0)
            pool->lists.push_back(id);
        return id;
    }

    // -------------------------------------------------------------------------
    // Command lists
    // -------------------------------------------------------------------------

    resource_id create_command_list(resource_id device, const command_list_desc &desc)
    {
        device_state *dev = find_device(device);
        if (!dev)
            return 0;

        // The convenience form: a private pool with exactly one list in it, which is what makes it
        // safe for `begin_recording` to recycle the pool itself.
        command_pool_state pool;
        pool.owner = device;
        pool.desc.queue = desc.queue;
        pool.implicit = true;
        pool.debug_name = copy_name(desc.debug_name);
        pool.pool = make_vk_pool(*dev, desc.queue, "create_command_list");
        if (!pool.pool)
            return 0;

        const resource_id pool_id = allocate_id();
        const resource_id id = make_list(*dev, device, pool_id, pool.pool, desc.queue, desc.debug_name,
                                         "create_command_list");
        if (id == 0)
        {
            release_command_pool_objects(*dev, pool);
            return 0;
        }

        pool.lists.push_back(id);
        reg().command_pools.emplace(pool_id, std::move(pool));
        return id;
    }

    void destroy_command_list(resource_id id) noexcept
    {
        command_list_state *cl = find(reg().command_lists, id);
        if (!cl)
            return;

        const resource_id pool_id = cl->pool_id;
        if (device_state *dev = find_device(cl->owner))
        {
            wait_point(*dev, cl->last_submit); // A pending command buffer cannot be freed.
            release_command_list_objects(*dev, *cl);
        }
        reg().command_lists.erase(id);

        if (command_pool_state *pool = find(reg().command_pools, pool_id))
        {
            std::erase(pool->lists, id);
            // The implicit pool exists for exactly one list; with the list gone it has no reason to.
            if (pool->implicit && pool->lists.empty())
            {
                if (device_state *dev = find_device(pool->owner))
                    release_command_pool_objects(*dev, *pool);
                reg().command_pools.erase(pool_id);
            }
        }
    }

    bool is_command_list_valid(resource_id id) noexcept
    {
        return find(reg().command_lists, id) != nullptr;
    }

    command_list_desc get_command_list_desc(resource_id id) noexcept
    {
        const command_list_state *cl = find(reg().command_lists, id);
        if (!cl)
            return {};
        command_list_desc desc = cl->desc;
        desc.debug_name = name_or_null(cl->debug_name);
        return desc;
    }

    timeline_point command_list_last_submission(resource_id id) noexcept
    {
        const command_list_state *cl = find(reg().command_lists, id);
        return cl ? cl->last_submit : timeline_point{};
    }

    bool begin_recording(resource_id id)
    {
        command_list_state *cl = find(reg().command_lists, id);
        if (!cl || cl->recording)
            return false;
        device_state *dev = find_device(cl->owner);
        if (!dev)
            return false;

        command_pool_state *pool = find(reg().command_pools, cl->pool_id);
        if (!pool)
            return false;

        // The wait that used to be here is gone. What replaced it is a refusal: a command buffer
        // cannot be reset while the GPU is reading it, so a list still in flight is not recordable,
        // and saying so is the caller's cue to rotate lists or wait on the point deliberately.
        if (cl->last_submit.valid() && !dev->lost)
        {
            refresh_completed(*dev);
            if (queue_for(*dev, cl->last_submit.queue()).completed < cl->last_submit.value())
            {
                logging::warn<detail::render_log>(
                    "begin_recording: the list is still executing (queue {} is at {}, it was submitted at {}); "
                    "wait on last_submission, or record from a frame_ring pool",
                    to_string(cl->last_submit.queue()), queue_for(*dev, cl->last_submit.queue()).completed,
                    cl->last_submit.value());
                return false;
            }
        }

        if (pool->implicit)
        {
            // One list in the pool, and it has just been shown to be idle, so recycling here is
            // safe and saves the caller a `reset_command_pool` they could not get wrong anyway.
            if (vkResetCommandPool(dev->device, pool->pool, 0) != VK_SUCCESS)
                return false;
            for (VkDescriptorPool descriptors : cl->descriptor_pools)
                vkResetDescriptorPool(dev->device, descriptors, 0);
            cl->active_pool = 0;
            cl->recorded = false;
            cl->last_submit = {};
        }
        else if (cl->recorded)
        {
            // A shared pool is recycled as a whole. Re-recording without that would reset one
            // command buffer out of a pool that was not created to allow it.
            logging::warn<detail::render_log>(
                "begin_recording: the list has already been recorded since its pool was reset; "
                "call reset_command_pool first");
            return false;
        }

        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        begin.flags = VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT; // Lists may be re-submitted while still running.
        const VkResult result = vkBeginCommandBuffer(cl->cmd, &begin);
        if (result != VK_SUCCESS)
        {
            logging::error<detail::render_log>("begin_recording: vkBeginCommandBuffer failed ({})",
                                               result_string(result));
            return false;
        }

        reset_bindings(*cl);
        cl->recording = true;
        cl->ready = false;
        cl->recorded = true;
        return true;
    }

    bool end_recording(resource_id id)
    {
        command_list_state *cl = recording_list(id);
        if (!cl || cl->in_render_pass)
            return false;

        const VkResult result = vkEndCommandBuffer(cl->cmd);
        cl->recording = false;
        if (result != VK_SUCCESS)
        {
            logging::error<detail::render_log>("end_recording: vkEndCommandBuffer failed ({})", result_string(result));
            cl->ready = false;
            return false;
        }
        cl->ready = true;
        return true;
    }

    bool is_recording(resource_id id) noexcept
    {
        return recording_list(id) != nullptr;
    }

    // -------------------------------------------------------------------------
    // Render passes
    // -------------------------------------------------------------------------

    void begin_render_pass(resource_id id, const render_pass_desc &desc) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || cl->in_render_pass || !queue_accepts(cl->desc.queue, queue_kind::graphics))
            return;
        if (desc.color_attachments.size() > max_color_attachments)
            return;
        if (desc.color_attachments.empty() && !desc.depth_stencil.target)
            return;

        std::array<VkRenderingAttachmentInfo, max_color_attachments> colors{};
        std::vector<VkImage> present_images;
        std::uint32_t width = UINT32_MAX;
        std::uint32_t height = UINT32_MAX;

        for (std::size_t i = 0; i < desc.color_attachments.size(); ++i)
        {
            const color_attachment &a = desc.color_attachments[i];
            const texture_state *t = find(reg().textures, a.target.id());
            if (!t || t->owner != cl->owner || !has_flag(t->desc.usage, texture_usage::render_target))
                return;

            width = std::min(width, t->desc.extent.width);
            height = std::min(height, t->desc.extent.height);

            VkRenderingAttachmentInfo &info = colors[i];
            info.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            info.imageView = t->view;
            info.imageLayout = t->presentable ? VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_GENERAL;
            info.loadOp = to_vk_load_op(a.load);
            info.storeOp = to_vk_store_op(a.store);
            info.clearValue.color = {{a.clear.r, a.clear.g, a.clear.b, a.clear.a}};

            if (t->presentable)
                present_images.push_back(t->image);
        }

        VkRenderingAttachmentInfo depth{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        bool has_depth = false;
        bool has_stencil = false;
        if (desc.depth_stencil.target)
        {
            const texture_state *t = find(reg().textures, desc.depth_stencil.target.id());
            if (!t || t->owner != cl->owner || !has_flag(t->desc.usage, texture_usage::depth_stencil))
                return;

            width = std::min(width, t->desc.extent.width);
            height = std::min(height, t->desc.extent.height);

            depth.imageView = t->view;
            depth.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            depth.loadOp = to_vk_load_op(desc.depth_stencil.load);
            depth.storeOp = to_vk_store_op(desc.depth_stencil.store);
            depth.clearValue.depthStencil = {desc.depth_stencil.clear_depth, desc.depth_stencil.clear_stencil};
            has_depth = true;
            has_stencil = is_stencil_format(t->desc.pixel_format);
        }

        // Make everything written before this pass (uploads, compute, earlier passes) visible, then move presentable
        // images into their attachment layout.
        full_barrier(cl->cmd);
        transition_presentable(cl->cmd, present_images, true);

        VkRenderingInfo rendering{VK_STRUCTURE_TYPE_RENDERING_INFO};
        rendering.renderArea = {{0, 0}, {width, height}};
        rendering.layerCount = 1;
        rendering.colorAttachmentCount = static_cast<std::uint32_t>(desc.color_attachments.size());
        rendering.pColorAttachments = colors.data();
        rendering.pDepthAttachment = has_depth ? &depth : nullptr;
        rendering.pStencilAttachment = has_stencil ? &depth : nullptr;
        vkCmdBeginRendering(cl->cmd, &rendering);

        // Full-target defaults so a pass that never calls set_viewport / set_scissor still draws.
        apply_viewport(*cl, {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f});
        apply_scissor(*cl, {0, 0, width, height});

        cl->pass_present_images = std::move(present_images);
        cl->in_render_pass = true;
    }

    void end_render_pass(resource_id id) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || !cl->in_render_pass)
            return;

        vkCmdEndRendering(cl->cmd);
        transition_presentable(cl->cmd, cl->pass_present_images, false);
        cl->pass_present_images.clear();
        cl->in_render_pass = false;
    }

    // -------------------------------------------------------------------------
    // State
    // -------------------------------------------------------------------------

    void set_pipeline(resource_id id, resource_id pipeline) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl)
            return;
        const pipeline_state *p = find(reg().pipelines, pipeline);
        if (!p || p->owner != cl->owner)
            return;

        const VkPipelineBindPoint bind_point = p->type == pipeline_type::compute ? VK_PIPELINE_BIND_POINT_COMPUTE
                                                                                 : VK_PIPELINE_BIND_POINT_GRAPHICS;
        vkCmdBindPipeline(cl->cmd, bind_point, p->pipeline);
        cl->bound_pipeline = pipeline;
        cl->bound_pipeline_type = p->type;
    }

    void set_viewport(resource_id id, const viewport &vp) noexcept
    {
        if (command_list_state *cl = recording_list(id))
            apply_viewport(*cl, vp);
    }

    void set_scissor(resource_id id, const scissor_rect &rect) noexcept
    {
        if (command_list_state *cl = recording_list(id))
            apply_scissor(*cl, rect);
    }

    void set_vertex_buffer(resource_id id, std::uint32_t binding, resource_id buffer, std::size_t offset) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || binding >= max_vertex_bindings)
            return;
        const buffer_state *b = usable_buffer(*cl, buffer, buffer_usage::vertex);
        if (!b || offset >= b->desc.size_bytes)
            return;
        const VkDeviceSize vk_offset = offset;
        vkCmdBindVertexBuffers(cl->cmd, binding, 1, &b->buffer, &vk_offset);
    }

    void set_index_buffer(resource_id id, resource_id buffer, index_type type, std::size_t offset) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl)
            return;
        const buffer_state *b = usable_buffer(*cl, buffer, buffer_usage::index);
        if (!b || offset >= b->desc.size_bytes)
            return;
        vkCmdBindIndexBuffer(cl->cmd, b->buffer, offset, to_vk_index_type(type));
    }

    namespace
    {
        void bind_buffer_slot(command_list_state &cl, std::uint32_t set_index, std::uint32_t slot, resource_id buffer,
                              std::size_t offset, std::size_t size, buffer_usage usage) noexcept
        {
            const buffer_state *b = usable_buffer(cl, buffer, usage);
            if (!b || offset >= b->desc.size_bytes)
                return;
            const std::size_t available = b->desc.size_bytes - offset;
            if (size > available)
                return;

            buffer_binding binding;
            binding.buffer = b->buffer;
            binding.offset = offset;
            binding.range = size == 0 ? available : size;

            if (set_index == set_uniform_buffers)
                cl.uniform_buffers[slot] = binding;
            else
                cl.storage_buffers[slot] = binding;
            cl.dirty[set_index] = true;
        }
    } // namespace

    void set_uniform_buffer(resource_id id, std::uint32_t slot, resource_id buffer, std::size_t offset,
                            std::size_t size) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || slot >= max_uniform_buffer_slots)
            return;
        bind_buffer_slot(*cl, set_uniform_buffers, slot, buffer, offset, size, buffer_usage::uniform);
    }

    void set_storage_buffer(resource_id id, std::uint32_t slot, resource_id buffer, std::size_t offset,
                            std::size_t size) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || slot >= max_storage_buffer_slots)
            return;
        bind_buffer_slot(*cl, set_storage_buffers, slot, buffer, offset, size, buffer_usage::storage);
    }

    void set_texture(resource_id id, std::uint32_t slot, resource_id texture) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || slot >= max_texture_slots)
            return;
        const texture_state *t = find(reg().textures, texture);
        if (!t || t->owner != cl->owner || !has_flag(t->desc.usage, texture_usage::sampled))
            return;
        cl->textures[slot] = t->sampled_view;
        cl->dirty[set_textures] = true;
    }

    void set_sampler(resource_id id, std::uint32_t slot, resource_id sampler) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || slot >= max_sampler_slots)
            return;
        const sampler_state *s = find(reg().samplers, sampler);
        if (!s || s->owner != cl->owner)
            return;
        cl->samplers[slot] = s->sampler;
        cl->dirty[set_samplers] = true;
    }

    void push_constants(resource_id id, std::uint32_t offset, std::span<const std::byte> data) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || data.empty())
            return;
        if (offset % 4 != 0 || data.size() % 4 != 0)
            return; // Vulkan requires 4-byte granularity.
        device_state *dev = find_device(cl->owner);
        if (!dev)
            return;
        vkCmdPushConstants(cl->cmd, dev->pipeline_layout, VK_SHADER_STAGE_ALL, offset,
                           static_cast<std::uint32_t>(data.size()), data.data());
    }

    // -------------------------------------------------------------------------
    // Work
    // -------------------------------------------------------------------------

    namespace
    {
        /** The recording list if it is inside a render pass with a graphics pipeline bound. */
        command_list_state *drawable_list(resource_id id, device_state *&out_dev) noexcept
        {
            command_list_state *cl = recording_list(id);
            if (!cl || !cl->in_render_pass)
                return nullptr;
            const pipeline_state *p = find(reg().pipelines, cl->bound_pipeline);
            if (!p || p->type != pipeline_type::graphics)
                return nullptr;
            out_dev = find_device(cl->owner);
            return out_dev ? cl : nullptr;
        }
    } // namespace

    void draw(resource_id id, std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t first_vertex,
              std::uint32_t first_instance) noexcept
    {
        device_state *dev = nullptr;
        command_list_state *cl = drawable_list(id, dev);
        if (!cl)
            return;
        flush_descriptors(*dev, *cl, VK_PIPELINE_BIND_POINT_GRAPHICS);
        vkCmdDraw(cl->cmd, vertex_count, instance_count, first_vertex, first_instance);
    }

    void draw_indexed(resource_id id, std::uint32_t index_count, std::uint32_t instance_count,
                      std::uint32_t first_index, std::int32_t vertex_offset, std::uint32_t first_instance) noexcept
    {
        device_state *dev = nullptr;
        command_list_state *cl = drawable_list(id, dev);
        if (!cl)
            return;
        flush_descriptors(*dev, *cl, VK_PIPELINE_BIND_POINT_GRAPHICS);
        vkCmdDrawIndexed(cl->cmd, index_count, instance_count, first_index, vertex_offset, first_instance);
    }

    void dispatch(resource_id id, std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || cl->in_render_pass || !queue_accepts(cl->desc.queue, queue_kind::compute))
            return;
        const pipeline_state *p = find(reg().pipelines, cl->bound_pipeline);
        if (!p || p->type != pipeline_type::compute)
            return;
        device_state *dev = find_device(cl->owner);
        if (!dev)
            return;

        full_barrier(cl->cmd); // Inputs may have been written by earlier commands in this list.
        flush_descriptors(*dev, *cl, VK_PIPELINE_BIND_POINT_COMPUTE);
        vkCmdDispatch(cl->cmd, x, y, z);
    }

    void copy_buffer(resource_id id, resource_id src, std::size_t src_offset, resource_id dst, std::size_t dst_offset,
                     std::size_t size) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || cl->in_render_pass)
            return;

        const buffer_state *s = find(reg().buffers, src);
        const buffer_state *d = find(reg().buffers, dst);
        if (!s || !d || s->owner != cl->owner || d->owner != cl->owner)
            return;
        if (!range_in_bounds(s->desc.size_bytes, src_offset, size) ||
            !range_in_bounds(d->desc.size_bytes, dst_offset, size))
            return;

        full_barrier(cl->cmd); // The source may have been written by earlier commands in this list.

        VkBufferCopy region{};
        region.srcOffset = src_offset;
        region.dstOffset = dst_offset;
        region.size = size;
        vkCmdCopyBuffer(cl->cmd, s->buffer, d->buffer, 1, &region);
    }

    // -------------------------------------------------------------------------
    // Submission
    // -------------------------------------------------------------------------

    std::expected<std::uint64_t, error> submit(resource_id device, queue_kind kind,
                                               std::span<const command_list> lists,
                                               std::span<const timeline_point> waits)
    {
        device_state *dev = find_device(device);
        if (!dev)
            return std::unexpected(make_error(error_code::invalid_argument, "submit"));

        const rendering::device device_handle{device};

        std::vector<VkCommandBuffer> commands;
        commands.reserve(lists.size());
        for (const command_list &handle : lists)
        {
            const command_list_state *cl = find(reg().command_lists, handle.id());
            if (!cl || cl->owner != device || !cl->ready || cl->recording)
                return std::unexpected(make_error(error_code::invalid_argument, "submit"));
            commands.push_back(cl->cmd);
        }

        // The swapchain's binary semaphores ride along with whatever cross-queue timeline
        // dependencies the caller asked for; see submit_batch.
        std::vector<VkSemaphore> binary_waits;
        std::vector<VkPipelineStageFlags> wait_stages;
        std::vector<VkSemaphore> binary_signals;
        collect_acquire_waits(*dev, binary_waits, wait_stages, binary_signals);

        const auto value = submit_batch(*dev, {.kind = kind,
                                               .commands = commands,
                                               .binary_waits = binary_waits,
                                               .wait_stages = wait_stages,
                                               .binary_signals = binary_signals,
                                               .timeline_waits = waits});
        if (!value)
            return std::unexpected(value.error());

        const timeline_point point{device_handle, kind, *value};
        complete_acquire_waits(*dev, point);
        for (const command_list &handle : lists)
        {
            if (command_list_state *cl = find(reg().command_lists, handle.id()))
                cl->last_submit = point;
        }
        return *value;
    }

} // namespace catalyst::rendering::detail
