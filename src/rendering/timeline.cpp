/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Public timeline API: querying and waiting on a `timeline_point`, parking coroutines on
 * one, and `pump`, which resumes them.
 * @details The park list is deliberately here rather than in a backend. What a backend knows is how
 * to compare a timeline value against the GPU's progress; who is waiting for that value, and on
 * which thread they should wake up, is the same answer on every backend and belongs on this side of
 * the seam.
 *
 * The list is global rather than per device because a `timeline_point` carries its device with it,
 * so a single vector keyed by nothing at all is enough, and a device with no parked continuations
 * costs one empty-vector check in `pump`. It is guarded by a mutex: `co_await` may be reached from
 * whatever thread a task happens to be running on, even while the device itself is still
 * single-threaded, and that is exactly the case a lock is cheap insurance for.
 */

#include <catalyst/rendering/timeline.hpp>

#include "detail_backend.hpp"
#include "detail_log.hpp"
#include "detail_sync.hpp"

#include <algorithm>
#include <chrono>
#include <mutex>
#include <utility>
#include <vector>

namespace catalyst::rendering
{

    namespace
    {
        struct parked_continuation
        {
            timeline_point point;
            std::coroutine_handle<> continuation;
        };

        struct park_list
        {
            std::mutex mutex;
            std::vector<parked_continuation> entries;
        };

        park_list &parked() noexcept
        {
            static park_list list;
            return list;
        }

        /** A wait of this length or longer means "no deadline"; see `queue_wait`. */
        constexpr std::chrono::nanoseconds forever = std::chrono::nanoseconds::max();
    } // namespace

    bool timeline_point::is_complete() const noexcept
    {
        if (!valid())
            return true;
        const detail::shared_guard guard;
        // A lost device never signals again. Reporting "complete" keeps a polling caller from
        // spinning forever; `is_device_lost` and `wait` are how the difference is told.
        if (detail::is_device_lost(device_.id()))
            return true;
        return detail::queue_completed(device_.id(), queue_) >= value_;
    }

    std::expected<void, error> timeline_point::wait() const noexcept
    {
        return wait_for(forever);
    }

    std::expected<void, error> timeline_point::wait_for(std::chrono::nanoseconds timeout) const noexcept
    {
        if (!valid())
            return {};
        // Deliberately unguarded: this is the wait a frame loop pays every frame, and the backend
        // takes the module lock only around the bookkeeping either side of the block. See
        // detail_backend.hpp.
        return detail::queue_wait(device_.id(), queue_, value_, timeout);
    }

    namespace detail
    {
        bool park(const timeline_point &point, std::coroutine_handle<> continuation)
        {
            if (!continuation)
                return false;
            // Nothing will ever pump a device that does not exist, so refuse rather than stranding
            // the coroutine; the awaiter resumes it inline instead.
            {
                const shared_guard guard;
                if (!point.valid() || !is_device_valid(point.owner().id()))
                    return false;
            }

            park_list &list = parked();
            const std::scoped_lock lock{list.mutex};
            list.entries.push_back({point, continuation});
            return true;
        }

        void unpark(std::coroutine_handle<> continuation) noexcept
        {
            if (!continuation)
                return;

            park_list &list = parked();
            const std::scoped_lock lock{list.mutex};

            const auto pos = std::find_if(list.entries.begin(), list.entries.end(),
                                          [continuation](const parked_continuation &entry) {
                                              return entry.continuation == continuation;
                                          });
            if (pos != list.entries.end())
                list.entries.erase(pos);
        }

        std::size_t parked_count() noexcept
        {
            park_list &list = parked();
            const std::scoped_lock lock{list.mutex};
            return list.entries.size();
        }
    } // namespace detail

    void pump(const device &dev)
    {
        if (!dev)
            return;

        // Retire whatever the GPU has finished with. Backends key deferred destruction on timeline
        // values, so this is what actually frees a resource destroyed while it was still in flight.
        {
            const detail::exclusive_guard guard;
            detail::collect_garbage(dev.id());
        }

        // Snapshot the ready continuations under the lock, resume them outside it: a resumed
        // coroutine may `co_await` another point and re-enter `park`, and doing that while holding
        // the mutex would deadlock.
        std::vector<std::coroutine_handle<>> ready;
        {
            park_list &list = parked();
            const std::scoped_lock lock{list.mutex};

            // Stays parked: someone else's device, or ours and not finished. A lost device needs no
            // special case here - `is_complete` reports true for every point on one, which is what
            // releases the coroutines waiting on work that will now never happen. They find out
            // through `await_resume`, which asks the device rather than the point.
            const auto first_ready = std::partition(list.entries.begin(), list.entries.end(),
                                                    [&dev](const parked_continuation &entry) {
                                                        return entry.point.owner() != dev ||
                                                               !entry.point.is_complete();
                                                    });

            ready.reserve(static_cast<std::size_t>(std::distance(first_ready, list.entries.end())));
            for (auto it = first_ready; it != list.entries.end(); ++it)
                ready.push_back(it->continuation);
            list.entries.erase(first_ready, list.entries.end());
        }

        for (const std::coroutine_handle<> continuation : ready)
        {
            if (continuation && !continuation.done())
                continuation.resume();
        }
    }

} // namespace catalyst::rendering
