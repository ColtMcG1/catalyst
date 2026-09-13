/**
 * @file queued.hpp
 * @brief A sink that holds events until their owner comes to collect them.
 * @details This is the sink a GUI panel is built on, and the only way an `owner_thread` sink reaches
 * a router. `async_sink` moves the writing to *a* thread; `queued_sink` moves it to *the* thread -
 * the one that owns whatever the sink touches, on the schedule that thread chooses.
 *
 *     auto panel = std::make_shared<queued_sink>(std::make_shared<my_gui_sink>());
 *     router.add_sink(panel);
 *
 *     // ... once a frame, on the thread that owns the widgets:
 *     panel->drain();
 *
 * On the logging thread `write` copies the event into a bounded ring and returns; nothing the owner
 * owns is touched, and nothing waits. `drain()` then delivers what accumulated, on the caller's
 * thread, and returns how many events that was. `drain(n)` delivers at most `n`, for an owner that
 * would rather spread a backlog over several frames than spend one of them catching up.
 *
 * The ceiling is real and so `overflow_policy` applies, minus `block`: a logging thread parked until
 * the owner next drains is a stall when the owner is busy and a deadlock when the owner is the
 * thread that logged. Dropped events are counted and reported into the inner sink on the next drain,
 * so a gap in the panel says so.
 *
 * Two things to know:
 *
 *   - A `queued_sink` has no `flush()`, deliberately. Flushing means delivering, delivering means
 *     running the inner sink, and the router flushes from the logging thread - which is the one
 *     thread this sink exists to keep away from the owner's state. So the router will not empty this
 *     queue for you, including on the way out at `fatal`. A panel is for reading the log while the
 *     process is alive; pair it with a console or file sink for the record of the process dying.
 *
 *   - Whatever has not been drained when the `queued_sink` is destroyed is discarded, because the
 *     destructor has no way to know it is running on the owner's thread. Drain first if it matters.
 *
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/logging/detail/event_ring.hpp>
#include <catalyst/logging/detail/sink_entry.hpp>
#include <catalyst/logging/event.hpp>
#include <catalyst/logging/sink.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <string_view>
#include <type_traits>
#include <utility>

namespace catalyst::logging
{

    /**
     * @struct queued_options
     * @brief How a `queued_sink` is sized and what it does when the owner falls behind.
     */
    struct queued_options
    {
        std::size_t capacity = 4096; ///< Events the queue holds; clamped up to 1.

        /// What happens when it is full. `block` is rejected; see the file comment.
        overflow_policy overflow = overflow_policy::drop_oldest;

        /// The category the "events were dropped" notice is logged under.
        std::string_view drop_category = "logging";
    };

    /**
     * @class queued_sink
     * @brief Another sink, written to by the thread that owns it rather than the one that logged.
     * @details Declares `reentrant`: `write` only takes this sink's own lock and copies into its own
     * ring, so any number of logging threads may call it at once. What it wraps is left entirely to
     * `drain`, and is therefore only ever touched by whoever calls that.
     */
    class queued_sink : public sink_contract<sink_threading::reentrant>
    {
    public:
        /**
         * @brief Wraps a sink held by value.
         * @tparam S The sink type; may declare any contract, `owner_thread` included.
         * @param sink The sink; moved into shared ownership and written to only from `drain`.
         * @param options Queue size and overflow behaviour.
         */
        template <log_sink S>
            requires(!std::same_as<std::decay_t<S>, queued_sink>)
        explicit queued_sink(S sink, queued_options options = {})
            : queued_sink(detail::make_entry(0, std::make_shared<S>(std::move(sink))), options)
        {
        }

        /// Wraps a sink the caller also holds - the usual way, since the owner already has a handle.
        template <log_sink S>
        explicit queued_sink(std::shared_ptr<S> sink, queued_options options = {})
            : queued_sink(detail::make_entry(0, std::move(sink)), options)
        {
        }

        /// Wraps a sink somebody else owns, and which must outlive this one.
        template <log_sink S>
        explicit queued_sink(std::reference_wrapper<S> sink, queued_options options = {})
            : queued_sink(detail::make_entry(0, &sink.get()), options)
        {
        }

        queued_sink(const queued_sink &) = delete;
        queued_sink &operator=(const queued_sink &) = delete;

        /// Discards anything not yet drained. See the file comment.
        ~queued_sink();

        /// Queues the event, or applies the overflow policy. Does not touch the inner sink.
        void write(const log_event &event);

        /**
         * @brief Delivers everything queued to the inner sink, on this thread.
         * @return How many events were delivered, not counting any drop notice.
         * @details Call this from the thread that owns whatever the inner sink touches, and only
         * from there. Events queued while it runs are left for the next call, so a drain always
         * finishes even when a logging thread is filling the queue behind it.
         */
        std::size_t drain();

        /**
         * @brief Delivers at most `max_events`, so a backlog can be spread over several frames.
         * @param max_events The most to deliver on this call.
         * @return How many events were delivered.
         */
        std::size_t drain(std::size_t max_events);

        /**
         * @brief Flushes the inner sink, if it has a `flush`.
         * @details Not named `flush`, and so not found by `flushable_sink`, on purpose: the router
         * flushes from the logging thread, and the inner sink belongs to this one. Call it after
         * `drain`, from the same thread.
         */
        void flush_inner();

        /// How many events are waiting to be drained.
        [[nodiscard]] std::size_t pending() const;

        /// How many events the queue holds.
        [[nodiscard]] std::size_t capacity() const;

        /// How many events have been dropped over this sink's life.
        [[nodiscard]] std::uint64_t dropped() const;

        /// How many events have reached the inner sink.
        [[nodiscard]] std::uint64_t delivered() const;

    private:
        /// The constructor the templates funnel into, once the inner sink has been type-erased.
        queued_sink(detail::sink_entry inner, queued_options options);

        detail::sink_entry inner_;
        std::string_view drop_category_;
        overflow_policy overflow_;

        mutable std::mutex mutex_;
        detail::event_ring queue_;
        std::uint64_t dropped_ = 0;
        std::uint64_t reported_drops_ = 0;
        std::uint64_t delivered_ = 0;

        /// Held for the length of a drain, so two owners draining at once do not overlap in the sink.
        std::mutex delivery_mutex_;
    };

} // namespace catalyst::logging
