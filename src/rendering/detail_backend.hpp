/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Internal contract every rendering backend implements. The public API in include/catalyst/rendering validates
 * handles and forwards to these functions with raw `resource_id`s; backends never see public handle types except
 * inside descriptor structs. Not part of the public API.
 *
 * @details **Locking is the caller's, with two exceptions.** From Tier 3 on, the public layer holds
 * the module lock (src/rendering/detail_sync.hpp) across every call below, in shared mode for reads
 * and recording and in exclusive mode for anything that creates, destroys, submits, or otherwise
 * mutates state the device shares. A backend therefore needs no synchronisation of its own and must
 * not take the module lock itself.
 *
 * The exceptions are @ref queue_wait and @ref wait_idle, which block for as long as the GPU takes.
 * Holding a lock across those would stall every other thread for a whole frame, so they are called
 * *unlocked* and are responsible for their own: take the lock, resolve what is needed, drop it,
 * block on the graphics API (whose synchronisation objects are thread-safe by specification), and
 * re-take it to record the result. Each is marked below.
 */

#pragma once

#include <catalyst/rendering/rendering.hpp>

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace catalyst::rendering::detail
{

    // -------------------------------------------------------------------------
    // Identity
    // -------------------------------------------------------------------------

    const char *backend_name();
    backend_kind backend_type() noexcept;

    // -------------------------------------------------------------------------
    // Device
    // -------------------------------------------------------------------------

    resource_id create_device(const device_desc &desc);
    void destroy_device(resource_id id) noexcept;
    bool is_device_valid(resource_id id) noexcept;
    bool is_device_lost(resource_id id) noexcept;
    device_info get_device_info(resource_id id) noexcept;

    /** Blocks until every queue is idle. **Called unlocked**; takes the module lock itself. */
    void wait_idle(resource_id id) noexcept;

    /** State of the device's staging ring; see transfer.hpp. */
    staging_info get_staging_info(resource_id id) noexcept;

    // -------------------------------------------------------------------------
    // Queues and timelines
    //
    // Every device exposes all three queue kinds; `get_queue_info().dedicated` says whether the one
    // asked for is distinct hardware. Each kind has its own monotonic timeline whose values start
    // at 1, which is the whole of the synchronisation model the public API exposes: a Vulkan
    // timeline semaphore, a D3D12 fence and a Metal shared event all implement it directly.
    // -------------------------------------------------------------------------

    queue_info get_queue_info(resource_id device, queue_kind kind) noexcept;

    /** Value most recently signalled on this queue's timeline; 0 when nothing has been submitted. */
    std::uint64_t queue_last_submitted(resource_id device, queue_kind kind) noexcept;

    /** Highest value the GPU is known to have reached on this queue. Must not block. */
    std::uint64_t queue_completed(resource_id device, queue_kind kind) noexcept;

    /**
     * Blocks until this queue's timeline reaches `value`, or `timeout` elapses. A negative or
     * maximum timeout means wait indefinitely. Returns `error_code::timeout` when the deadline
     * passes and `error_code::device_lost` when the device dies.
     *
     * **Called unlocked**, and must not hold the module lock while blocking: this is the wait a
     * frame loop pays every frame, and stalling resource creation on other threads for its
     * duration would defeat the point of Tier 3.
     */
    std::expected<void, error> queue_wait(resource_id device, queue_kind kind, std::uint64_t value,
                                          std::chrono::nanoseconds timeout) noexcept;

    /**
     * Releases everything whose last use is now behind every queue's completed value. Driven by
     * `pump`; backends also call it opportunistically from `submit`. Must not block.
     */
    void collect_garbage(resource_id device) noexcept;

    // -------------------------------------------------------------------------
    // Buffers
    // -------------------------------------------------------------------------

    resource_id create_buffer(resource_id device, const buffer_desc &desc, std::span<const std::byte> initial_data);
    void destroy_buffer(resource_id id) noexcept;
    bool is_buffer_valid(resource_id id) noexcept;
    buffer_desc get_buffer_desc(resource_id id) noexcept;
    bool write_buffer(resource_id id, std::size_t offset, std::span<const std::byte> data) noexcept;
    bool read_buffer(resource_id id, std::size_t offset, std::span<std::byte> out) noexcept;

    // -------------------------------------------------------------------------
    // Shaders
    // -------------------------------------------------------------------------

    resource_id create_shader(resource_id device, const shader_desc &desc);
    void destroy_shader(resource_id id) noexcept;
    bool is_shader_valid(resource_id id) noexcept;
    std::span<const std::byte> get_bytecode(resource_id id) noexcept;
    shader_stage get_shader_stage(resource_id id) noexcept;

    // -------------------------------------------------------------------------
    // Textures and samplers
    // -------------------------------------------------------------------------

    resource_id create_texture(resource_id device, const texture_desc &desc, std::span<const std::byte> initial_data);
    void destroy_texture(resource_id id) noexcept;
    bool is_texture_valid(resource_id id) noexcept;
    texture_desc get_texture_desc(resource_id id) noexcept;

    resource_id create_sampler(resource_id device, const sampler_desc &desc);
    void destroy_sampler(resource_id id) noexcept;
    bool is_sampler_valid(resource_id id) noexcept;

    // -------------------------------------------------------------------------
    // Pipelines
    // -------------------------------------------------------------------------

    resource_id create_graphics_pipeline(resource_id device, const graphics_pipeline_desc &desc);
    resource_id create_compute_pipeline(resource_id device, const compute_pipeline_desc &desc);
    void destroy_pipeline(resource_id id) noexcept;
    bool is_pipeline_valid(resource_id id) noexcept;
    pipeline_type get_pipeline_type(resource_id id) noexcept;

    // -------------------------------------------------------------------------
    // Swapchains
    // -------------------------------------------------------------------------

    resource_id create_swapchain(resource_id device, const swapchain_desc &desc);
    void destroy_swapchain(resource_id id) noexcept;
    bool is_swapchain_valid(resource_id id) noexcept;
    swapchain_desc get_swapchain_desc(resource_id id) noexcept;
    bool resize_swapchain(resource_id id, extent2d extent);
    resource_id acquire_next_image(resource_id id);
    bool present(resource_id id);

    // -------------------------------------------------------------------------
    // Command pools and command lists
    // -------------------------------------------------------------------------
    //
    // A pool is the unit of recycling and the unit of thread affinity: lists allocated from it
    // become recordable again on `reset_command_pool` and never before, and one thread owns the
    // whole of it. Backends may assume a pool and its lists are touched by one thread at a time.

    resource_id create_command_pool(resource_id device, const command_pool_desc &desc);
    void destroy_command_pool(resource_id id) noexcept;
    bool is_command_pool_valid(resource_id id) noexcept;
    command_pool_desc get_command_pool_desc(resource_id id) noexcept;

    /** The device that owns `id`, or 0. */
    resource_id get_command_pool_device(resource_id id) noexcept;

    /**
     * Recycles every list allocated from the pool. Must not block: return `error_code::not_ready`
     * when any of them is still in flight, having changed nothing.
     */
    std::expected<void, error> reset_command_pool(resource_id id);

    /** Allocates a list from `pool`. The list takes the pool's queue kind. */
    resource_id create_command_list_in_pool(resource_id pool, const char *debug_name);

    /** Creates a list with a private pool of its own, which `begin_recording` may recycle. */
    resource_id create_command_list(resource_id device, const command_list_desc &desc);
    void destroy_command_list(resource_id id) noexcept;
    bool is_command_list_valid(resource_id id) noexcept;
    command_list_desc get_command_list_desc(resource_id id) noexcept;

    /** The point the list was last submitted at; invalid when never submitted or since recycled. */
    timeline_point command_list_last_submission(resource_id id) noexcept;

    /** Must not block: false when the list is still in flight or already recorded since its pool
     * was reset. */
    bool begin_recording(resource_id id);
    bool end_recording(resource_id id);
    bool is_recording(resource_id id) noexcept;

    void begin_render_pass(resource_id id, const render_pass_desc &desc) noexcept;
    void end_render_pass(resource_id id) noexcept;
    void set_pipeline(resource_id id, resource_id pipeline) noexcept;
    void set_viewport(resource_id id, const viewport &vp) noexcept;
    void set_scissor(resource_id id, const scissor_rect &rect) noexcept;
    void set_vertex_buffer(resource_id id, std::uint32_t binding, resource_id buffer, std::size_t offset) noexcept;
    void set_index_buffer(resource_id id, resource_id buffer, index_type type, std::size_t offset) noexcept;
    void set_uniform_buffer(resource_id id, std::uint32_t slot, resource_id buffer, std::size_t offset,
                            std::size_t size) noexcept;
    void set_storage_buffer(resource_id id, std::uint32_t slot, resource_id buffer, std::size_t offset,
                            std::size_t size) noexcept;
    void set_texture(resource_id id, std::uint32_t slot, resource_id texture) noexcept;
    void set_sampler(resource_id id, std::uint32_t slot, resource_id sampler) noexcept;
    void push_constants(resource_id id, std::uint32_t offset, std::span<const std::byte> data) noexcept;
    void draw(resource_id id, std::uint32_t vertex_count, std::uint32_t instance_count, std::uint32_t first_vertex,
              std::uint32_t first_instance) noexcept;
    void draw_indexed(resource_id id, std::uint32_t index_count, std::uint32_t instance_count,
                      std::uint32_t first_index, std::int32_t vertex_offset, std::uint32_t first_instance) noexcept;
    void dispatch(resource_id id, std::uint32_t x, std::uint32_t y, std::uint32_t z) noexcept;
    void copy_buffer(resource_id id, resource_id src, std::size_t src_offset, resource_id dst, std::size_t dst_offset,
                     std::size_t size) noexcept;

    /**
     * Submits `lists` on `kind`, after every point in `waits` has completed on the GPU. Returns the
     * timeline value that will be signalled when the submission finishes. Lists are validated by
     * the public layer; a backend may assume they are closed and owned by this device.
     */
    std::expected<std::uint64_t, error> submit(resource_id device, queue_kind kind,
                                               std::span<const command_list> lists,
                                               std::span<const timeline_point> waits);

    // -------------------------------------------------------------------------
    // Transfers
    //
    // Uploads are staged into a ring and submitted to the copy queue without a CPU wait anywhere;
    // downloads land in a host-visible buffer the `readback` owns. Backends are responsible for
    // making a transfer's result visible to later submissions on every queue - the public API
    // promises that a `write_buffer` followed by a draw is correct without the caller ordering it.
    // -------------------------------------------------------------------------

    /** Opens a batch on `device`, or 0. */
    resource_id begin_transfer_batch(resource_id device);

    std::expected<void, error> transfer_upload_buffer(resource_id batch, resource_id dst, std::size_t offset,
                                                      std::span<const std::byte> data);
    std::expected<void, error> transfer_upload_texture(resource_id batch, resource_id dst,
                                                       std::span<const std::byte> data);

    /** Submits the batch on the copy queue; returns its timeline value, or 0 for an empty batch. */
    std::expected<std::uint64_t, error> submit_transfer_batch(resource_id batch);

    /** Drops an unsubmitted batch and returns its staging space. */
    void discard_transfer_batch(resource_id batch) noexcept;

    std::size_t transfer_batch_staged_bytes(resource_id batch) noexcept;
    std::size_t transfer_batch_size(resource_id batch) noexcept;

    /**
     * Records a copy out of `src` into host-visible memory owned by the returned readback, ordered
     * after `after` (or after everything submitted, when `after` is empty), and submits it. The
     * timeline value it completes at is written to `out_value`.
     */
    std::expected<resource_id, error> begin_download(resource_id device, resource_id src, std::size_t offset,
                                                     std::size_t size, std::span<const timeline_point> after,
                                                     std::uint64_t &out_value);

    /** The staged bytes, once the download's point has passed. */
    std::expected<std::span<const std::byte>, error> download_bytes(resource_id id);

    void destroy_download(resource_id id) noexcept;

} // namespace catalyst::rendering::detail
