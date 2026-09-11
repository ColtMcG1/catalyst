/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief `timeline_point`: a value naming a moment on a device queue's timeline, returned by
 * everything that costs GPU time.
 * @details This is the type the rest of the rewrite is built on, so it is worth saying plainly what
 * it is for. Before it, the rendering API had no way to talk about time. `submit` returned a
 * `bool` - the driver accepted the work, nothing more - and the only way to find out whether the
 * GPU had finished anything was `wait_idle`, which stalls everything on the device. So a caller who
 * wanted to reuse a command list, free a staging buffer, or read back a render target had exactly
 * one tool, and it was the biggest hammer in the box.
 *
 * A @ref timeline_point closes that gap. It names the moment a particular submission completes, and
 * it can be:
 *
 *   - **asked** - @ref timeline_point::is_complete, which never blocks;
 *   - **waited on by the CPU** - @ref timeline_point::wait, or @ref timeline_point::wait_for with a
 *     deadline;
 *   - **waited on by the GPU** - hand it to another `submit` as a dependency and the CPU is not
 *     involved at all, which is how work on the copy queue is ordered before work on the graphics
 *     queue without a round trip;
 *   - **awaited** - `co_await` it inside a `catalyst::events::task`, resuming in @ref pump on the
 *     thread that calls it.
 *
 * **Why one type covers three backends.** A Vulkan timeline semaphore, a D3D12 fence and a Metal
 * shared event are the same object: a 64-bit counter that only ever increases, which a submission
 * signals and a wait compares against. Binary semaphores, which are none of those things, appear
 * only in swapchain interop and stay inside the backend where they belong. So `timeline_point` is
 * not an abstraction over three different synchronisation models - it is the one model all three
 * already implement.
 *
 * **Ordering.** Points from the same device queue are totally ordered, and `a <= b` means "a
 * completes no later than b". Points from different queues are not ordered at all, because nothing
 * about the hardware says which of two engines reaches its next milestone first; comparing them
 * yields `std::partial_ordering::unordered` rather than a plausible-looking lie. Use
 * @ref same_timeline when a caller needs to know before comparing.
 *
 * **Threads.** @ref timeline_point::is_complete, @ref timeline_point::wait and
 * @ref timeline_point::wait_for may be called from any thread. Everything else on a device is still
 * single-threaded until Tier 3; see device.hpp.
 */

#pragma once

#include <catalyst/rendering/device.hpp>
#include <catalyst/rendering/error.hpp>
#include <catalyst/rendering/types.hpp>

#include <chrono>
#include <compare>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <expected>

namespace catalyst::rendering
{

    class timeline_point;

    namespace detail
    {
        /**
         * Registers `continuation` to be resumed by `pump` once `point` completes.
         * @return True when the continuation was parked and will be resumed later; false when it
         * could not be (the device is gone), in which case the caller resumes immediately.
         * Not part of the public API.
         */
        bool park(const timeline_point &point, std::coroutine_handle<> continuation);

        /**
         * Takes `continuation` back out of the park list, if it is still in it. Called when a
         * parked coroutine is destroyed rather than resumed, so that `pump` is not left holding a
         * handle to a freed frame. A no-op for a continuation that is not parked.
         * Not part of the public API.
         */
        void unpark(std::coroutine_handle<> continuation) noexcept;

        /**
         * How many continuations are parked, across every device. For tests: the only way to see
         * that a dropped task took its continuation with it is to count what is left behind.
         * Not part of the public API.
         */
        [[nodiscard]] std::size_t parked_count() noexcept;
    } // namespace detail

    /**
     * @class timeline_point
     * @brief A moment on one device queue's timeline.
     * @details A value, not a handle: copying one is free, it keeps nothing alive, and it stays
     * meaningful for as long as its device does. A default-constructed point is the "no work"
     * point - it is not valid, and it reports itself complete, so code that submits nothing does
     * not need a special case.
     *
     * Trivially copyable and 24 bytes, so returning one costs the same as returning the `bool` it
     * replaces.
     */
    class timeline_point
    {
    public:
        constexpr timeline_point() noexcept = default;

        /** @brief Builds a point directly. Callers get these from `submit`; this exists for
         * backends and tests. */
        constexpr timeline_point(device owner, queue_kind queue, std::uint64_t value) noexcept
            : device_(owner), value_(value), queue_(queue)
        {
        }

        /** @brief The device whose queue this point is on. */
        [[nodiscard]] constexpr device owner() const noexcept { return device_; }

        /** @brief Which queue's timeline it names. */
        [[nodiscard]] constexpr queue_kind queue() const noexcept { return queue_; }

        /**
         * @brief The raw counter value, for logging and for backends.
         * @details Zero means "no work". Values are per device queue and start at 1.
         */
        [[nodiscard]] constexpr std::uint64_t value() const noexcept { return value_; }

        /** @brief True when the point names actual submitted work. */
        [[nodiscard]] constexpr bool valid() const noexcept { return value_ != 0 && static_cast<bool>(device_); }

        [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

        /**
         * @brief Whether the GPU has reached this point yet. Never blocks.
         * @details True for an invalid point: nothing was submitted, so nothing is outstanding.
         * Also true once the device has been lost, because no further wait would ever be satisfied
         * and a caller polling in a loop would spin forever - check for
         * @ref error_code::device_lost through @ref wait if the difference matters.
         */
        [[nodiscard]] bool is_complete() const noexcept;

        /**
         * @brief Blocks the calling thread until the GPU reaches this point.
         * @return Nothing on success; @ref error_code::device_lost if the device died while
         * waiting. Returns immediately for an invalid point.
         * @details This is the honest, unbounded wait. Prefer @ref is_complete in a frame loop and
         * `co_await` in a task; reach for this when there is genuinely nothing else to do.
         */
        std::expected<void, error> wait() const noexcept;

        /**
         * @brief Blocks until the GPU reaches this point or `timeout` elapses.
         * @return Nothing on success; @ref error_code::timeout if the deadline passed first, which
         * is not a failure of the work - it may still complete, and waiting again is legitimate.
         */
        std::expected<void, error> wait_for(std::chrono::nanoseconds timeout) const noexcept;

        /**
         * @brief Suspends a coroutine until the GPU reaches this point.
         * @details The continuation is resumed by @ref pump, on the thread that calls it - never on
         * a driver thread - so a resumed coroutine may do anything an ordinary function may do.
         * `co_await` yields `std::expected<void, error>`: a device lost while the coroutine was
         * parked resumes it with @ref error_code::device_lost rather than letting it proceed as if
         * the work had finished.
         *
         * A parked task may be dropped: destroying it takes its continuation back out of the park
         * list, so a later @ref pump will not resume it. The one case that stays the caller's is
         * destroying a task while a @ref pump on another thread is already resuming it - the
         * resumption is under way by then and no bookkeeping here can call it back.
         */
        [[nodiscard]] auto operator co_await() const noexcept
        {
            struct awaiter
            {
                timeline_point point;

                /** The handle this awaiter put in the park list, while it is still in there. */
                std::coroutine_handle<> parked{};

                [[nodiscard]] bool await_ready() const noexcept { return point.is_complete(); }

                [[nodiscard]] bool await_suspend(std::coroutine_handle<> continuation)
                {
                    // False resumes immediately: the device is gone, so nothing will ever pump it.
                    if (!detail::park(point, continuation))
                        return false;

                    parked = continuation;
                    return true;
                }

                [[nodiscard]] std::expected<void, error> await_resume() noexcept
                {
                    // `pump` took the entry out of the list before resuming us, so there is nothing
                    // left to give back.
                    parked = {};

                    // `pump` resumes a continuation for one of two reasons: the point completed, or
                    // the device died and nothing will ever complete again. `is_complete` reports
                    // true in both cases, on purpose, so ask the device which one happened.
                    if (point.valid() && is_device_lost(point.owner()))
                        return std::unexpected(make_error(error_code::device_lost));
                    return {};
                }

                // The awaiter lives in the coroutine frame, so destroying a suspended coroutine
                // runs this. Dropping a parked task - an early return, an exception, a frame job
                // that went away - would otherwise leave `pump` holding a handle to a freed frame
                // and resuming it.
                ~awaiter()
                {
                    if (parked)
                        detail::unpark(parked);
                }
            };
            return awaiter{*this};
        }

        [[nodiscard]] friend constexpr bool operator==(const timeline_point &, const timeline_point &) noexcept =
            default;

        /**
         * @brief Orders two points on the same timeline; `unordered` for points on different ones.
         * @details Two engines running independently have no defined order between their
         * milestones, and returning one anyway would make `a < b` read as a guarantee the hardware
         * does not give. Test with @ref same_timeline first when the answer matters.
         */
        [[nodiscard]] friend constexpr std::partial_ordering operator<=>(const timeline_point &a,
                                                                         const timeline_point &b) noexcept
        {
            if (a.device_ != b.device_ || a.queue_ != b.queue_)
                return std::partial_ordering::unordered;
            return a.value_ <=> b.value_;
        }

    private:
        device device_{};
        std::uint64_t value_ = 0;
        queue_kind queue_ = queue_kind::graphics;
    };

    /** @brief True when both points name moments on the same device queue, so they can be ordered. */
    [[nodiscard]] constexpr bool same_timeline(const timeline_point &a, const timeline_point &b) noexcept
    {
        return a.owner() == b.owner() && a.queue() == b.queue();
    }

    /**
     * @brief The later of two points on the same timeline.
     * @details Returns the valid one when only one is valid, and `a` when they are on different
     * timelines - a caller mixing timelines here has a bug, and picking arbitrarily is less
     * misleading than inventing an order. Useful for "the frame is done when all of this is done"
     * bookkeeping within one queue.
     */
    [[nodiscard]] constexpr timeline_point latest(const timeline_point &a, const timeline_point &b) noexcept
    {
        if (!a.valid())
            return b;
        if (!b.valid() || !same_timeline(a, b))
            return a;
        return a.value() >= b.value() ? a : b;
    }

    /**
     * @brief Resumes the coroutines whose points have completed, and retires resources whose last
     * use is now in the past.
     * @details The counterpart of `audio::stream::pump`, and the same bargain: nothing is delivered
     * from a driver thread, so everything is delivered by a call the application makes on a thread
     * it chose. Continuations parked by `co_await` resume here, inside this call, on this thread.
     *
     * Call it once a frame. A program that never calls it still renders - `submit`, `wait` and
     * `is_complete` do not depend on it - but its coroutines never resume and resources destroyed
     * while in flight are not released until the device is.
     *
     * Cheap when there is nothing to do: one atomic read per queue.
     */
    void pump(const device &dev);

} // namespace catalyst::rendering
