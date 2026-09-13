/**
 * @file ring_buffer.hpp
 * @brief The last N events, kept for something to read later.
 * @details A GUI log panel drawing itself each frame, or a crash report wanting the run-up to the
 * failure. Nothing is written anywhere; events are retained until they are pushed out by newer ones.
 *
 *     auto history = std::make_shared<ring_buffer_sink>(1024);
 *     router.add_sink(history);
 *
 *     // ... later, on whatever thread wants to look:
 *     for (const log_event &e : history->since(last_seen))
 *         draw(e);
 *
 * Shared rather than owned by the router, because the whole point is that somebody else reads it.
 *
 * This is the sink for a reader that wants the current state of the log. A reader that wants every
 * event exactly once, on its own thread, wants `queued_sink`: this one overwrites what a slow reader
 * has not looked at yet, and says nothing about having done so.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/logging/event.hpp>
#include <catalyst/logging/sink.hpp>

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <vector>

namespace catalyst::logging
{

    /**
     * @class ring_buffer_sink
     * @brief Keeps the last `capacity` events for something to read later: a GUI log panel, a crash report.
     * @details Meant to be shared with the router through a `shared_ptr` so the reader keeps a handle.
     * `snapshot()` is everything retained, oldest first; `since()` is only what arrived after a sequence
     * number the reader saw last, which is what a panel that redraws every frame wants.
     */
    class ring_buffer_sink : public sink_contract<sink_threading::reentrant>
    {
    public:
        /// @param capacity How many events to retain; clamped up to 1.
        explicit ring_buffer_sink(std::size_t capacity = 4096);

        void write(const log_event &event);

        [[nodiscard]] std::size_t capacity() const noexcept;

        /// How many events are retained right now.
        [[nodiscard]] std::size_t size() const;

        /// How many events have ever been written, including those that have since been overwritten.
        [[nodiscard]] std::uint64_t written() const;

        /// Every retained event, oldest first.
        [[nodiscard]] std::vector<log_event> snapshot() const;

        /// The retained events with a sequence number greater than `sequence`, oldest first.
        [[nodiscard]] std::vector<log_event> since(std::uint64_t sequence) const;

        /// Drops the retained events. `written()` keeps counting from where it was.
        void clear();

    private:
        /// Copies out the retained events whose sequence is greater than `after`, oldest first. Caller holds the lock.
        [[nodiscard]] std::vector<log_event> collect_locked(std::uint64_t after) const;

        mutable std::mutex mutex_;
        std::vector<log_event> buffer_;
        std::size_t index_ = 0;
        std::size_t size_ = 0;
        std::uint64_t written_ = 0;
    };

} // namespace catalyst::logging
