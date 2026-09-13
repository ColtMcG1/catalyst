/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief The module lock: one readers-writer mutex shared by the public API layer and every
 * backend, which is what makes creating, destroying and recording from several threads defined
 * rather than merely untested.
 * @details Tier 3 wanted parallel recording, and parallel recording is only useful if the thread
 * doing it is not racing the thread loading the next level. What was in the way was not the
 * recording path - a `command_list` touches nothing but itself - but the *registry* underneath it:
 * every backend keeps its resource records in `unordered_map`s keyed by `resource_id`, and a
 * `create_buffer` that rehashes one of those maps while another thread is looking a command list up
 * in a different one is a data race in the standard's eyes and a crash in practice.
 *
 * So the lock guards the registry, and the split is:
 *
 *   - **@ref exclusive_guard** - anything that inserts into or erases from a registry map, plus
 *     anything that mutates state shared by the whole device: `create_*`, `destroy_*`, `submit`,
 *     `write_buffer`, the transfer ring, `collect_garbage`, swapchain acquire and present.
 *   - **@ref shared_guard** - everything else: resolving a handle, reading a description, and the
 *     entire recording path. Recording mutates the list's own record and nothing else, and a list
 *     belongs to one `command_pool`, which belongs to one thread. Two workers recording at once
 *     hold the shared lock simultaneously and never touch the same bytes.
 *
 * **What is deliberately outside the lock.** `queue_wait` and `wait_idle` block, sometimes for a
 * whole frame, and holding even a shared lock across a blocking wait would stall every thread
 * trying to create a resource for exactly as long as the GPU takes. Those two are declared in
 * detail_backend.hpp as locking internally: they take the lock, copy out the handles they need,
 * drop it, block on the graphics API (whose synchronisation primitives are thread-safe by
 * specification), and re-take it to record what they learned.
 *
 * **Why one lock and not one per device.** Because the registry is one table, not one per device:
 * `resource_id`s are allocated from a single monotonic counter and resolved through a single map
 * per resource kind. Splitting the lock per device would mean splitting the registry per device
 * first, which buys nothing until an application drives two adapters - and multi-adapter is
 * deferred until one device is correct under threads, which is this. The hot path (recording) takes
 * the lock in shared mode and therefore does not contend with itself.
 *
 * Not part of the public API.
 */

#pragma once

#include <mutex>
#include <shared_mutex>

namespace catalyst::rendering::detail
{

    /**
     * The module lock itself. An inline function-local static, so the one instance is shared by the
     * public API layer and by whichever backend static library was linked in - they are separate
     * targets and neither can define a symbol for the other.
     */
    inline std::shared_mutex &module_mutex() noexcept
    {
        static std::shared_mutex mutex;
        return mutex;
    }

    /** Held for the duration of a call that only reads the registry, or records into one list. */
    class shared_guard
    {
    public:
        shared_guard() noexcept : lock_(module_mutex()) {}

        shared_guard(const shared_guard &) = delete;
        shared_guard &operator=(const shared_guard &) = delete;

    private:
        std::shared_lock<std::shared_mutex> lock_;
    };

    /** Held for the duration of a call that creates, destroys, submits or otherwise mutates state
     * the whole device shares. */
    class exclusive_guard
    {
    public:
        exclusive_guard() noexcept : lock_(module_mutex()) {}

        exclusive_guard(const exclusive_guard &) = delete;
        exclusive_guard &operator=(const exclusive_guard &) = delete;

        /** Drops the lock early, for the one pattern that needs it: resolve, unlock, block. */
        void unlock() noexcept
        {
            if (lock_.owns_lock())
                lock_.unlock();
        }

        void lock() noexcept
        {
            if (!lock_.owns_lock())
                lock_.lock();
        }

    private:
        std::unique_lock<std::shared_mutex> lock_;
    };

} // namespace catalyst::rendering::detail
