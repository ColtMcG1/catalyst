/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Internal header of the Vulkan rendering backend: the per-resource state records, the id registry and the
 * helpers shared between the translation units that implement src/rendering/detail_backend.hpp on top of Vulkan 1.3.
 * @details Design in one paragraph so the individual files make sense:
 *   - One `VkInstance` + `VkDevice` per `device`, with up to three queues taken from distinct families.
 *   - Render passes use dynamic rendering (core 1.3): no VkRenderPass / VkFramebuffer objects.
 *   - Image layouts follow a fixed invariant instead of being tracked: user textures always sit in
 *     `VK_IMAGE_LAYOUT_GENERAL`, presentable swapchain images in `VK_IMAGE_LAYOUT_PRESENT_SRC_KHR` outside a render
 *     pass and `COLOR_ATTACHMENT_OPTIMAL` inside one. Hazards are covered by a conservative full barrier before each
 *     render pass, dispatch and copy.
 *   - Resource binding uses one pipeline layout for every pipeline with four descriptor sets: uniform buffers (set 0),
 *     storage buffers (set 1), sampled textures (set 2) and samplers (set 3); the public `slot` is the binding index.
 *     Sets are partially bound, so unbound slots are simply not written. 128 bytes of push constants are visible to all
 *     stages.
 *   - Three queues - graphics, compute, copy - taken from distinct families where the adapter has them and aliased
 *     onto graphics where it does not. Each has its own timeline semaphore, whose value is what a public
 *     `timeline_point` names. Destroying a resource while work is in flight defers the Vulkan release until every
 *     queue has passed the value it stood at when the destruction was asked for.
 *   - Transfers are asynchronous. Uploads are staged into a ring (`staging_ring`), recorded into a transfer command
 *     buffer and submitted on the copy queue; nothing waits. The "immediate" command buffer survives only for the two
 *     places that genuinely have to be synchronous - swapchain image setup, and teardown.
 *   - Buffers and images are created with `VK_SHARING_MODE_CONCURRENT` across the distinct queue families when the
 *     adapter has more than one. Exclusive sharing would require an explicit ownership transfer every time a resource
 *     crossed engines, which nothing here tracks; concurrent is the correct-by-construction choice for a backend whose
 *     hazard model is still one full barrier per pass. Tier 6's explicit resource states is where that gets narrowed.
 *   - Buffers, images and command lists are looked up through one process-wide registry. It is not internally
 *     synchronised: the public API layer holds the module lock (src/rendering/detail_sync.hpp) across every call into
 *     this backend, shared for reads and recording, exclusive for creation, destruction and submission. See
 *     src/rendering/detail_backend.hpp for what a backend may assume.
 * Not part of the public API.
 */

#pragma once

#if defined(_WIN32)
#ifndef VK_USE_PLATFORM_WIN32_KHR
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#endif

#include <vulkan/vulkan.h>

#include "../detail_backend.hpp"
#include "../detail_log.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <vector>

namespace catalyst::rendering::detail::vulkan
{

    // -------------------------------------------------------------------------
    // Binding model
    // -------------------------------------------------------------------------

    inline constexpr std::uint32_t set_uniform_buffers = 0;
    inline constexpr std::uint32_t set_storage_buffers = 1;
    inline constexpr std::uint32_t set_textures = 2;
    inline constexpr std::uint32_t set_samplers = 3;
    inline constexpr std::uint32_t descriptor_set_count = 4;

    /** Descriptor sets allocated per pool before a command list grows a new one. */
    inline constexpr std::uint32_t descriptor_sets_per_pool = 256;

    // -------------------------------------------------------------------------
    // Resource records
    // -------------------------------------------------------------------------

    /**
     * One engine, and the timeline semaphore that tracks it. `queue_kind::graphics` is always a real
     * queue; the other two are real where the adapter exposes a family for them and aliases of
     * graphics where it does not - `dedicated` is which.
     *
     * Each kind gets its own timeline even when two of them share a `VkQueue`, so a `timeline_point`
     * means the same thing regardless of what the adapter happened to offer.
     */
    struct queue_state
    {
        VkQueue queue = VK_NULL_HANDLE;
        std::uint32_t family = 0;
        bool dedicated = false;
        VkSemaphore timeline = VK_NULL_HANDLE;
        /** Highest value submitted; the next submission signals this plus one. */
        std::uint64_t last_submitted = 0;
        /** Highest value the GPU is known to have reached. Only ever moves forward. */
        std::uint64_t completed = 0;
    };

    /**
     * A resource destroyed while work was still in flight. It is released once every queue has
     * passed the value it had when the destruction was requested - all three, because a texture does
     * not record which engines were reading it.
     */
    struct deferred_release
    {
        std::array<std::uint64_t, queue_kind_count> points{};
        std::function<void()> release;
    };

    struct device_features
    {
        bool sampler_anisotropy = false;
        bool fill_mode_non_solid = false;
        bool depth_clamp = false;
    };

    /**
     * Persistently mapped transfer-source buffer. One is owned by each device and grown on demand rather than
     * allocated per transfer: `vkAllocateMemory` / `vkFreeMemory` dominate the cost of a small staged upload.
     *
     * Used now only by the two paths that are still synchronous by nature - swapchain image setup and teardown. The
     * transfer path uses `staging_ring` instead.
     */
    struct staging_buffer
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void *mapped = nullptr;
        VkDeviceSize capacity = 0;
        bool coherent = true;
    };

    /**
     * The device's upload ring: one persistently mapped host-visible allocation, carved front to back and recycled
     * behind the copy queue's timeline.
     *
     * `head` and `tail` are absolute byte counters that only ever increase, so `head - tail` is what is in use and the
     * physical offset of a byte is `absolute % capacity`. An allocation that would straddle the end of the buffer pads
     * to the start instead, because a `vkCmdCopyBuffer` region has to be contiguous.
     *
     * The old design was one buffer grown on demand, with the constraint stated outright in its own comment: the
     * immediate command buffer is submitted and waited on before control returns, so successive transfers cannot
     * overlap. That is what this replaces - and when the ring is full of bytes the GPU has not read yet, the answer is
     * `error_code::staging_exhausted` rather than a hidden stall.
     */
    struct staging_ring
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        std::byte *mapped = nullptr;
        VkDeviceSize capacity = 0;
        VkDeviceSize alignment = 16;
        bool coherent = true;

        /** Absolute counters. `head - tail` bytes are in use; neither ever moves backwards except on discard. */
        std::uint64_t head = 0;
        std::uint64_t tail = 0;

        /** One submitted batch: everything below `end` is free once the copy queue passes `point`. */
        struct block
        {
            std::uint64_t end = 0;
            std::uint64_t point = 0;
        };

        /**
         * Submitted blocks, kept sorted by `end`, retired from the front. Sorted rather than appended because two
         * batches open at once may submit in either order; retiring only a prefix then frees a little later than it
         * strictly could, which is the safe direction.
         */
        std::vector<block> in_flight;
    };

    /**
     * Command buffers for transfer submissions, recycled by the copy queue's timeline. The pool carries
     * `RESET_COMMAND_BUFFER_BIT` so one buffer can be recycled without touching the others - which is exactly what a
     * ring of in-flight transfers needs and what a pool-wide reset cannot give.
     */
    struct transfer_context
    {
        VkCommandPool pool = VK_NULL_HANDLE;

        struct entry
        {
            VkCommandBuffer cmd = VK_NULL_HANDLE;
            /** Copy-queue value that frees it; 0 when never submitted. */
            std::uint64_t point = 0;
            /** Held by an open batch. */
            bool busy = false;
        };

        std::vector<entry> buffers;
    };

    /** One open `transfer_batch`: a command buffer being recorded into, and the ring range it has staged. */
    struct transfer_batch_state
    {
        resource_id owner = 0;
        std::size_t entry = 0;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        bool recording = false;

        /** Absolute ring range this batch has taken. */
        std::uint64_t begin_offset = 0;
        std::uint64_t end_offset = 0;

        std::size_t staged_bytes = 0;
        std::size_t count = 0;

        /**
         * Per queue: true when a target of this batch could still be being read by work already submitted there, so
         * the transfer has to be ordered after it. False for a resource nothing has been submitted against since it
         * was created - the level-load case, which is the one where overlapping with the frame actually matters.
         */
        std::array<bool, queue_kind_count> hazard{};
    };

    /** One in-flight `readback`: the host-visible buffer a download lands in. */
    struct download_state
    {
        resource_id owner = 0;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void *mapped = nullptr;
        VkDeviceSize size = 0;
        bool coherent = true;
        timeline_point point{};
        /** Set the first time the bytes are read, so a non-coherent mapping is invalidated once. */
        bool invalidated = false;
    };

    struct device_state
    {
        device_desc desc;
        std::string application_name;

        VkInstance instance = VK_NULL_HANDLE;
        VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
        VkPhysicalDevice physical_device = VK_NULL_HANDLE;
        VkDevice device = VK_NULL_HANDLE;
        std::array<queue_state, queue_kind_count> queues{};

        /**
         * Latched the first time the driver reports VK_ERROR_DEVICE_LOST, and never cleared. Every
         * entry point checks it and fails fast, because after device loss a wait would hang rather
         * than fail and a submit would be rejected over and over.
         */
        bool lost = false;

        VkPhysicalDeviceProperties properties{};
        VkPhysicalDeviceMemoryProperties memory_properties{};
        std::string adapter_name;
        std::uint64_t dedicated_video_memory = 0;
        device_features features;
        /** What `format::d24_unorm_s8_uint` resolves to on this adapter (itself, or d32_float_s8_uint). */
        format depth_stencil_format = format::d24_unorm_s8_uint;

        bool debug_utils = false;
        PFN_vkSetDebugUtilsObjectNameEXT set_object_name = nullptr;

        std::array<VkDescriptorSetLayout, descriptor_set_count> set_layouts{};
        VkPipelineLayout pipeline_layout = VK_NULL_HANDLE;

        std::vector<deferred_release> garbage;

        VkCommandPool immediate_pool = VK_NULL_HANDLE;
        VkCommandBuffer immediate_cmd = VK_NULL_HANDLE;
        /** Reused upload buffer for the synchronous paths that remain; only ever touched by the immediate buffer. */
        staging_buffer staging;

        /** Asynchronous transfers: the upload ring and the command buffers that carry it. */
        staging_ring ring;
        transfer_context transfers;

        /**
         * The copy-queue value of the most recent transfer submission. Every later submission on another queue waits
         * on it, on the GPU, so `write_buffer` followed by a draw is correct with no caller-side ordering and no CPU
         * round trip. Skipped once the copy queue has passed it, which is the common case by the next frame.
         */
        std::uint64_t last_transfer = 0;

        /** The distinct queue families this device uses, for `VK_SHARING_MODE_CONCURRENT`. */
        std::vector<std::uint32_t> families;

        /**
         * True when device-local memory is host-visible, as on integrated adapters where the device-local heap is
         * system RAM. `gpu_only` buffers are then mapped and written directly instead of through a staging copy.
         */
        bool unified_memory = false;

        /** Swapchains holding an acquired image whose acquire semaphore no submission has waited on yet. */
        std::vector<resource_id> pending_acquires;
    };

    /**
     * Per queue, the timeline value that queue stood at when a resource was created. Nothing submitted at or before
     * that value can possibly reference it, which is how a transfer into a brand-new resource skips the
     * write-after-read wait that a transfer into a live one needs.
     */
    using creation_marks = std::array<std::uint64_t, queue_kind_count>;

    struct buffer_state
    {
        resource_id owner = 0;
        buffer_desc desc;
        std::string debug_name;
        creation_marks created{};
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        /**
         * Persistent mapping when the allocation landed in host-visible memory, else null. `gpu_only` buffers are
         * mapped too on unified-memory adapters, which is what lets `write_buffer` avoid staging there.
         */
        void *mapped = nullptr;
        bool coherent = true;
    };

    struct shader_state
    {
        resource_id owner = 0;
        shader_stage stage = shader_stage::vertex;
        shader_bytecode_format bytecode_format = shader_bytecode_format::spirv;
        std::string entry_point;
        std::string debug_name;
        std::vector<std::byte> bytecode;
        VkShaderModule module = VK_NULL_HANDLE;
    };

    struct texture_state
    {
        resource_id owner = 0;
        texture_desc desc;
        std::string debug_name;
        creation_marks created{};
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        /** View covering every aspect; used for attachments. */
        VkImageView view = VK_NULL_HANDLE;
        /** Depth-only view for depth/stencil formats, otherwise equal to `view`; used for sampling. */
        VkImageView sampled_view = VK_NULL_HANDLE;
        VkFormat vk_format = VK_FORMAT_UNDEFINED;
        VkImageAspectFlags aspect = 0;
        /** Non-zero when the texture is a back buffer owned by that swapchain. */
        resource_id swapchain = 0;
        /** True when `image` belongs to a VkSwapchainKHR (and lives in PRESENT_SRC outside render passes). */
        bool presentable = false;
        std::uint32_t image_index = 0;
    };

    struct sampler_state
    {
        resource_id owner = 0;
        sampler_desc desc;
        VkSampler sampler = VK_NULL_HANDLE;
    };

    struct pipeline_state
    {
        resource_id owner = 0;
        pipeline_type type = pipeline_type::graphics;
        VkPipeline pipeline = VK_NULL_HANDLE;
    };

    struct swapchain_state
    {
        resource_id owner = 0;
        swapchain_desc desc;
        std::string debug_name;

        /** False for window-less swapchains, which emulate presentation with a ring of ordinary render targets. */
        bool windowed = false;
        VkSurfaceKHR surface = VK_NULL_HANDLE;
        VkSwapchainKHR swapchain = VK_NULL_HANDLE;
        VkFormat vk_format = VK_FORMAT_UNDEFINED;
        VkColorSpaceKHR color_space = VK_COLOR_SPACE_SRGB_NONLINEAR_KHR;
        VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;

        /** Texture ids of the back buffers, in image-index order. */
        std::vector<resource_id> images;

        /** Ring of acquire semaphores and the submission that consumed each. */
        std::vector<VkSemaphore> acquire_semaphores;
        std::vector<timeline_point> acquire_points;
        std::uint32_t acquire_slot = 0;
        /** Per image: signalled by the submission that consumed the acquire, waited on by the present. */
        std::vector<VkSemaphore> render_finished;
        /** Per image: used when work was submitted after `render_finished` was signalled (see `present`). */
        std::vector<VkSemaphore> present_ready;

        std::uint32_t current_image = 0;
        bool acquired = false;
        /** The current image's acquire semaphore has not been waited on by any submission yet. */
        bool acquire_pending = false;
        /** The submission that signalled `render_finished[current_image]`. */
        timeline_point render_finished_point{};
        /** Set when acquire / present reported the surface changed; `acquire_next_image` fails until a resize. */
        bool out_of_date = false;

        /** Window-less swapchains: next image to hand out. */
        std::uint32_t next_image = 0;
    };

    struct buffer_binding
    {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceSize offset = 0;
        VkDeviceSize range = 0;
    };

    /**
     * A `VkCommandPool` and the lists allocated from it. The unit of recycling - `reset_command_pool` resets the whole
     * pool, which is what makes every list from it recordable again - and the unit of thread affinity, since a pool is
     * externally synchronised by the Vulkan specification and the public API says one thread owns it.
     */
    struct command_pool_state
    {
        resource_id owner = 0;
        command_pool_desc desc;
        std::string debug_name;

        VkCommandPool pool = VK_NULL_HANDLE;
        std::vector<resource_id> lists;

        /** True for the private pool that a `create_command_list(device, ...)` list owns, which `begin_recording` may
         * recycle by itself because there is nothing else in it. */
        bool implicit = false;
    };

    struct command_list_state
    {
        resource_id owner = 0;
        command_list_desc desc;
        std::string debug_name;

        /** The pool this list was allocated from; never 0. */
        resource_id pool_id = 0;
        VkCommandPool pool = VK_NULL_HANDLE;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        std::vector<VkDescriptorPool> descriptor_pools;
        std::size_t active_pool = 0;

        bool recording = false;
        bool ready = false;
        bool in_render_pass = false;
        resource_id bound_pipeline = 0;
        pipeline_type bound_pipeline_type = pipeline_type::graphics;
        /**
         * The last submission that included this list; invalid when never submitted, and cleared when the pool is
         * reset. `begin_recording` refuses while this is outstanding rather than waiting for it - see Tier 3.
         */
        timeline_point last_submit{};

        /** Recorded since the pool was last reset, so it may not be recorded again until it is. */
        bool recorded = false;

        /** Presentable images bound as attachments of the open render pass (transitioned back at end). */
        std::vector<VkImage> pass_present_images;

        std::array<buffer_binding, max_uniform_buffer_slots> uniform_buffers{};
        std::array<buffer_binding, max_storage_buffer_slots> storage_buffers{};
        std::array<VkImageView, max_texture_slots> textures{};
        std::array<VkSampler, max_sampler_slots> samplers{};
        /** Per set: bindings changed since the set was last allocated. */
        std::array<bool, descriptor_set_count> dirty{};
        /** Per set: the most recently allocated descriptor set (null until first use). */
        std::array<VkDescriptorSet, descriptor_set_count> sets{};
        /** Per set: whether `sets[i]` is currently bound to the graphics / compute bind point. */
        std::array<bool, descriptor_set_count> bound_graphics{};
        std::array<bool, descriptor_set_count> bound_compute{};
    };

    // -------------------------------------------------------------------------
    // Registry
    // -------------------------------------------------------------------------

    struct registry
    {
        resource_id next_id = 1;
        std::unordered_map<resource_id, device_state> devices;
        std::unordered_map<resource_id, buffer_state> buffers;
        std::unordered_map<resource_id, shader_state> shaders;
        std::unordered_map<resource_id, texture_state> textures;
        std::unordered_map<resource_id, sampler_state> samplers;
        std::unordered_map<resource_id, pipeline_state> pipelines;
        std::unordered_map<resource_id, swapchain_state> swapchains;
        std::unordered_map<resource_id, command_pool_state> command_pools;
        std::unordered_map<resource_id, command_list_state> command_lists;
        std::unordered_map<resource_id, transfer_batch_state> transfer_batches;
        std::unordered_map<resource_id, download_state> downloads;
    };

    registry &reg() noexcept;
    resource_id allocate_id() noexcept;

    template <typename Map>
    typename Map::mapped_type *find(Map &map, resource_id id) noexcept
    {
        if (id == 0)
            return nullptr;
        auto it = map.find(id);
        return it == map.end() ? nullptr : &it->second;
    }

    inline device_state *find_device(resource_id id) noexcept
    {
        return find(reg().devices, id);
    }

    inline std::string copy_name(const char *name)
    {
        return name ? std::string{name} : std::string{};
    }

    inline const char *name_or_null(const std::string &name) noexcept
    {
        return name.empty() ? nullptr : name.c_str();
    }

    inline bool range_in_bounds(std::size_t size, std::size_t offset, std::size_t length) noexcept
    {
        return offset <= size && length <= size - offset;
    }

    // -------------------------------------------------------------------------
    // Texture geometry
    //
    // In the header rather than in vulkan_texture.cpp because the transfer path needs the same
    // three answers when it records a buffer-to-image copy.
    // -------------------------------------------------------------------------

    /** Tightly packed size of mip 0, layer 0. Block-aware: a compressed format rounds up to whole blocks. */
    inline std::size_t mip0_bytes(const texture_desc &desc) noexcept
    {
        return static_cast<std::size_t>(format_image_size_bytes(desc.pixel_format, desc.extent));
    }

    inline VkExtent3D image_extent(const texture_desc &desc) noexcept
    {
        VkExtent3D e{desc.extent.width, desc.extent.height, desc.extent.depth};
        if (desc.dimension == texture_dimension::texture_1d)
            e.height = 1;
        if (desc.dimension != texture_dimension::texture_3d)
            e.depth = 1;
        e.width = e.width ? e.width : 1u;
        e.height = e.height ? e.height : 1u;
        e.depth = e.depth ? e.depth : 1u;
        return e;
    }

    inline std::uint32_t layer_count(const texture_desc &desc) noexcept
    {
        if (desc.dimension == texture_dimension::texture_3d)
            return 1u;
        return desc.array_layers ? desc.array_layers : 1u;
    }

    /** Vulkan handle as the 64-bit integer VK_EXT_debug_utils expects (handles are pointers on 64-bit targets). */
    template <typename Handle>
    std::uint64_t handle_bits(Handle h) noexcept
    {
        if constexpr (std::is_pointer_v<Handle>)
            return reinterpret_cast<std::uint64_t>(h);
        else
            return static_cast<std::uint64_t>(h);
    }

    // -------------------------------------------------------------------------
    // Diagnostics (vulkan_device.cpp)
    // -------------------------------------------------------------------------

    /** Spells a VkResult the way the API does ("VK_ERROR_DEVICE_LOST"). Static storage, never null. */
    const char *result_string(VkResult result) noexcept;
    void set_debug_name(device_state &dev, VkObjectType type, std::uint64_t handle, const char *name) noexcept;

    inline void set_debug_name(device_state &dev, VkObjectType type, std::uint64_t handle,
                               const std::string &name) noexcept
    {
        if (!name.empty())
            set_debug_name(dev, type, handle, name.c_str());
    }

    // -------------------------------------------------------------------------
    // Submission tracking (vulkan_device.cpp)
    // -------------------------------------------------------------------------

    /** The engine of a given kind. */
    queue_state &queue_for(device_state &dev, queue_kind kind) noexcept;
    const queue_state &queue_for(const device_state &dev, queue_kind kind) noexcept;

    /**
     * Maps a VkResult onto the public `error`, filling in the driver's own spelling of it. Latches
     * `dev.lost` for VK_ERROR_DEVICE_LOST, so this is the single place device loss is noticed.
     */
    error to_error(device_state &dev, VkResult result, const char *operation) noexcept;

    /** One submission. Binary semaphores are the swapchain's; timeline waits are cross-queue deps. */
    struct submit_batch_info
    {
        queue_kind kind = queue_kind::graphics;
        std::span<const VkCommandBuffer> commands;
        std::span<const VkSemaphore> binary_waits;
        std::span<const VkPipelineStageFlags> wait_stages;
        std::span<const VkSemaphore> binary_signals;
        std::span<const timeline_point> timeline_waits;
    };

    /** Submits on `batch.kind` and returns the timeline value the submission will signal. */
    std::expected<std::uint64_t, error> submit_batch(device_state &dev, const submit_batch_info &batch) noexcept;

    /** Reads every queue's timeline counter. Never blocks. */
    void refresh_completed(device_state &dev) noexcept;

    /** Runs the deferred releases every queue has now passed. */
    void collect_garbage(device_state &dev) noexcept;

    /** Blocks until `kind`'s timeline reaches `value`, or `timeout` elapses. */
    std::expected<void, error> wait_timeline(device_state &dev, queue_kind kind, std::uint64_t value,
                                             std::chrono::nanoseconds timeout) noexcept;

    /**
     * An unbounded wait for a point, discarding the outcome. Used where the old code called
     * `wait_for_serial` and had nothing to do about a failure there either.
     */
    inline void wait_point(device_state &dev, const timeline_point &point) noexcept
    {
        if (point.valid())
            (void)wait_timeline(dev, point.queue(), point.value(), std::chrono::nanoseconds::max());
    }

    /** Blocks until the device is idle. */
    void wait_all(device_state &dev) noexcept;

    /** Runs `release` now if nothing is in flight, otherwise once every current submission has completed. */
    void defer_release(device_state &dev, std::function<void()> release);

    /**
     * Begins the device's immediate command buffer. Pair with `end_immediate`, which submits it and waits for
     * completion. Returns VK_NULL_HANDLE on failure.
     */
    VkCommandBuffer begin_immediate(device_state &dev) noexcept;
    bool end_immediate(device_state &dev) noexcept;

    /** Records an all-commands / all-memory pipeline barrier. */
    void full_barrier(VkCommandBuffer cmd) noexcept;

    /** Substitutes formats the adapter cannot use (currently only the depth/stencil pair). */
    format resolve_format(const device_state &dev, format f) noexcept;

    // -------------------------------------------------------------------------
    // Memory (vulkan_memory.cpp)
    // -------------------------------------------------------------------------

    bool find_memory_type(const device_state &dev, std::uint32_t type_bits, VkMemoryPropertyFlags required,
                          VkMemoryPropertyFlags preferred, std::uint32_t &out_index) noexcept;

    /** Allocates and binds memory for `buffer` according to `access`; maps it persistently when host-visible. */
    bool allocate_buffer_memory(device_state &dev, VkBuffer buffer, memory_access access, VkDeviceMemory &out_memory,
                                void *&out_mapped, bool &out_coherent) noexcept;

    /** Allocates and binds device-local memory for `image`. */
    bool allocate_image_memory(device_state &dev, VkImage image, VkDeviceMemory &out_memory) noexcept;

    /** Flush / invalidate the whole mapped range of a non-coherent allocation (no-ops are cheap enough to skip checks). */
    void flush_host_writes(device_state &dev, VkDeviceMemory memory) noexcept;
    void invalidate_host_reads(device_state &dev, VkDeviceMemory memory) noexcept;

    /**
     * Copies `data` into the device's staging buffer (growing it first if needed) and reports the buffer to use as
     * the transfer source. Valid until the next staged transfer on this device; the immediate command buffer is
     * submitted and waited on before control returns to the caller, so successive transfers cannot overlap.
     *
     * The synchronous path, kept for swapchain setup. Everything a caller can reach goes through the ring below.
     */
    bool stage_upload(device_state &dev, std::span<const std::byte> data, VkBuffer &out_source) noexcept;
    void release_staging(device_state &dev) noexcept;

    // -------------------------------------------------------------------------
    // Staging ring and transfers (vulkan_transfer.cpp)
    // -------------------------------------------------------------------------

    /** Creates the device's upload ring. `bytes` of 0 takes the default. */
    bool create_staging_ring(device_state &dev, std::uint64_t bytes) noexcept;
    void release_staging_ring(device_state &dev) noexcept;
    void release_transfer_context(device_state &dev) noexcept;

    /**
     * Frees every ring block the copy queue has now passed. Cheap, and called before every allocation, so the ring
     * drains without anyone having to remember to drain it.
     */
    void retire_staging_blocks(device_state &dev) noexcept;

    /**
     * Reserves `size` aligned bytes of the ring and returns the physical offset to write at, or nothing when the ring
     * is too full (or too small, which never clears). `out_end` receives the absolute high-water mark the caller must
     * fold into its batch.
     */
    std::optional<VkDeviceSize> allocate_staging(device_state &dev, VkDeviceSize size, std::uint64_t &out_end) noexcept;

    /** Records that everything below `end` is free once the copy queue reaches `point`. */
    void retain_staging(device_state &dev, std::uint64_t end, std::uint64_t point);

    /** The queue values a resource created now should record; see `creation_marks`. */
    creation_marks marks_now(const device_state &dev) noexcept;

    /**
     * The one-shot transfer: stage `data` into the ring, hand `record` a command buffer and the
     * ring range to copy out of, and submit it on the copy queue. Does not wait.
     *
     * `data` may be empty, for a submission that only needs to record a barrier. `created` is the
     * destination's creation marks, which decide whether the submission has to be ordered after
     * work that might still be reading it - see the file comment in vulkan_transfer.cpp.
     */
    std::expected<std::uint64_t, error> transfer_once(
        device_state &dev, resource_id device_id, std::span<const std::byte> data, const creation_marks &created,
        const std::function<void(VkCommandBuffer, VkBuffer, VkDeviceSize)> &record) noexcept;

    /**
     * Fills in `sharingMode` and the family list. Concurrent across every distinct family the
     * device uses, because a resource here may be written on the copy queue and read on the
     * graphics one with nothing tracking the ownership transfer an exclusive resource would need.
     * A single-family adapter gets exclusive sharing and pays nothing.
     */
    template <typename CreateInfo>
    void apply_sharing(const device_state &dev, CreateInfo &info) noexcept
    {
        if (dev.families.size() < 2)
        {
            info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
            return;
        }
        info.sharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = static_cast<std::uint32_t>(dev.families.size());
        info.pQueueFamilyIndices = dev.families.data();
    }

    // -------------------------------------------------------------------------
    // Per-resource release hooks used by destroy_device (each in its own file)
    // -------------------------------------------------------------------------

    void release_buffer_objects(device_state &dev, buffer_state &b) noexcept;
    void release_shader_objects(device_state &dev, shader_state &s) noexcept;
    void release_texture_objects(device_state &dev, texture_state &t) noexcept;
    void release_sampler_objects(device_state &dev, sampler_state &s) noexcept;
    void release_pipeline_objects(device_state &dev, pipeline_state &p) noexcept;
    void release_swapchain_objects(device_state &dev, swapchain_state &sc) noexcept;
    void release_command_list_objects(device_state &dev, command_list_state &cl) noexcept;
    void release_command_pool_objects(device_state &dev, command_pool_state &pool) noexcept;
    void release_download_objects(device_state &dev, download_state &d) noexcept;

    // -------------------------------------------------------------------------
    // Cross-file texture helpers (vulkan_texture.cpp)
    // -------------------------------------------------------------------------

    /**
     * Creates a texture owned by `device_id`, optionally uploading `initial_data` to mip 0 / layer 0, and registers it
     * with `swapchain_id` (0 for user textures). The image is left in VK_IMAGE_LAYOUT_GENERAL.
     */
    resource_id create_texture_internal(resource_id device_id, const texture_desc &desc,
                                        std::span<const std::byte> initial_data, resource_id swapchain_id);

    /** Registers a VkSwapchainKHR image as a texture (creates its view; the image itself is not owned). */
    resource_id register_presentable_image(resource_id device_id, resource_id swapchain_id, std::uint32_t index,
                                           VkImage image, VkFormat vk_format, const texture_desc &desc,
                                           const std::string &debug_name);

    /** Destroys a swapchain-owned texture record immediately (the device must be idle). */
    void destroy_swapchain_texture(device_state &dev, resource_id texture_id) noexcept;

    // -------------------------------------------------------------------------
    // Swapchain <-> submit handoff (vulkan_swapchain.cpp)
    // -------------------------------------------------------------------------

    /**
     * Appends, for every swapchain in `dev.pending_acquires`, the acquire semaphore a submission must wait on and the
     * render-finished semaphore it must signal. Call before `submit_batch`, then `complete_acquire_waits` with the
     * resulting point.
     */
    void collect_acquire_waits(device_state &dev, std::vector<VkSemaphore> &waits,
                               std::vector<VkPipelineStageFlags> &wait_stages, std::vector<VkSemaphore> &signals);
    void complete_acquire_waits(device_state &dev, const timeline_point &point) noexcept;

} // namespace catalyst::rendering::detail::vulkan
