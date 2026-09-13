/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief CPU-side bookkeeping implementation of the rendering backend contract. It tracks resource lifetimes, enforces
 * the usage rules documented in the public headers and keeps buffer / shader contents in host memory, but performs no
 * GPU work. It is the resource layer of the "null" backend and also stands in for backends whose resource layer has not
 * been written yet, so the public API is exercisable (and testable) everywhere.
 */

#include "../detail_backend.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

namespace catalyst::rendering::detail
{

    namespace
    {
        // ---------------------------------------------------------------------
        // Resource records
        // ---------------------------------------------------------------------

        struct device_state
        {
            device_desc desc;
            std::string application_name;

            /**
             * One timeline per queue kind. There is no GPU, so a submission completes the instant
             * it is made and `completed` is simply the last value handed out - which is the right
             * answer rather than a shortcut: the bookkeeping backend really has finished.
             *
             * The values still increase, and still differ per queue, so a test can check that
             * points are ordered within a timeline and unordered across timelines without needing
             * hardware.
             */
            std::array<std::uint64_t, queue_kind_count> timelines{};

            /**
             * The staging budget, modelled but not allocated. Transfers complete immediately, so
             * `in_use` only ever holds what open batches have staged - which is enough for
             * `staging_exhausted` to be reachable, and therefore testable, with no GPU.
             */
            std::uint64_t staging_capacity = 0;
            std::uint64_t staging_in_use = 0;
        };

        struct buffer_state
        {
            resource_id owner = 0;
            buffer_desc desc;
            std::string debug_name;
            std::vector<std::byte> data;
        };

        struct shader_state
        {
            resource_id owner = 0;
            shader_stage stage = shader_stage::vertex;
            shader_bytecode_format bytecode_format = shader_bytecode_format::spirv;
            std::string entry_point;
            std::string debug_name;
            std::vector<std::byte> bytecode;
        };

        struct texture_state
        {
            resource_id owner = 0;
            texture_desc desc;
            std::string debug_name;
            /** Non-zero when the texture is a back buffer owned by that swapchain. */
            resource_id swapchain = 0;
        };

        struct sampler_state
        {
            resource_id owner = 0;
            sampler_desc desc;
        };

        struct pipeline_state
        {
            resource_id owner = 0;
            pipeline_type type = pipeline_type::graphics;
        };

        struct swapchain_state
        {
            resource_id owner = 0;
            swapchain_desc desc;
            std::string debug_name;
            std::vector<resource_id> images;
            std::uint32_t next_image = 0;
            bool acquired = false;
        };

        struct pending_copy
        {
            resource_id src = 0;
            std::size_t src_offset = 0;
            resource_id dst = 0;
            std::size_t dst_offset = 0;
            std::size_t size = 0;
        };

        struct command_pool_state
        {
            resource_id owner = 0;
            command_pool_desc desc;
            std::string debug_name;
            std::vector<resource_id> lists;
            /** True for the private pool a `create_command_list(device, ...)` list owns. */
            bool implicit = false;
        };

        struct command_list_state
        {
            resource_id owner = 0;
            command_list_desc desc;
            std::string debug_name;
            resource_id pool_id = 0;
            bool recording = false;
            bool ready = false;
            bool in_render_pass = false;
            resource_id bound_pipeline = 0;
            std::size_t command_count = 0;
            std::vector<pending_copy> copies;
            /** Recorded since the pool was last reset. Work here completes before `submit` returns,
             * so a list is never "in flight" - but the record-once-per-reset rule is the same one
             * the Vulkan backend enforces, and enforcing it here is how CI catches a caller
             * breaking it without a GPU. */
            bool recorded = false;
            timeline_point last_submit{};
        };

        /**
         * One open `transfer_batch`. There is no GPU, so an upload is a memcpy that has already
         * happened by the time it returns; what the batch still has to model faithfully is the
         * staging budget, so that `error_code::staging_exhausted` is reachable in a test.
         */
        struct transfer_batch_state
        {
            resource_id owner = 0;
            std::uint64_t staged = 0;
            std::size_t count = 0;
        };

        struct download_state
        {
            resource_id owner = 0;
            std::vector<std::byte> data;
        };

        // ---------------------------------------------------------------------
        // Registry
        // ---------------------------------------------------------------------

        resource_id g_next_id = 1;

        std::unordered_map<resource_id, device_state> g_devices;
        std::unordered_map<resource_id, buffer_state> g_buffers;
        std::unordered_map<resource_id, shader_state> g_shaders;
        std::unordered_map<resource_id, texture_state> g_textures;
        std::unordered_map<resource_id, sampler_state> g_samplers;
        std::unordered_map<resource_id, pipeline_state> g_pipelines;
        std::unordered_map<resource_id, swapchain_state> g_swapchains;
        std::unordered_map<resource_id, command_pool_state> g_command_pools;
        std::unordered_map<resource_id, command_list_state> g_command_lists;
        std::unordered_map<resource_id, transfer_batch_state> g_transfer_batches;
        std::unordered_map<resource_id, download_state> g_downloads;

        resource_id allocate_id() noexcept
        {
            return g_next_id++;
        }

        template <typename Map>
        typename Map::mapped_type *find(Map &map, resource_id id) noexcept
        {
            if (id == 0)
                return nullptr;
            auto it = map.find(id);
            return it == map.end() ? nullptr : &it->second;
        }

        template <typename Map>
        void erase_owned_by(Map &map, resource_id device)
        {
            std::erase_if(map, [device](const auto &entry) { return entry.second.owner == device; });
        }

        std::string copy_name(const char *name)
        {
            return name ? std::string{name} : std::string{};
        }

        const char *name_or_null(const std::string &name) noexcept
        {
            return name.empty() ? nullptr : name.c_str();
        }

        std::size_t texture_mip0_bytes(const texture_desc &desc) noexcept
        {
            // Block-aware: `format_size_bytes` is 0 for a compressed format, so the per-texel multiplication this
            // replaced would have sized every BC texture at nothing.
            return static_cast<std::size_t>(format_image_size_bytes(desc.pixel_format, desc.extent));
        }

        void create_swapchain_images(resource_id id, swapchain_state &sc)
        {
            sc.images.clear();
            sc.images.reserve(sc.desc.image_count);
            for (std::uint32_t i = 0; i < sc.desc.image_count; ++i)
            {
                texture_state t;
                t.owner = sc.owner;
                t.swapchain = id;
                t.desc.dimension = texture_dimension::texture_2d;
                t.desc.extent = {sc.desc.extent.width, sc.desc.extent.height, 1};
                t.desc.pixel_format = sc.desc.pixel_format;
                t.desc.usage = texture_usage::render_target | texture_usage::transfer_src | texture_usage::transfer_dst;
                t.debug_name = sc.debug_name.empty() ? std::string{} : sc.debug_name + " image " + std::to_string(i);

                const resource_id image = allocate_id();
                g_textures.emplace(image, std::move(t));
                sc.images.push_back(image);
            }
            sc.next_image = 0;
            sc.acquired = false;
        }

        void destroy_swapchain_images(swapchain_state &sc) noexcept
        {
            for (const resource_id image : sc.images)
                g_textures.erase(image);
            sc.images.clear();
        }

        /** The command list if it exists and is currently recording; null otherwise. */
        command_list_state *recording_list(resource_id id) noexcept
        {
            command_list_state *cl = find(g_command_lists, id);
            return (cl && cl->recording) ? cl : nullptr;
        }

        void record(command_list_state &cl) noexcept
        {
            ++cl.command_count;
        }

        bool buffer_has_usage(resource_id id, buffer_usage usage) noexcept
        {
            const buffer_state *b = find(g_buffers, id);
            return b && has_flag(b->desc.usage, usage);
        }

        bool range_in_bounds(std::size_t size, std::size_t offset, std::size_t length) noexcept
        {
            return offset <= size && length <= size - offset;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // Device
    // -------------------------------------------------------------------------

    resource_id create_device(const device_desc &desc)
    {
        device_state s;
        s.desc = desc;
        s.staging_capacity = desc.staging_ring_bytes != 0 ? desc.staging_ring_bytes : 16ull * 1024ull * 1024ull;
        s.application_name = copy_name(desc.application_name);

        const resource_id id = allocate_id();
        g_devices.emplace(id, std::move(s));
        return id;
    }

    void destroy_device(resource_id id) noexcept
    {
        if (!find(g_devices, id))
            return;

        erase_owned_by(g_downloads, id);
        erase_owned_by(g_transfer_batches, id);
        erase_owned_by(g_command_lists, id);
        erase_owned_by(g_command_pools, id);
        erase_owned_by(g_swapchains, id);
        erase_owned_by(g_pipelines, id);
        erase_owned_by(g_samplers, id);
        erase_owned_by(g_textures, id);
        erase_owned_by(g_shaders, id);
        erase_owned_by(g_buffers, id);
        g_devices.erase(id);
    }

    bool is_device_valid(resource_id id) noexcept
    {
        return find(g_devices, id) != nullptr;
    }

    device_info get_device_info(resource_id id) noexcept
    {
        device_info info;
        if (!find(g_devices, id))
            return info;
        info.backend = backend_type();
        info.adapter_name = "Catalyst bookkeeping adapter (no GPU work)";
        info.dedicated_video_memory_bytes = 0;
        return info;
    }

    bool is_device_lost(resource_id /*id*/) noexcept
    {
        // Nothing here can be removed, reset or hung by a driver, so this is honestly always false.
        return false;
    }

    void wait_idle(resource_id /*id*/) noexcept
    {
    }

    void collect_garbage(resource_id /*device*/) noexcept
    {
        // Resources are released the moment they are destroyed: nothing is ever in flight.
    }

    staging_info get_staging_info(resource_id id) noexcept
    {
        staging_info info;
        const device_state *dev = find(g_devices, id);
        if (!dev)
            return info;
        info.capacity_bytes = dev->staging_capacity;
        info.in_use_bytes = dev->staging_in_use;
        info.largest_transfer_bytes = dev->staging_capacity;
        return info;
    }

    // -------------------------------------------------------------------------
    // Queues and timelines
    // -------------------------------------------------------------------------

    queue_info get_queue_info(resource_id device, queue_kind kind) noexcept
    {
        queue_info info;
        info.kind = kind;
        if (!find(g_devices, device))
            return info;
        // One imaginary engine runs everything, so only graphics is its own. Reporting compute and
        // copy as aliased keeps the fallback path - the one a caller hits on adapters with no DMA
        // engine - exercised on every machine, including the ones in CI with no GPU at all.
        info.dedicated = kind == queue_kind::graphics;
        info.family_index = 0;
        return info;
    }

    std::uint64_t queue_last_submitted(resource_id device, queue_kind kind) noexcept
    {
        const device_state *dev = find(g_devices, device);
        return dev ? dev->timelines[static_cast<std::size_t>(kind)] : 0;
    }

    std::uint64_t queue_completed(resource_id device, queue_kind kind) noexcept
    {
        // Everything submitted has completed; see device_state::timelines.
        return queue_last_submitted(device, kind);
    }

    std::expected<void, error> queue_wait(resource_id device, queue_kind kind, std::uint64_t value,
                                          std::chrono::nanoseconds /*timeout*/) noexcept
    {
        if (value == 0)
            return {};

        const device_state *dev = find(g_devices, device);
        if (!dev)
            return std::unexpected(make_error(error_code::invalid_argument, "queue_wait"));

        // Naming work that was never submitted is a caller error rather than a wait, because
        // nothing will ever signal it -- the same answer the Vulkan backend gives, and the reason
        // this backend cannot just return success for every value. There is no GPU, so the wait
        // itself never blocks and the timeout never matters: `completed` and `last_submitted` are
        // the same number here (see device_state::timelines), which collapses Vulkan's two
        // comparisons into this one.
        if (value > dev->timelines[static_cast<std::size_t>(kind)])
            return std::unexpected(make_error(error_code::invalid_argument, "queue_wait"));

        return {};
    }

    // -------------------------------------------------------------------------
    // Buffers
    // -------------------------------------------------------------------------

    resource_id create_buffer(resource_id device, const buffer_desc &desc, std::span<const std::byte> initial_data)
    {
        if (!find(g_devices, device))
            return 0;

        buffer_state s;
        s.owner = device;
        s.desc = desc;
        s.debug_name = copy_name(desc.debug_name);
        s.data.resize(desc.size_bytes);
        if (!initial_data.empty())
            std::memcpy(s.data.data(), initial_data.data(), initial_data.size());

        const resource_id id = allocate_id();
        g_buffers.emplace(id, std::move(s));
        return id;
    }

    void destroy_buffer(resource_id id) noexcept
    {
        g_buffers.erase(id);
    }

    bool is_buffer_valid(resource_id id) noexcept
    {
        return find(g_buffers, id) != nullptr;
    }

    buffer_desc get_buffer_desc(resource_id id) noexcept
    {
        const buffer_state *b = find(g_buffers, id);
        if (!b)
            return {};
        buffer_desc d = b->desc;
        d.debug_name = name_or_null(b->debug_name);
        return d;
    }

    bool write_buffer(resource_id id, std::size_t offset, std::span<const std::byte> data) noexcept
    {
        buffer_state *b = find(g_buffers, id);
        if (!b || b->desc.access == memory_access::gpu_to_cpu)
            return false;
        if (!range_in_bounds(b->data.size(), offset, data.size()))
            return false;
        if (!data.empty())
            std::memcpy(b->data.data() + offset, data.data(), data.size());
        return true;
    }

    bool read_buffer(resource_id id, std::size_t offset, std::span<std::byte> out) noexcept
    {
        const buffer_state *b = find(g_buffers, id);
        if (!b || b->desc.access != memory_access::gpu_to_cpu)
            return false;
        if (!range_in_bounds(b->data.size(), offset, out.size()))
            return false;
        if (!out.empty())
            std::memcpy(out.data(), b->data.data() + offset, out.size());
        return true;
    }

    // -------------------------------------------------------------------------
    // Shaders
    // -------------------------------------------------------------------------

    resource_id create_shader(resource_id device, const shader_desc &desc)
    {
        if (!find(g_devices, device) || desc.bytecode.empty())
            return 0;

        shader_state s;
        s.owner = device;
        s.stage = desc.stage;
        s.bytecode_format = desc.bytecode_format;
        s.entry_point = copy_name(desc.entry_point);
        s.debug_name = copy_name(desc.debug_name);
        s.bytecode.assign(desc.bytecode.begin(), desc.bytecode.end());

        const resource_id id = allocate_id();
        g_shaders.emplace(id, std::move(s));
        return id;
    }

    void destroy_shader(resource_id id) noexcept
    {
        g_shaders.erase(id);
    }

    bool is_shader_valid(resource_id id) noexcept
    {
        return find(g_shaders, id) != nullptr;
    }

    std::span<const std::byte> get_bytecode(resource_id id) noexcept
    {
        const shader_state *s = find(g_shaders, id);
        if (!s)
            return {};
        return s->bytecode;
    }

    shader_stage get_shader_stage(resource_id id) noexcept
    {
        const shader_state *s = find(g_shaders, id);
        return s ? s->stage : shader_stage::vertex;
    }

    // -------------------------------------------------------------------------
    // Textures and samplers
    // -------------------------------------------------------------------------

    resource_id create_texture(resource_id device, const texture_desc &desc, std::span<const std::byte> initial_data)
    {
        if (!find(g_devices, device))
            return 0;
        if (!initial_data.empty() && initial_data.size() != texture_mip0_bytes(desc))
            return 0;

        texture_state s;
        s.owner = device;
        s.desc = desc;
        s.debug_name = copy_name(desc.debug_name);

        const resource_id id = allocate_id();
        g_textures.emplace(id, std::move(s));
        return id;
    }

    void destroy_texture(resource_id id) noexcept
    {
        const texture_state *t = find(g_textures, id);
        if (!t || t->swapchain != 0)
            return; // Swapchain images are owned by their swapchain.
        g_textures.erase(id);
    }

    bool is_texture_valid(resource_id id) noexcept
    {
        return find(g_textures, id) != nullptr;
    }

    texture_desc get_texture_desc(resource_id id) noexcept
    {
        const texture_state *t = find(g_textures, id);
        if (!t)
            return {};
        texture_desc d = t->desc;
        d.debug_name = name_or_null(t->debug_name);
        return d;
    }

    resource_id create_sampler(resource_id device, const sampler_desc &desc)
    {
        if (!find(g_devices, device))
            return 0;

        sampler_state s;
        s.owner = device;
        s.desc = desc;
        s.desc.debug_name = nullptr;

        const resource_id id = allocate_id();
        g_samplers.emplace(id, s);
        return id;
    }

    void destroy_sampler(resource_id id) noexcept
    {
        g_samplers.erase(id);
    }

    bool is_sampler_valid(resource_id id) noexcept
    {
        return find(g_samplers, id) != nullptr;
    }

    // -------------------------------------------------------------------------
    // Pipelines
    // -------------------------------------------------------------------------

    resource_id create_graphics_pipeline(resource_id device, const graphics_pipeline_desc &desc)
    {
        if (!find(g_devices, device))
            return 0;

        const shader_state *vs = find(g_shaders, desc.vertex_shader.id());
        if (!vs || vs->owner != device || vs->stage != shader_stage::vertex)
            return 0;

        if (desc.fragment_shader)
        {
            const shader_state *fs = find(g_shaders, desc.fragment_shader.id());
            if (!fs || fs->owner != device || fs->stage != shader_stage::fragment)
                return 0;
        }

        pipeline_state s;
        s.owner = device;
        s.type = pipeline_type::graphics;

        const resource_id id = allocate_id();
        g_pipelines.emplace(id, s);
        return id;
    }

    resource_id create_compute_pipeline(resource_id device, const compute_pipeline_desc &desc)
    {
        if (!find(g_devices, device))
            return 0;

        const shader_state *cs = find(g_shaders, desc.compute_shader.id());
        if (!cs || cs->owner != device || cs->stage != shader_stage::compute)
            return 0;

        pipeline_state s;
        s.owner = device;
        s.type = pipeline_type::compute;

        const resource_id id = allocate_id();
        g_pipelines.emplace(id, s);
        return id;
    }

    void destroy_pipeline(resource_id id) noexcept
    {
        g_pipelines.erase(id);
    }

    bool is_pipeline_valid(resource_id id) noexcept
    {
        return find(g_pipelines, id) != nullptr;
    }

    pipeline_type get_pipeline_type(resource_id id) noexcept
    {
        const pipeline_state *p = find(g_pipelines, id);
        return p ? p->type : pipeline_type::graphics;
    }

    // -------------------------------------------------------------------------
    // Swapchains
    // -------------------------------------------------------------------------

    resource_id create_swapchain(resource_id device, const swapchain_desc &desc)
    {
        if (!find(g_devices, device))
            return 0;

        swapchain_state s;
        s.owner = device;
        s.desc = desc;
        s.debug_name = copy_name(desc.debug_name);

        const resource_id id = allocate_id();
        auto [it, inserted] = g_swapchains.emplace(id, std::move(s));
        create_swapchain_images(id, it->second);
        return id;
    }

    void destroy_swapchain(resource_id id) noexcept
    {
        swapchain_state *sc = find(g_swapchains, id);
        if (!sc)
            return;
        destroy_swapchain_images(*sc);
        g_swapchains.erase(id);
    }

    bool is_swapchain_valid(resource_id id) noexcept
    {
        return find(g_swapchains, id) != nullptr;
    }

    swapchain_desc get_swapchain_desc(resource_id id) noexcept
    {
        const swapchain_state *sc = find(g_swapchains, id);
        if (!sc)
            return {};
        swapchain_desc d = sc->desc;
        d.debug_name = name_or_null(sc->debug_name);
        return d;
    }

    bool resize_swapchain(resource_id id, extent2d extent)
    {
        swapchain_state *sc = find(g_swapchains, id);
        if (!sc)
            return false;
        destroy_swapchain_images(*sc);
        sc->desc.extent = extent;
        create_swapchain_images(id, *sc);
        return true;
    }

    resource_id acquire_next_image(resource_id id)
    {
        swapchain_state *sc = find(g_swapchains, id);
        if (!sc || sc->images.empty())
            return 0;
        const resource_id image = sc->images[sc->next_image];
        sc->next_image = (sc->next_image + 1) % static_cast<std::uint32_t>(sc->images.size());
        sc->acquired = true;
        return image;
    }

    bool present(resource_id id)
    {
        swapchain_state *sc = find(g_swapchains, id);
        if (!sc || !sc->acquired)
            return false;
        sc->acquired = false;
        return true;
    }

    // -------------------------------------------------------------------------
    // Command lists
    // -------------------------------------------------------------------------

    resource_id create_command_pool(resource_id device, const command_pool_desc &desc)
    {
        if (!find(g_devices, device))
            return 0;

        command_pool_state s;
        s.owner = device;
        s.desc = desc;
        s.desc.debug_name = nullptr;
        s.debug_name = copy_name(desc.debug_name);

        const resource_id id = allocate_id();
        g_command_pools.emplace(id, std::move(s));
        return id;
    }

    void destroy_command_pool(resource_id id) noexcept
    {
        command_pool_state *pool = find(g_command_pools, id);
        if (!pool)
            return;
        for (const resource_id list_id : pool->lists)
            g_command_lists.erase(list_id);
        g_command_pools.erase(id);
    }

    bool is_command_pool_valid(resource_id id) noexcept
    {
        return find(g_command_pools, id) != nullptr;
    }

    command_pool_desc get_command_pool_desc(resource_id id) noexcept
    {
        const command_pool_state *pool = find(g_command_pools, id);
        if (!pool)
            return {};
        command_pool_desc desc = pool->desc;
        desc.debug_name = name_or_null(pool->debug_name);
        return desc;
    }

    resource_id get_command_pool_device(resource_id id) noexcept
    {
        const command_pool_state *pool = find(g_command_pools, id);
        return pool ? pool->owner : 0;
    }

    std::expected<void, error> reset_command_pool(resource_id id)
    {
        command_pool_state *pool = find(g_command_pools, id);
        if (!pool)
            return std::unexpected(make_error(error_code::invalid_argument, "reset_command_pool"));

        // Never `not_ready`: work completes before `submit` returns, so no list from this pool can
        // still be executing. The rest of the reset is the same as everywhere else.
        for (const resource_id list_id : pool->lists)
        {
            command_list_state *cl = find(g_command_lists, list_id);
            if (!cl)
                continue;
            cl->recording = false;
            cl->ready = false;
            cl->recorded = false;
            cl->in_render_pass = false;
            cl->command_count = 0;
            cl->copies.clear();
            cl->last_submit = {};
        }
        return {};
    }

    resource_id create_command_list_in_pool(resource_id pool_id, const char *debug_name)
    {
        command_pool_state *pool = find(g_command_pools, pool_id);
        if (!pool)
            return 0;

        command_list_state s;
        s.owner = pool->owner;
        s.desc.queue = pool->desc.queue;
        s.debug_name = copy_name(debug_name);
        s.pool_id = pool_id;

        const resource_id id = allocate_id();
        g_command_lists.emplace(id, std::move(s));
        pool->lists.push_back(id);
        return id;
    }

    resource_id create_command_list(resource_id device, const command_list_desc &desc)
    {
        if (!find(g_devices, device))
            return 0;

        // A private pool with one list in it, so `begin_recording` may recycle it by itself.
        command_pool_state pool;
        pool.owner = device;
        pool.desc.queue = desc.queue;
        pool.implicit = true;

        const resource_id pool_id = allocate_id();

        command_list_state s;
        s.owner = device;
        s.desc = desc;
        s.desc.debug_name = nullptr;
        s.debug_name = copy_name(desc.debug_name);
        s.pool_id = pool_id;

        const resource_id id = allocate_id();
        g_command_lists.emplace(id, std::move(s));
        pool.lists.push_back(id);
        g_command_pools.emplace(pool_id, std::move(pool));
        return id;
    }

    void destroy_command_list(resource_id id) noexcept
    {
        const command_list_state *cl = find(g_command_lists, id);
        if (!cl)
            return;
        const resource_id pool_id = cl->pool_id;
        g_command_lists.erase(id);

        if (command_pool_state *pool = find(g_command_pools, pool_id))
        {
            std::erase(pool->lists, id);
            if (pool->implicit && pool->lists.empty())
                g_command_pools.erase(pool_id);
        }
    }

    timeline_point command_list_last_submission(resource_id id) noexcept
    {
        const command_list_state *cl = find(g_command_lists, id);
        return cl ? cl->last_submit : timeline_point{};
    }

    bool is_command_list_valid(resource_id id) noexcept
    {
        return find(g_command_lists, id) != nullptr;
    }

    command_list_desc get_command_list_desc(resource_id id) noexcept
    {
        const command_list_state *cl = find(g_command_lists, id);
        if (!cl)
            return {};
        command_list_desc desc = cl->desc;
        desc.debug_name = name_or_null(cl->debug_name);
        return desc;
    }

    bool begin_recording(resource_id id)
    {
        command_list_state *cl = find(g_command_lists, id);
        if (!cl || cl->recording)
            return false;

        const command_pool_state *pool = find(g_command_pools, cl->pool_id);
        // Nothing is ever in flight here, so the only refusal left is the record-once-per-reset
        // rule - which is enforced anyway, because a caller who breaks it would find out on the
        // first machine with a GPU otherwise.
        if (pool && !pool->implicit && cl->recorded)
            return false;

        cl->recording = true;
        cl->ready = false;
        cl->recorded = true;
        cl->in_render_pass = false;
        cl->bound_pipeline = 0;
        cl->command_count = 0;
        cl->copies.clear();
        cl->last_submit = {};
        return true;
    }

    bool end_recording(resource_id id)
    {
        command_list_state *cl = recording_list(id);
        if (!cl || cl->in_render_pass)
            return false;
        cl->recording = false;
        cl->ready = true;
        return true;
    }

    bool is_recording(resource_id id) noexcept
    {
        return recording_list(id) != nullptr;
    }

    void begin_render_pass(resource_id id, const render_pass_desc &desc) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || cl->in_render_pass || !queue_accepts(cl->desc.queue, queue_kind::graphics))
            return;
        if (desc.color_attachments.size() > max_color_attachments)
            return;
        if (desc.color_attachments.empty() && !desc.depth_stencil.target)
            return;

        for (const color_attachment &a : desc.color_attachments)
        {
            const texture_state *t = find(g_textures, a.target.id());
            if (!t || !has_flag(t->desc.usage, texture_usage::render_target))
                return;
        }

        if (desc.depth_stencil.target)
        {
            const texture_state *t = find(g_textures, desc.depth_stencil.target.id());
            if (!t || !has_flag(t->desc.usage, texture_usage::depth_stencil))
                return;
        }

        cl->in_render_pass = true;
        record(*cl);
    }

    void end_render_pass(resource_id id) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || !cl->in_render_pass)
            return;
        cl->in_render_pass = false;
        record(*cl);
    }

    void set_pipeline(resource_id id, resource_id pipeline) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || !find(g_pipelines, pipeline))
            return;
        cl->bound_pipeline = pipeline;
        record(*cl);
    }

    void set_viewport(resource_id id, const viewport & /*vp*/) noexcept
    {
        if (command_list_state *cl = recording_list(id))
            record(*cl);
    }

    void set_scissor(resource_id id, const scissor_rect & /*rect*/) noexcept
    {
        if (command_list_state *cl = recording_list(id))
            record(*cl);
    }

    void set_vertex_buffer(resource_id id, std::uint32_t binding, resource_id buffer, std::size_t /*offset*/) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || binding >= max_vertex_bindings || !buffer_has_usage(buffer, buffer_usage::vertex))
            return;
        record(*cl);
    }

    void set_index_buffer(resource_id id, resource_id buffer, index_type /*type*/, std::size_t /*offset*/) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || !buffer_has_usage(buffer, buffer_usage::index))
            return;
        record(*cl);
    }

    void set_uniform_buffer(resource_id id, std::uint32_t /*slot*/, resource_id buffer, std::size_t /*offset*/,
                            std::size_t /*size*/) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || !buffer_has_usage(buffer, buffer_usage::uniform))
            return;
        record(*cl);
    }

    void set_storage_buffer(resource_id id, std::uint32_t /*slot*/, resource_id buffer, std::size_t /*offset*/,
                            std::size_t /*size*/) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || !buffer_has_usage(buffer, buffer_usage::storage))
            return;
        record(*cl);
    }

    void set_texture(resource_id id, std::uint32_t /*slot*/, resource_id texture) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || !find(g_textures, texture))
            return;
        record(*cl);
    }

    void set_sampler(resource_id id, std::uint32_t /*slot*/, resource_id sampler) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || !find(g_samplers, sampler))
            return;
        record(*cl);
    }

    void push_constants(resource_id id, std::uint32_t /*offset*/, std::span<const std::byte> /*data*/) noexcept
    {
        if (command_list_state *cl = recording_list(id))
            record(*cl);
    }

    void draw(resource_id id, std::uint32_t /*vertex_count*/, std::uint32_t /*instance_count*/,
              std::uint32_t /*first_vertex*/, std::uint32_t /*first_instance*/) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || !cl->in_render_pass)
            return;
        const pipeline_state *p = find(g_pipelines, cl->bound_pipeline);
        if (!p || p->type != pipeline_type::graphics)
            return;
        record(*cl);
    }

    void draw_indexed(resource_id id, std::uint32_t /*index_count*/, std::uint32_t /*instance_count*/,
                      std::uint32_t /*first_index*/, std::int32_t /*vertex_offset*/,
                      std::uint32_t /*first_instance*/) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || !cl->in_render_pass)
            return;
        const pipeline_state *p = find(g_pipelines, cl->bound_pipeline);
        if (!p || p->type != pipeline_type::graphics)
            return;
        record(*cl);
    }

    void dispatch(resource_id id, std::uint32_t /*x*/, std::uint32_t /*y*/, std::uint32_t /*z*/) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || cl->in_render_pass || !queue_accepts(cl->desc.queue, queue_kind::compute))
            return;
        const pipeline_state *p = find(g_pipelines, cl->bound_pipeline);
        if (!p || p->type != pipeline_type::compute)
            return;
        record(*cl);
    }

    void copy_buffer(resource_id id, resource_id src, std::size_t src_offset, resource_id dst, std::size_t dst_offset,
                     std::size_t size) noexcept
    {
        command_list_state *cl = recording_list(id);
        if (!cl || cl->in_render_pass)
            return;

        const buffer_state *s = find(g_buffers, src);
        const buffer_state *d = find(g_buffers, dst);
        if (!s || !d)
            return;
        if (!range_in_bounds(s->data.size(), src_offset, size) || !range_in_bounds(d->data.size(), dst_offset, size))
            return;

        cl->copies.push_back({src, src_offset, dst, dst_offset, size});
        record(*cl);
    }

    std::expected<std::uint64_t, error> submit(resource_id device, queue_kind kind,
                                               std::span<const command_list> lists,
                                               std::span<const timeline_point> /*waits*/)
    {
        device_state *dev = find(g_devices, device);
        if (!dev)
            return std::unexpected(make_error(error_code::invalid_argument, "submit"));

        for (const command_list &handle : lists)
        {
            const command_list_state *cl = find(g_command_lists, handle.id());
            if (!cl || cl->owner != device || !cl->ready || cl->recording)
                return std::unexpected(make_error(error_code::invalid_argument, "submit"));
        }

        // `waits` needs nothing: work completes before this function returns, so every dependency
        // named by a point on this device is already satisfied by the time it would be checked.

        // "Execute": the only observable work the bookkeeping backend performs is buffer-to-buffer copies.
        for (const command_list &handle : lists)
        {
            const command_list_state *cl = find(g_command_lists, handle.id());
            for (const pending_copy &c : cl->copies)
            {
                const buffer_state *s = find(g_buffers, c.src);
                buffer_state *d = find(g_buffers, c.dst);
                if (!s || !d)
                    continue;
                if (!range_in_bounds(s->data.size(), c.src_offset, c.size) ||
                    !range_in_bounds(d->data.size(), c.dst_offset, c.size))
                    continue;
                std::memmove(d->data.data() + c.dst_offset, s->data.data() + c.src_offset, c.size);
            }
        }

        const std::uint64_t value = ++dev->timelines[static_cast<std::size_t>(kind)];
        for (const command_list &handle : lists)
        {
            if (command_list_state *cl = find(g_command_lists, handle.id()))
                cl->last_submit = timeline_point{rendering::device{device}, kind, value};
        }
        return value;
    }

    // -------------------------------------------------------------------------
    // Transfers
    //
    // There is no GPU, so a transfer is a memcpy and the point it completes at is the one it was
    // submitted at. What is still modelled honestly is the staging budget and the ordering
    // vocabulary, because those are what a caller writes code against.
    // -------------------------------------------------------------------------

    resource_id begin_transfer_batch(resource_id device)
    {
        if (!find(g_devices, device))
            return 0;

        transfer_batch_state s;
        s.owner = device;

        const resource_id id = allocate_id();
        g_transfer_batches.emplace(id, s);
        return id;
    }

    void discard_transfer_batch(resource_id id) noexcept
    {
        transfer_batch_state *batch = find(g_transfer_batches, id);
        if (!batch)
            return;
        if (device_state *dev = find(g_devices, batch->owner))
            dev->staging_in_use -= std::min(dev->staging_in_use, batch->staged);
        g_transfer_batches.erase(id);
    }

    std::size_t transfer_batch_staged_bytes(resource_id id) noexcept
    {
        const transfer_batch_state *batch = find(g_transfer_batches, id);
        return batch ? static_cast<std::size_t>(batch->staged) : 0;
    }

    std::size_t transfer_batch_size(resource_id id) noexcept
    {
        const transfer_batch_state *batch = find(g_transfer_batches, id);
        return batch ? batch->count : 0;
    }

    namespace
    {
        /** Charges `bytes` against the device's staging budget, or reports it exhausted. */
        std::expected<void, error> take_staging(device_state &dev, transfer_batch_state &batch, std::size_t bytes)
        {
            if (dev.staging_in_use + bytes > dev.staging_capacity)
                return std::unexpected(make_error(error_code::staging_exhausted, "transfer_batch::upload"));
            dev.staging_in_use += bytes;
            batch.staged += bytes;
            return {};
        }
    } // namespace

    std::expected<void, error> transfer_upload_buffer(resource_id id, resource_id dst, std::size_t offset,
                                                      std::span<const std::byte> data)
    {
        transfer_batch_state *batch = find(g_transfer_batches, id);
        if (!batch)
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::upload"));

        device_state *dev = find(g_devices, batch->owner);
        buffer_state *b = find(g_buffers, dst);
        if (!dev || !b || b->owner != batch->owner)
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::upload"));
        if (!range_in_bounds(b->data.size(), offset, data.size()))
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::upload"));
        if (b->desc.access == memory_access::gpu_to_cpu)
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::upload"));

        if (auto ok = take_staging(*dev, *batch, data.size()); !ok)
            return ok;

        std::memcpy(b->data.data() + offset, data.data(), data.size());
        ++batch->count;
        return {};
    }

    std::expected<void, error> transfer_upload_texture(resource_id id, resource_id dst,
                                                       std::span<const std::byte> data)
    {
        transfer_batch_state *batch = find(g_transfer_batches, id);
        if (!batch)
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::upload"));

        device_state *dev = find(g_devices, batch->owner);
        const texture_state *t = find(g_textures, dst);
        if (!dev || !t || t->owner != batch->owner || t->swapchain != 0)
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::upload"));
        if (data.size() != texture_mip0_bytes(t->desc))
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::upload"));

        // Texture contents are not kept - there is nothing that could read them back - so the size
        // check above is the whole of what this can honestly verify.
        if (auto ok = take_staging(*dev, *batch, data.size()); !ok)
            return ok;
        ++batch->count;
        return {};
    }

    std::expected<std::uint64_t, error> submit_transfer_batch(resource_id id)
    {
        transfer_batch_state *batch = find(g_transfer_batches, id);
        if (!batch)
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::submit"));

        device_state *dev = find(g_devices, batch->owner);
        if (!dev)
            return std::unexpected(make_error(error_code::invalid_argument, "transfer_batch::submit"));

        // An empty batch reports the "no work" value, exactly as a real backend does when every
        // upload went straight into mapped memory.
        const bool empty = batch->count == 0;

        dev->staging_in_use -= std::min(dev->staging_in_use, batch->staged);
        batch->staged = 0;
        batch->count = 0;
        if (empty)
            return 0ull;

        return ++dev->timelines[static_cast<std::size_t>(queue_kind::copy)];
    }

    std::expected<resource_id, error> begin_download(resource_id device, resource_id src, std::size_t offset,
                                                     std::size_t size, std::span<const timeline_point> /*after*/,
                                                     std::uint64_t &out_value)
    {
        device_state *dev = find(g_devices, device);
        const buffer_state *b = find(g_buffers, src);
        if (!dev || !b || b->owner != device)
            return std::unexpected(make_error(error_code::invalid_argument, "download"));
        if (!range_in_bounds(b->data.size(), offset, size))
            return std::unexpected(make_error(error_code::invalid_argument, "download"));
        if (!has_flag(b->desc.usage, buffer_usage::transfer_src))
            return std::unexpected(make_error(error_code::invalid_argument, "download"));

        download_state d;
        d.owner = device;
        d.data.assign(b->data.begin() + static_cast<std::ptrdiff_t>(offset),
                      b->data.begin() + static_cast<std::ptrdiff_t>(offset + size));

        out_value = ++dev->timelines[static_cast<std::size_t>(queue_kind::copy)];

        const resource_id id = allocate_id();
        g_downloads.emplace(id, std::move(d));
        return id;
    }

    std::expected<std::span<const std::byte>, error> download_bytes(resource_id id)
    {
        const download_state *d = find(g_downloads, id);
        if (!d)
            return std::unexpected(make_error(error_code::invalid_argument, "readback::bytes"));
        return std::span<const std::byte>{d->data};
    }

    void destroy_download(resource_id id) noexcept
    {
        g_downloads.erase(id);
    }

} // namespace catalyst::rendering::detail
