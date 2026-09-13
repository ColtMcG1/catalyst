/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Queues: the engines a device submits work to, and `submit` itself.
 * @details A `queue` is a lightweight view - a device handle plus a @ref queue_kind - obtained from
 * @ref get_queue. It is a value, copying it is free, and it keeps nothing alive.
 *
 * **What this replaced.** `command_list_desc::queue` used to be a `queue_type` that restricted what
 * a list could record while every list ran on the same `VkQueue` regardless: a `transfer` list was
 * refused a `dispatch` and gained no parallelism in exchange. Now the three kinds are real queues
 * where the adapter has them, `queue_info::dedicated` says whether the one you have is real, and
 * `submit` returns a @ref timeline_point instead of a `bool`.
 *
 * **A list goes to the queue it was recorded for.** Not to any queue capable of running it: a
 * command buffer belongs to the family its pool came from, so `create_command_list(dev, {kind})`
 * decides where the list is submitted, and `submit` refuses anything else. On an adapter with fewer
 * engines than kinds the queues alias onto graphics beneath the API, so the same code is correct
 * without a fallback path.
 *
 * **Ordering, and how much of it you get.** Submissions to one queue execute in the order they were
 * submitted. Submissions to *different* queues have no order at all unless you give them one, which
 * is what @ref submit_info::wait is for: each point named there must complete before this
 * submission begins, and the wait happens on the GPU with the CPU uninvolved.
 *
 * @code
 *   // Cull on the compute engine, draw on the graphics engine, ordered by the GPU.
 *   const timeline_point culled = submit(get_queue(dev, queue_kind::compute), cull_list).value();
 *   const timeline_point drawn  = submit(get_queue(dev), {.lists = {&draw, 1}, .wait = {&culled, 1}}).value();
 * @endcode
 *
 * **Threads.** Recording and submitting still belong to one thread per device until Tier 3. What is
 * already safe from any thread is asking a queue what it has finished - @ref completed - and
 * waiting on a point.
 */

#pragma once

#include <catalyst/rendering/command.hpp>
#include <catalyst/rendering/device.hpp>
#include <catalyst/rendering/error.hpp>
#include <catalyst/rendering/timeline.hpp>
#include <catalyst/rendering/types.hpp>

#include <cstdint>
#include <expected>
#include <span>

namespace catalyst::rendering
{

    /**
     * @struct queue_info
     * @brief What a queue actually is on this adapter.
     */
    struct queue_info
    {
        /** @brief Which kind it was asked for. */
        queue_kind kind = queue_kind::graphics;

        /**
         * @brief True when this is a distinct hardware engine; false when it is the graphics queue
         * answering to another name.
         * @details The single most useful fact about a queue, and the one the old API had no way to
         * express. Work on an aliased queue is correct and runs in submission order with everything
         * else on graphics - it simply does not overlap with it, so a caller who was counting on a
         * copy proceeding alongside a frame should know it will not.
         *
         * Always true for @ref queue_kind::graphics.
         */
        bool dedicated = false;

        /**
         * @brief The backend's index for the underlying engine - a Vulkan queue family index, a
         * D3D12 command-list type. For debugging and for logs; do not branch on it.
         */
        std::uint32_t family_index = 0;
    };

    /**
     * @class queue
     * @brief A device engine that command lists are submitted to.
     * @details Two fields and no ownership. A default-constructed queue is invalid and every
     * operation on it fails with @ref error_code::invalid_argument rather than doing nothing
     * quietly.
     */
    class queue
    {
    public:
        constexpr queue() noexcept = default;
        constexpr queue(device owner, queue_kind kind) noexcept : device_(owner), kind_(kind) {}

        [[nodiscard]] constexpr device owner() const noexcept { return device_; }
        [[nodiscard]] constexpr queue_kind kind() const noexcept { return kind_; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return static_cast<bool>(device_); }

        [[nodiscard]] friend constexpr bool operator==(const queue &, const queue &) noexcept = default;

    private:
        device device_{};
        queue_kind kind_ = queue_kind::graphics;
    };

    /**
     * @brief The device's queue of the given kind. Every device has all three; ask
     * @ref get_queue_info whether this one is dedicated hardware.
     */
    [[nodiscard]] queue get_queue(const device &dev, queue_kind kind = queue_kind::graphics) noexcept;

    /** @brief What `q` is on this adapter. Defaults for an invalid queue. */
    [[nodiscard]] queue_info get_queue_info(const queue &q) noexcept;

    /**
     * @struct submit_info
     * @brief One submission: the lists to run, and what must finish first.
     */
    struct submit_info
    {
        /**
         * @brief Recorded lists to execute, in order. Must all be closed, belong to this queue's
         * device, and have been recorded for *this* queue's kind.
         * @details Same-kind, not "any queue that could run it". A command list owns a command
         * buffer allocated from a pool tied to one hardware queue family, and submitting it
         * anywhere else is undefined behaviour rather than a slow path - so the kind chosen at
         * `create_command_list` is where the list goes, full stop.
         *
         * That costs nothing on an adapter with fewer engines than kinds. The aliasing happens
         * underneath: `get_queue(dev, queue_kind::copy)` on an adapter with no DMA engine is the
         * graphics queue, and a `copy` list on that adapter was allocated from the graphics family
         * to begin with. Caller-side code is identical either way.
         *
         * A non-owning view read only during the call.
         */
        std::span<const command_list> lists;

        /**
         * @brief Points that must complete before this submission begins.
         * @details Waited on by the GPU, not the CPU: this call does not block on them. Points on
         * this same queue are redundant but harmless, since a queue already runs its submissions in
         * order. Invalid points are ignored, so a caller can pass a `timeline_point{}` for "no
         * dependency" without a branch.
         *
         * A non-owning view read only during the call.
         */
        std::span<const timeline_point> wait;
    };

    /**
     * @brief Queues `info.lists` for execution on `q`.
     * @return The point at which this submission completes.
     * @details Does not block. The returned point is what everything downstream is keyed on: reuse
     * of the lists, release of anything they touched, and the GPU-side dependency of whatever runs
     * next.
     *
     * Fails with @ref error_code::invalid_argument if a list is open, belongs to another device, or
     * was recorded for a different queue kind; nothing is submitted in that case.
     * @ref error_code::device_lost if the device is gone.
     *
     * Submitting no lists is legal and useful: it returns a point that completes after everything
     * already on this queue, optionally after `info.wait` too, which is how a caller inserts a
     * barrier between queues without any work to run.
     */
    [[nodiscard]] std::expected<timeline_point, error> submit(const queue &q, const submit_info &info);

    /** @brief Convenience for the common single-list submission with no dependencies. */
    [[nodiscard]] std::expected<timeline_point, error> submit(const queue &q, const command_list &cl);

    /**
     * @brief The point of the most recent successful submission to `q`, or an invalid point if
     * there has been none.
     */
    [[nodiscard]] timeline_point last_submitted(const queue &q) noexcept;

    /**
     * @brief The furthest point on `q` the GPU is known to have reached. Never blocks.
     * @details Everything at or before this has completed. Safe to call from any thread.
     */
    [[nodiscard]] timeline_point completed(const queue &q) noexcept;

    /**
     * @brief Blocks until everything submitted to `q` so far has completed.
     * @details Narrower than `wait_idle(device)`, which waits for all three engines. Equivalent to
     * `last_submitted(q).wait()`.
     */
    std::expected<void, error> wait_idle(const queue &q);

} // namespace catalyst::rendering
