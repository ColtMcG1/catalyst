/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Command lists: recorded sequences of GPU work (render passes, state binds, draws, dispatches, copies) that are
 * handed to the device with `submit`.
 * @details Recording is bracketed by `begin_recording` / `end_recording`. Draw calls must sit inside a
 * `begin_render_pass` / `end_render_pass` pair; dispatches and copies must sit outside one. Calls made in an invalid
 * state are ignored by release builds and reported by backends with validation enabled. A command list may be
 * re-submitted until it is re-recorded; re-recording resets it.
 *
 * **Nothing here blocks.** `begin_recording` used to wait for the list's previous submission, because a list cannot be
 * reset while the GPU is still reading it. It no longer does: it *fails* while the list is in flight, and the wait
 * moved to `frame_ring::begin` in frame.hpp, which says out loud that it waits and waits once per frame rather than
 * once per list. A caller who wants the old behaviour writes `last_submission(cl).wait()` and can see what it costs.
 *
 * **Pools own recycling, and pools own threads.** A `command_pool` is created for one @ref queue_kind and is meant to
 * be touched by one thread. Every list allocated from it becomes recordable again when `reset_command_pool` is called,
 * and not before - which is the Vulkan / D3D12 model rather than a Catalyst invention, and the reason N threads can
 * record at once without a lock between them: worker `i` holds pool `i`, and no two pools share a byte.
 * `frame_ring` in frame.hpp is the pool bookkeeping most callers actually want; this header is what it is built from.
 *
 * @code
 *   // One thread, one pool, three lists rotated by hand. (frame_ring does this for you.)
 *   command_pool pool = create_command_pool(dev, {.queue = queue_kind::graphics});
 *   command_list cl   = create_command_list(pool, "frame");
 *
 *   // ... each frame:
 *   last_submission(cl).wait();        // where the GPU has got to with what this pool handed out
 *   reset_command_pool(pool).value();  // now every list from it may be re-recorded
 *   begin_recording(cl);
 * @endcode
 *
 * **Threads.** One thread per `command_pool`, for the pool and for every list allocated from it: create, record,
 * close, reset. Different pools may be driven by different threads at the same time, including pools belonging to the
 * same device and the same queue. `submit` is not thread-affine but is serialised internally, so the usual shape -
 * N workers record, one thread submits what they produced - needs no locking of its own.
 *
 * **Binding model.** Shader resources are bound to numbered slots of four independent kinds - uniform buffers, storage
 * buffers, textures and samplers - and a block of up to `max_push_constant_bytes` inline constants. Textures and
 * samplers are separate objects (as in D3D12 and Metal); combine them in the shader. How a slot appears in shader code:
 *
 * | Call                  | Vulkan (SPIR-V)                | D3D12 (DXIL)      | Metal                 |
 * |-----------------------|--------------------------------|-------------------|-----------------------|
 * | `set_uniform_buffer`  | `layout(set = 0, binding = n)` | `register(b<n>)`  | `[[buffer(n)]]`       |
 * | `set_storage_buffer`  | `layout(set = 1, binding = n)` | `register(u<n>)`  | `[[buffer(16 + n)]]`  |
 * | `set_texture`         | `layout(set = 2, binding = n)` | `register(t<n>)`  | `[[texture(n)]]`      |
 * | `set_sampler`         | `layout(set = 3, binding = n)` | `register(s<n>)`  | `[[sampler(n)]]`      |
 * | `push_constants`      | `layout(push_constant)`        | root constants    | `[[buffer(30)]]`      |
 *
 * Bindings are visible to every shader stage and persist across `set_pipeline` calls within a recording. Slot indices
 * are bounded by `max_uniform_buffer_slots`, `max_storage_buffer_slots`, `max_texture_slots` and `max_sampler_slots`.
 *
 * **Coordinate conventions.** Clip space is +Y up with depth in [0, 1] on every backend (the Vulkan backend flips its
 * viewport to match); `viewport` and `scissor_rect` are in framebuffer pixels with the origin at the top-left.
 */

#pragma once

#include <catalyst/rendering/buffer.hpp>
#include <catalyst/rendering/device.hpp>
#include <catalyst/rendering/pipeline.hpp>
#include <catalyst/rendering/texture.hpp>
#include <catalyst/rendering/types.hpp>

#include <catalyst/rendering/error.hpp>
#include <catalyst/rendering/timeline.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace catalyst::rendering
{

    // -----------------------------------------------------------------------------
    // Command pools
    // -----------------------------------------------------------------------------

    /**
     * @struct command_pool_desc
     * @brief Creation parameters for a command pool.
     */
    struct command_pool_desc
    {
        /**
         * @brief The queue kind every list from this pool is recorded for and submitted to.
         * @details Fixed here rather than per list because that is where the hardware fixes it: a
         * pool is created against one queue family, and the command buffers it hands out belong to
         * that family for life. Adapters with fewer engines than kinds alias the families
         * underneath, so a `copy` pool on an adapter with no DMA engine is a graphics-family pool
         * and no caller-side branch is needed.
         */
        queue_kind queue = queue_kind::graphics;
        const char *debug_name = nullptr;
    };

    struct command_pool_tag
    {
    };

    /**
     * @brief Handle to a command pool: the thread-affine allocator that command lists come from.
     * @details See `create_command_pool`.
     */
    using command_pool = resource_handle<command_pool_tag>;

    /** @brief Creates a pool on `dev`. Returns an invalid handle if the device is invalid. */
    [[nodiscard]] command_pool create_command_pool(const device &dev, const command_pool_desc &desc = {});

    /**
     * @brief Destroys the pool and every list allocated from it, then resets `pool`.
     * @details Waits for anything from this pool that is still in flight, because a command buffer
     * cannot be freed while the GPU is reading it. Reaching that wait means the caller destroyed a
     * pool mid-frame; the frame-loop path is `reset_command_pool`, which never waits.
     */
    void destroy_command_pool(command_pool &pool) noexcept;

    [[nodiscard]] bool is_valid(const command_pool &pool) noexcept;

    /** @brief The description the pool was created with. Defaults for an invalid handle. */
    [[nodiscard]] command_pool_desc get_command_pool_desc(const command_pool &pool) noexcept;

    /** @brief The device that owns the pool. An invalid handle for an invalid pool. */
    [[nodiscard]] device get_device(const command_pool &pool) noexcept;

    /**
     * @brief Recycles the pool: every list allocated from it becomes recordable again, and whatever
     * they recorded is discarded.
     * @return @ref error_code::not_ready when any list from the pool is still in flight - nothing
     * is reset in that case, and the pool is exactly as it was.
     * @details Does not block, and that is the whole point of it. Resetting a pool is how a command
     * buffer is reused on every graphics API worth the name, and the precondition is always the
     * same: the GPU must have finished with it. Satisfy it by waiting on the @ref timeline_point
     * that the pool's lists were submitted at - which is what `frame_ring::begin` does, once per
     * frame, for every pool of the frame it is about to hand out.
     *
     * Called on the pool's own thread, like everything else about a pool.
     */
    std::expected<void, error> reset_command_pool(const command_pool &pool);

    /**
     * @struct command_list_desc
     * @brief Creation parameters for a command list.
     */
    struct command_list_desc
    {
        /**
         * @brief Which kind of queue this list will be submitted to.
         * @note Ignored when the list is allocated from a @ref command_pool, which already fixed
         * the kind - see the `create_command_list` overload taking one.
         * @details Two things at once. It determines what the list may record -
         * `queue_kind::compute` refuses draws, `queue_kind::copy` refuses draws and dispatches - and
         * it fixes which queue the list is submitted to: `submit` accepts it on a queue of this kind
         * and no other, because the underlying command buffer belongs to one hardware queue family.
         *
         * Adapters with fewer engines than kinds need no fallback in caller code: the queues alias
         * onto graphics inside the device, so a `copy` list on such an adapter is allocated from the
         * graphics family and submitted to a `copy` queue that is the graphics queue.
         *
         * This used to be a `queue_type` that bought the recording restriction without any of the
         * parallelism: every kind ran on the same queue. See queue.hpp.
         */
        queue_kind queue = queue_kind::graphics;
        const char *debug_name = nullptr;
    };

    struct command_list_tag
    {
    };

    /**
     * @brief Handle to a command list. See `create_command_list`.
     */
    using command_list = resource_handle<command_list_tag>;

    // -----------------------------------------------------------------------------
    // Render passes
    // -----------------------------------------------------------------------------

    enum class load_op : std::uint8_t
    {
        load,      ///< Keep the attachment's previous contents.
        clear,     ///< Clear to the attachment's clear value.
        dont_care, ///< Contents are undefined; cheapest when everything will be overwritten.
    };

    enum class store_op : std::uint8_t
    {
        store,     ///< Keep the results after the pass.
        dont_care, ///< Results may be discarded (e.g. a depth buffer nobody reads back).
    };

    struct clear_color
    {
        float r = 0.0f;
        float g = 0.0f;
        float b = 0.0f;
        float a = 1.0f;
    };

    struct color_attachment
    {
        texture target;
        load_op load = load_op::clear;
        store_op store = store_op::store;
        clear_color clear{};
    };

    struct depth_stencil_attachment
    {
        /** Leave invalid for passes without depth. */
        texture target;
        load_op load = load_op::clear;
        store_op store = store_op::dont_care;
        float clear_depth = 1.0f;
        std::uint8_t clear_stencil = 0;
    };

    /**
     * @struct render_pass_desc
     * @brief Attachments of a render pass. `color_attachments` is a non-owning view read only during
     * `begin_render_pass`.
     */
    struct render_pass_desc
    {
        std::span<const color_attachment> color_attachments;
        depth_stencil_attachment depth_stencil;
        const char *debug_name = nullptr;
    };

    // -----------------------------------------------------------------------------
    // Lifetime and recording state
    // -----------------------------------------------------------------------------

    /**
     * @brief Allocates a list from `pool`. The list is recorded for, and submitted to, the pool's
     * queue kind.
     * @details The list lives until the pool is destroyed or `destroy_command_list` is called on
     * it, and becomes recordable again each time the pool is reset. Allocating a handful once and
     * re-recording them every frame is the intended shape; allocating per frame works and costs a
     * driver allocation.
     */
    [[nodiscard]] command_list create_command_list(const command_pool &pool, const char *debug_name = nullptr);

    /**
     * @brief Creates a list with a pool of its own, sized for exactly one list.
     * @details The convenience form, for a list that is not part of a frame ring: one-off setup
     * work, a test, a benchmark. Because the pool is private to the list, `begin_recording` may
     * recycle it without a separate `reset_command_pool` - but it still refuses to do so while the
     * list is in flight, so a caller re-recording every frame needs several of these and needs to
     * check @ref last_submission, which is the argument for using a `command_pool` instead.
     */
    [[nodiscard]] command_list create_command_list(const device &dev, const command_list_desc &desc = {});

    /**
     * @brief Releases the command list and resets `cl` to an invalid handle.
     * @details Waits for a pending submission of it, for the same reason `destroy_command_pool`
     * does: a command buffer the GPU is reading cannot be freed.
     */
    void destroy_command_list(command_list &cl) noexcept;

    [[nodiscard]] bool is_valid(const command_list &cl) noexcept;

    /**
     * @brief The description the list was created with. Defaults for an invalid handle.
     * @details Mostly there so a caller holding a list can ask which queue it needs without having
     * tracked that alongside it - `submit(get_queue(dev, get_command_list_desc(cl).queue), cl)`.
     */
    [[nodiscard]] command_list_desc get_command_list_desc(const command_list &cl) noexcept;

    /**
     * @brief The point the list was last submitted at, or an invalid point if it has never been
     * submitted or its pool has been reset since.
     * @details The answer to "may I record into this again": `last_submission(cl).is_complete()`.
     * Also the point to hand `submit_info::wait` when a later submission must not overtake work
     * this list is still doing.
     */
    [[nodiscard]] timeline_point last_submission(const command_list &cl) noexcept;

    /**
     * @brief Starts recording, discarding any previously recorded commands.
     * @return False if the list is invalid, already recording, still in flight, or was recorded
     * since its pool was last reset.
     * @details **Does not block**, which is the Tier 3 change. It used to wait for
     * @ref last_submission before resetting the list, because a command buffer cannot be reset
     * while the GPU is reading it - a hidden stall that serialised a single-list frame loop against
     * the GPU completely. The precondition did not go away; it became the caller's, and the caller
     * that discharges it properly is `frame_ring`, which waits once per frame for a whole frame's
     * worth of pools.
     *
     * Two ways to fail that are worth telling apart:
     *   - the list is still in flight - `last_submission(cl).is_complete()` is false. Rotate more
     *     lists, or wait on the point;
     *   - the list was already recorded since its pool was last reset. A pool's lists are recorded
     *     once per reset cycle; call `reset_command_pool` (after the wait) to open the next one.
     *     Lists made by the `device` overload own their pool and recycle it here, so they never hit
     *     this case.
     */
    bool begin_recording(const command_list &cl);

    /**
     * @brief Finishes recording, making the list submittable.
     * @return False if the list is not recording or a render pass is still open.
     */
    bool end_recording(const command_list &cl);

    [[nodiscard]] bool is_recording(const command_list &cl) noexcept;

    // -----------------------------------------------------------------------------
    // Commands
    // -----------------------------------------------------------------------------

    void begin_render_pass(const command_list &cl, const render_pass_desc &desc) noexcept;
    void end_render_pass(const command_list &cl) noexcept;

    void set_pipeline(const command_list &cl, const pipeline &p) noexcept;
    void set_viewport(const command_list &cl, const viewport &vp) noexcept;
    void set_scissor(const command_list &cl, const scissor_rect &rect) noexcept;

    void set_vertex_buffer(const command_list &cl, std::uint32_t binding, const buffer &b,
                           std::size_t offset_bytes = 0) noexcept;
    void set_index_buffer(const command_list &cl, const buffer &b, index_type type,
                          std::size_t offset_bytes = 0) noexcept;

    /** @brief Binds a uniform (constant) buffer range to `slot`; `size_bytes == 0` means "to the end of the buffer". */
    void set_uniform_buffer(const command_list &cl, std::uint32_t slot, const buffer &b, std::size_t offset_bytes = 0,
                            std::size_t size_bytes = 0) noexcept;
    /** @brief Binds a storage (structured / UAV) buffer range to `slot`; `size_bytes == 0` means "to the end". */
    void set_storage_buffer(const command_list &cl, std::uint32_t slot, const buffer &b, std::size_t offset_bytes = 0,
                            std::size_t size_bytes = 0) noexcept;
    void set_texture(const command_list &cl, std::uint32_t slot, const texture &t) noexcept;
    void set_sampler(const command_list &cl, std::uint32_t slot, const sampler &s) noexcept;

    /** @brief Uploads up to `max_push_constant_bytes` of inline shader constants. */
    void push_constants(const command_list &cl, std::uint32_t offset_bytes, std::span<const std::byte> data) noexcept;

    void draw(const command_list &cl, std::uint32_t vertex_count, std::uint32_t instance_count = 1,
              std::uint32_t first_vertex = 0, std::uint32_t first_instance = 0) noexcept;
    void draw_indexed(const command_list &cl, std::uint32_t index_count, std::uint32_t instance_count = 1,
                      std::uint32_t first_index = 0, std::int32_t vertex_offset = 0,
                      std::uint32_t first_instance = 0) noexcept;

    void dispatch(const command_list &cl, std::uint32_t group_count_x, std::uint32_t group_count_y = 1,
                  std::uint32_t group_count_z = 1) noexcept;

    void copy_buffer(const command_list &cl, const buffer &src, std::size_t src_offset_bytes, const buffer &dst,
                     std::size_t dst_offset_bytes, std::size_t size_bytes) noexcept;

    // -----------------------------------------------------------------------------
    // Submission
    // -----------------------------------------------------------------------------
    //
    // `submit` lives in queue.hpp, because what a submission needs to say is which engine it runs
    // on, what must finish first, and - in its return value - when it will be done. See that header.

} // namespace catalyst::rendering
