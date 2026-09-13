/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief `frame_ring`: N frames in flight, the command pools that belong to each of them, and the
 * one call in a frame loop that is allowed to block.
 * @details Every renderer needs the same piece of bookkeeping and every renderer writes it again:
 * keep N sets of per-frame GPU state, hand out set `i % N` each frame, and before reusing set `i`
 * make sure the GPU has finished with the frame that used it N frames ago. Getting it wrong is
 * either a corrupted frame (reused too early) or a stalled one (waited too long), and the version
 * that came before this one had neither - it had *one* command list and a hidden wait inside
 * `begin_recording`, which is the same thing as `frames_in_flight = 1`.
 *
 * So this is that ring, made explicit:
 *
 *   - @ref frame_ring::begin **blocks**, and is the only call in the loop that does. It waits for
 *     the frame `frames_in_flight` frames back, then resets that frame's pools so every list from
 *     them may be recorded again. The name says "begin"; the documentation says "waits"; nothing is
 *     hidden anywhere else.
 *   - @ref frame::pool hands out a @ref command_pool per *worker* and per @ref queue_kind. Worker
 *     `i` takes pool `i` and records on its own thread, touching nothing another worker touches.
 *     That is the whole of the thread-safety argument for parallel recording, and it is structural:
 *     there is no shared mutable state on that path to protect.
 *   - @ref frame::end takes the @ref timeline_point the frame's work was submitted at. That point
 *     is what `begin` will wait on when this slot comes round again.
 *
 * **Why the pool is per (worker, kind) and not just per worker.** A pool belongs to a queue family,
 * which Tier 2 established is hardware and not convention - a list recorded from a graphics-family
 * pool cannot be submitted to the copy queue at all. So a worker that records both a draw list and
 * a copy list needs two pools, and the ring makes all three kinds available rather than making the
 * caller predict which it will want. Pools it is never asked for cost one driver object each.
 *
 * @code
 *   auto frames = frame_ring::create(dev, {.frames_in_flight = 3}).value();
 *
 *   while (running)
 *   {
 *       pump(dev);
 *
 *       auto f = frames.begin();                  // waits for frame N-3, resets its pools
 *       if (!f)
 *           break;                                // device lost
 *
 *       command_list cl = create_command_list(f->pool(), "frame");  // or keep one per slot
 *       begin_recording(cl);
 *       // ... record ...
 *       end_recording(cl);
 *
 *       const timeline_point done = submit(get_queue(dev), cl).value();
 *       f->end(done);
 *   }
 * @endcode
 *
 * **Threads.** The ring itself belongs to one thread: `begin` and `end` are called from the thread
 * that drives the frame loop. What may leave that thread is the @ref command_pool a `frame` hands
 * out - pass pool `i` to worker `i` and let it record there, then join before submitting. See
 * command.hpp for the rules a pool imposes on its thread.
 */

#pragma once

#include <catalyst/rendering/command.hpp>
#include <catalyst/rendering/device.hpp>
#include <catalyst/rendering/error.hpp>
#include <catalyst/rendering/timeline.hpp>
#include <catalyst/rendering/types.hpp>

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace catalyst::rendering
{

    class frame_ring;

    /**
     * @struct frame_ring_desc
     * @brief Creation parameters for a @ref frame_ring.
     */
    struct frame_ring_desc
    {
        /**
         * @brief How many frames of GPU work may be outstanding at once.
         * @details The number that decides how much latency is traded for how much overlap. Two
         * lets the CPU record frame N+1 while the GPU draws frame N; three adds slack for a frame
         * that runs long, at the cost of one more frame of input latency and one more set of
         * per-frame resources. One is legal and means "no overlap": `begin` waits for the previous
         * frame to finish entirely, which is what the module did before this type existed.
         *
         * Clamped to [1, @ref max_frames_in_flight].
         */
        std::uint32_t frames_in_flight = 2;

        /**
         * @brief How many threads will record into each frame.
         * @details One pool is created per worker per queue kind per frame, so this is the number
         * that multiplies. Leave it at 1 for a single-threaded renderer; set it to the size of the
         * job system's worker set for a parallel one. Clamped to [1, @ref max_frame_workers].
         */
        std::uint32_t workers = 1;

        /** @brief Prefix for the debug names given to the pools, so a capture tool can tell them
         * apart. Null for none. */
        const char *debug_name = nullptr;
    };

    /** @brief Upper bound on @ref frame_ring_desc::frames_in_flight. */
    inline constexpr std::uint32_t max_frames_in_flight = 8;

    /** @brief Upper bound on @ref frame_ring_desc::workers. */
    inline constexpr std::uint32_t max_frame_workers = 64;

    /**
     * @class frame
     * @brief One frame handed out by a @ref frame_ring: its pools, and where its completion point
     * is recorded.
     * @details A view, not an owner - it borrows the ring and must not outlive it. Copying one is
     * free and gives two references to the same frame, which is exactly what passing it to workers
     * wants.
     */
    class frame
    {
    public:
        constexpr frame() noexcept = default;

        /**
         * @brief The pool belonging to `worker` for `kind`.
         * @details Worker `w` must be the only thread that touches `pool(w, ...)`. Returns an
         * invalid handle when `worker` is past the ring's worker count, which is a caller bug
         * rather than a fallback: the recording that follows will do nothing.
         */
        [[nodiscard]] command_pool pool(std::uint32_t worker = 0,
                                        queue_kind kind = queue_kind::graphics) const noexcept;

        /** @brief Which of the ring's slots this frame occupies, in [0, frames_in_flight). */
        [[nodiscard]] constexpr std::uint32_t slot() const noexcept { return slot_; }

        /** @brief The frame's serial number since the ring was created, counting from 0. */
        [[nodiscard]] constexpr std::uint64_t index() const noexcept { return index_; }

        [[nodiscard]] constexpr bool valid() const noexcept { return ring_ != nullptr; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

        /**
         * @brief Records the point this frame's work completes at.
         * @details What @ref frame_ring::begin will wait on when this slot comes round again. Call
         * it once per frame with the last submission of the frame; calling it several times keeps
         * the latest point per queue, so a frame that submitted to graphics and to copy can report
         * both.
         *
         * Skipping it is not fatal but is not free either: the ring cannot know when the slot is
         * safe, so the next `begin` on this slot falls back to waiting for everything submitted to
         * the device - correct, and slower than it needed to be.
         *
         * The no-argument form ends a frame that submitted nothing - a skipped frame, or one whose
         * `submit` failed - and frees the slot immediately.
         */
        void end(const timeline_point &done = {}) noexcept;

        /** @brief `end` for a frame whose work finishes on several queues. */
        void end(std::span<const timeline_point> done) noexcept;

    private:
        friend class frame_ring;

        constexpr frame(frame_ring *ring, std::uint32_t slot, std::uint64_t index) noexcept
            : ring_(ring), slot_(slot), index_(index)
        {
        }

        frame_ring *ring_ = nullptr;
        std::uint32_t slot_ = 0;
        std::uint64_t index_ = 0;
    };

    /**
     * @class frame_ring
     * @brief N frames of command pools, cycled, with the wait for reuse made explicit.
     * @details Move-only and RAII: the destructor waits for anything still in flight and destroys
     * every pool it made. It owns pools and nothing else - no images, no descriptor heaps, no
     * per-frame uniform buffers - because those differ per renderer and the bookkeeping that does
     * not differ is the ring index and the wait.
     */
    class frame_ring
    {
    public:
        /**
         * @brief Builds a ring on `dev`.
         * @return @ref error_code::invalid_argument for an invalid device;
         * @ref error_code::out_of_host_memory if a pool could not be created.
         */
        [[nodiscard]] static std::expected<frame_ring, error> create(const device &dev,
                                                                    const frame_ring_desc &desc = {});

        frame_ring() noexcept = default;
        frame_ring(const frame_ring &) = delete;
        frame_ring &operator=(const frame_ring &) = delete;
        frame_ring(frame_ring &&other) noexcept;
        frame_ring &operator=(frame_ring &&other) noexcept;
        ~frame_ring();

        /**
         * @brief Waits for the frame `frames_in_flight` frames back, recycles its pools, and hands
         * it out again.
         * @return @ref error_code::device_lost if the device died while waiting;
         * @ref error_code::invalid_argument if the ring was never created or has been moved from.
         * @details **This blocks the calling thread**, and it is the only call in a frame loop
         * built on this type that does. How long for depends on how far ahead the CPU has got: on
         * a GPU-bound frame it is most of a frame, on a CPU-bound one it returns immediately.
         *
         * Calling `begin` twice without an intervening @ref frame::end is treated as the mistake it
         * usually is and handled conservatively: the open frame is closed against everything
         * currently submitted to the device, and a warning is logged.
         */
        [[nodiscard]] std::expected<frame, error> begin();

        /**
         * @brief Waits for every frame still outstanding, then recycles all of the ring's pools.
         * @details For a resize, a scene swap, or anything else that has to know the GPU is no
         * longer reading last frame's resources. The destructor does this too.
         */
        std::expected<void, error> wait_all();

        [[nodiscard]] device owner() const noexcept { return device_; }
        [[nodiscard]] std::uint32_t frames_in_flight() const noexcept { return frames_in_flight_; }
        [[nodiscard]] std::uint32_t workers() const noexcept { return workers_; }

        /** @brief How many frames have been handed out by @ref begin. */
        [[nodiscard]] std::uint64_t frame_index() const noexcept { return next_index_; }

        [[nodiscard]] bool valid() const noexcept { return frames_in_flight_ != 0; }
        [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

    private:
        friend class frame;

        /** Per slot: the points its work completes at, one per queue kind. */
        using slot_points = std::array<timeline_point, queue_kind_count>;

        [[nodiscard]] command_pool pool_at(std::uint32_t slot, std::uint32_t worker, queue_kind kind) const noexcept;
        void record_end(std::uint32_t slot, std::span<const timeline_point> done) noexcept;
        void destroy() noexcept;

        device device_{};
        std::uint32_t frames_in_flight_ = 0;
        std::uint32_t workers_ = 0;
        std::uint64_t next_index_ = 0;
        /** True between `begin` and the matching `end`; see `begin`. */
        bool open_ = false;
        std::uint32_t open_slot_ = 0;

        /** Slot-major, then worker, then queue kind. */
        std::vector<command_pool> pools_;
        std::vector<slot_points> points_;
    };

} // namespace catalyst::rendering
