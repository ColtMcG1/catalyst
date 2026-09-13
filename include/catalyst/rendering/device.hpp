/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief The rendering device: the root object that owns every other GPU resource (buffers, shaders, textures, pipelines,
 * swapchains, command lists) and represents one logical connection to a graphics adapter.
 *
 * @details **Threads.** From Tier 3 on, the module is safe to use from several threads at once, and
 * the rules are worth stating precisely because "thread-safe" on its own says nothing useful:
 *
 *   - **Creating and destroying resources** may be done from any thread, including while other
 *     threads are recording. The registry underneath is guarded; the calls serialise against each
 *     other, so a loader thread creating a thousand buffers is not free, but it is defined.
 *   - **Recording** belongs to whichever thread owns the `command_pool` a list came from - see
 *     command.hpp. N pools, N threads, no contention between them.
 *   - **`submit`** may be called from any thread and is serialised internally. Submissions to one
 *     queue execute in the order they were accepted; two threads submitting to the same queue at
 *     the same time get an unspecified order between them, which is why the usual shape is N
 *     workers recording and one thread submitting.
 *   - **Waiting and querying** - `timeline_point::is_complete`, `wait`, `wait_for`, `completed` -
 *     are safe from any thread and do not hold the module's lock while they block.
 *   - **`pump`** belongs to whichever thread the application picks, and must be the same one every
 *     time: it is where parked coroutines resume, and they have every right to expect a stable
 *     thread. Same bargain as `audio::stream::pump`.
 *   - **`destroy_device`** must not race anything else on that device. It tears down every handle
 *     created from it, so there is nothing left for another thread to be holding.
 */

#pragma once

#include <catalyst/rendering/types.hpp>

#include <cstdint>

namespace catalyst::rendering
{

    /**
     * @struct device_desc
     * @brief Creation parameters for a rendering device.
     */
    struct device_desc
    {
        /** Reported to the driver (e.g. `VkApplicationInfo::pApplicationName`). */
        const char *application_name = "Catalyst";
        /** Enable API validation / debug layers where the backend supports them. Costs performance; use in debug builds. */
        bool enable_validation = false;
        /** Prefer a discrete GPU over an integrated one when several adapters are present. */
        bool prefer_discrete_adapter = true;

        /**
         * Size of the staging ring every asynchronous transfer is carved out of, in bytes; 0 takes
         * the backend's default (16 MiB).
         * @details The budget for data in flight between host memory and the GPU. A transfer that
         * does not fit in what is currently free fails with `error_code::staging_exhausted` rather
         * than blocking, so this number is the knob that decides how far ahead a streaming system
         * may run before it has to wait for the copy queue to catch up.
         *
         * Sizing it: roughly the largest burst uploaded between two `pump` calls. Too small shows
         * up as `staging_exhausted` under load; too large is host memory the process never gets
         * back. A single transfer larger than the whole ring can never succeed and has to be split
         * - `get_staging_info` reports the ceiling. See transfer.hpp.
         */
        std::uint64_t staging_ring_bytes = 0;
    };

    /**
     * @struct device_info
     * @brief Read-only facts about a created device.
     */
    struct device_info
    {
        backend_kind backend = backend_kind::null;
        /** Human-readable adapter name; owned by the backend and valid until the device is destroyed. */
        const char *adapter_name = "";
        std::uint64_t dedicated_video_memory_bytes = 0;
        /**
         * Backend limit on simultaneous memory allocations, or 0 when the backend does not report one. The Vulkan
         * backend allocates once per buffer and per texture, so this is an upper bound on live resources.
         */
        std::uint32_t max_memory_allocation_count = 0;
        /** True when device-local memory is host-visible (integrated adapters), so uploads skip the staging copy. */
        bool unified_memory = false;
    };

    struct device_tag
    {
    };

    /**
     * @brief Handle to a rendering device. See `create_device`.
     */
    using device = resource_handle<device_tag>;

    /**
     * @brief Creates a device on the compiled-in backend. Returns an invalid handle if no suitable adapter is found.
     */
    [[nodiscard]] device create_device(const device_desc &desc = {});

    /**
     * @brief Destroys the device and every resource created from it, then resets `d` to an invalid handle.
     * Waits for the GPU to go idle first.
     */
    void destroy_device(device &d) noexcept;

    [[nodiscard]] bool is_valid(const device &d) noexcept;

    [[nodiscard]] device_info get_device_info(const device &d) noexcept;

    /**
     * @brief Whether the device has been lost - removed, reset, or discarded by the driver after a
     * hang. Never blocks.
     * @details Once true it stays true: a lost device is not recoverable in place. Every handle
     * created from it is dead, waits on its timelines return @ref error_code::device_lost
     * immediately rather than hanging, and the correct response is to destroy it and rebuild.
     *
     * A caller does not have to poll this. `submit` reports @ref error_code::device_lost when it
     * happens, and `pump` publishes @ref device_lost_event once. The query exists so that code
     * holding a handle with no call in flight can ask.
     */
    [[nodiscard]] bool is_device_lost(const device &d) noexcept;

    /**
     * @brief Blocks until all submitted work on every queue of the device has completed.
     * @details The biggest hammer available, and after Tier 2 rarely the right one: waiting on a
     * `timeline_point` waits for exactly the work a caller cares about, and `wait_idle(queue)`
     * waits for one engine. This remains correct for teardown, where everything really does have
     * to stop.
     */
    void wait_idle(const device &d) noexcept;

} // namespace catalyst::rendering
