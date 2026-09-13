/**
 * @file event_ring.hpp
 * @brief The bounded queue of events shared by the two hand-off sinks.
 * @details `async_sink` and `queued_sink` differ only in who does the writing - a worker thread the
 * sink started, or the owner asking for its events when it is ready for them. What happens on the
 * logging thread is the same for both: copy the event into a fixed ring, or apply the overflow
 * policy and count what was lost. That part lives here.
 *
 * Nothing in here locks. The ring is a plain data structure; the sink around it owns the mutex,
 * because each of them needs one anyway for the condition variables and counters that are their own
 * business.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/logging/event.hpp>

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace catalyst::logging
{

    /**
     * @enum overflow_policy
     * @brief What a hand-off sink does with an event it has no room for.
     */
    enum class overflow_policy : std::uint8_t
    {
        drop_newest, ///< Discard the arriving event. The logging thread never waits.
        drop_oldest, ///< Discard the oldest queued event to make room. The logging thread never waits.

        /**
         * Wait for room. Nothing is lost and the logging thread is throttled to the sink.
         *
         * Only `async_sink` accepts this, because only `async_sink` has a thread of its own that is
         * guaranteed to be draining. A `queued_sink` is drained by its owner, and a logging thread
         * blocked until the owner gets round to it is a stall at best - and a deadlock the moment
         * the owner is itself the thread that logged.
         */
        block,
    };

} // namespace catalyst::logging

namespace catalyst::logging::detail
{

    /**
     * @class event_ring
     * @brief A fixed number of event slots, oldest first, with no synchronisation of its own.
     * @details Slots are copy-assigned rather than replaced, so each one keeps whatever capacity its
     * message string has already grown to and a steady stream of events settles into allocating
     * nothing.
     */
    class event_ring
    {
    public:
        /// @param capacity How many events the ring holds; clamped up to 1.
        explicit event_ring(std::size_t capacity);

        [[nodiscard]] std::size_t capacity() const noexcept { return slots_.size(); }
        [[nodiscard]] std::size_t size() const noexcept { return count_; }
        [[nodiscard]] bool empty() const noexcept { return count_ == 0; }
        [[nodiscard]] bool full() const noexcept { return count_ == slots_.size(); }

        /// Copies an event into the newest slot. The ring must not be full.
        void push(const log_event &event);

        /// Moves the oldest event out. The ring must not be empty.
        [[nodiscard]] log_event pop();

        /// Forgets the oldest event without returning it. The ring must not be empty.
        void discard_oldest() noexcept;

    private:
        std::vector<log_event> slots_;
        std::size_t head_ = 0;
        std::size_t count_ = 0;
    };

    /**
     * @brief The event that reports a gap in the log.
     * @param category The category to file the notice under.
     * @param count How many events were dropped since the last notice.
     * @return A warning event describing the gap, stamped with the calling thread and the time now.
     * @details Dropped events are reported into the sink they were dropped on the way to, so a gap
     * says so in the place it happened rather than passing for silence.
     */
    [[nodiscard]] log_event make_drop_notice(std::string_view category, std::uint64_t count);

} // namespace catalyst::logging::detail
