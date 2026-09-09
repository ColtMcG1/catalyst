/**
 * @file async.hpp
 * @brief A sink that takes events off the logging thread.
 * @details Every stock sink writes on the thread that logged: `file_sink` takes a lock and pushes a
 * line at the filesystem while the caller waits. That is the wrong trade in a frame loop, where the
 * cost of one slow write lands on whatever was being drawn.
 *
 *     router.emplace_sink<async_sink>(file_sink{"app.log"});
 *
 * `async_sink` wraps any other sink, copies each event into a bounded queue, and lets one worker
 * thread do the writing. What it cannot do is grow without limit, so a producer faster than the
 * sink behind it eventually meets the queue's ceiling, and `overflow_policy` says what happens then.
 * Dropped events are counted and reported into the inner sink, so a gap in the log says so rather
 * than passing for silence.
 *
 * The inner sink is only ever touched by the worker thread, which means it does not have to be
 * thread-safe on its own account - though the stock ones are anyway, since nothing stops the same
 * sink also being registered directly. What it must not be is `owner_thread`: the worker is a thread
 * this sink started, not the one the sink asked for, and wrapping a GUI sink in an `async_sink` would
 * quietly do exactly the thing the contract was written to prevent. Those go in a `queued_sink`,
 * and the constructor here says so.
 *
 * Two ways this sink can still hold up a logging thread, both of them chosen rather than accidental:
 * `overflow_policy::block` waits for room, and `flush()` waits for the worker to catch up - which
 * the router does on its own account at the flush level. `async_options::flush_timeout` puts a
 * ceiling on the second.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/logging/detail/event_ring.hpp>
#include <catalyst/logging/detail/sink_entry.hpp>
#include <catalyst/logging/event.hpp>
#include <catalyst/logging/format.hpp>
#include <catalyst/logging/sink.hpp>

#include <chrono>
#include <concepts>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace catalyst::logging
{

    /**
     * @struct async_options
     * @brief How an `async_sink` is sized, what it does when it fills up, and how long it will wait.
     */
    struct async_options
    {
        std::size_t capacity = 8192;                             ///< Events the queue holds; clamped up to 1.
        overflow_policy overflow = overflow_policy::drop_newest; ///< What happens when it is full.

        /// The category the "events were dropped" notice is logged under.
        std::string_view drop_category = "logging";

        /**
         * How long `flush()` waits for the worker before giving up; zero waits indefinitely.
         * @details Zero is the default because a flush that returns early has, by definition, left
         * something unwritten, and the usual reason to flush is that it mattered. Set it when the
         * flushing thread is one that must not be stopped - a frame loop reaching the router's flush
         * level - and accept the tail it may leave behind.
         */
        std::chrono::milliseconds flush_timeout{0};
    };

    /**
     * @class async_sink
     * @brief Another sink, written to by a worker thread instead of the logging thread.
     * @details Declares `reentrant`: `write` takes only this sink's own lock and copies into its own
     * queue, so any number of logging threads may call it at once.
     */
    class async_sink : public sink_contract<sink_threading::reentrant>
    {
    public:
        /**
         * @brief Wraps a sink held by value.
         * @tparam S The sink type; must not be `owner_thread`.
         * @param sink The sink; moved into shared ownership and written to only by the worker.
         * @param options Queue size, overflow behaviour, flush timeout.
         */
        template <log_sink S>
            requires(!std::same_as<std::decay_t<S>, async_sink>)
        explicit async_sink(S sink, async_options options = {})
            : async_sink(detail::make_entry(0, std::make_shared<S>(std::move(sink))), options)
        {
            static_assert(sink_threading_of<S> != sink_threading::owner_thread,
                          "async_sink writes on a worker thread of its own, which is not the thread this sink "
                          "asked for. A sink that declared sink_threading::owner_thread goes in a queued_sink, "
                          "which its owner drains from the right thread.");
        }

        /// Wraps a sink the caller also holds.
        template <log_sink S>
        explicit async_sink(std::shared_ptr<S> sink, async_options options = {})
            : async_sink(detail::make_entry(0, std::move(sink)), options)
        {
            static_assert(sink_threading_of<S> != sink_threading::owner_thread,
                          "async_sink writes on a worker thread of its own, which is not the thread this sink "
                          "asked for. A sink that declared sink_threading::owner_thread goes in a queued_sink, "
                          "which its owner drains from the right thread.");
        }

        /// Wraps a sink somebody else owns, and which must outlive this one.
        template <log_sink S>
        explicit async_sink(std::reference_wrapper<S> sink, async_options options = {})
            : async_sink(detail::make_entry(0, &sink.get()), options)
        {
            static_assert(sink_threading_of<S> != sink_threading::owner_thread,
                          "async_sink writes on a worker thread of its own, which is not the thread this sink "
                          "asked for. A sink that declared sink_threading::owner_thread goes in a queued_sink, "
                          "which its owner drains from the right thread.");
        }

        async_sink(const async_sink &) = delete;
        async_sink &operator=(const async_sink &) = delete;

        /// Drains what is queued, then stops the worker. Nothing already accepted is lost.
        ~async_sink();

        /// Queues the event, or applies the overflow policy. Does not touch the inner sink.
        void write(const log_event &event);

        /// Waits for everything queued before this call to reach the inner sink, then flushes it.
        void flush();

        /**
         * @brief `flush()`, with a ceiling on the wait.
         * @param timeout How long to wait; zero waits indefinitely.
         * @return True when the flush completed, false when it timed out with events still queued.
         */
        bool flush_within(std::chrono::milliseconds timeout);

        /// How many events are waiting.
        [[nodiscard]] std::size_t pending() const;

        /// How many events the queue holds.
        [[nodiscard]] std::size_t capacity() const;

        /// How many events have been dropped over this sink's life.
        [[nodiscard]] std::uint64_t dropped() const;

    private:
        /// The constructor the templates funnel into, once the inner sink has been type-erased.
        async_sink(detail::sink_entry inner, async_options options);

        void run();

        detail::sink_entry inner_;
        std::string_view drop_category_;
        overflow_policy overflow_;
        std::chrono::milliseconds flush_timeout_;

        mutable std::mutex mutex_;
        std::condition_variable to_worker_;
        std::condition_variable to_caller_;

        detail::event_ring queue_;

        bool stopping_ = false;
        std::uint64_t enqueued_ = 0;
        std::uint64_t processed_ = 0;
        std::uint64_t dropped_ = 0;
        std::uint64_t reported_drops_ = 0;
        std::uint64_t flush_requests_ = 0;
        std::uint64_t flushes_done_ = 0;
        std::uint64_t flush_target_ = 0;

        std::thread worker_;
    };

} // namespace catalyst::logging
