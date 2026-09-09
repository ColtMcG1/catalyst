/**
 * @file queued.cpp
 * @brief Implements the owner-drained sink declared in queued.hpp.
 * @details One lock covers the ring and the counters, and is held only for as long as it takes to
 * copy an event in or move one out. The inner sink is never called under it: `drain` takes an event,
 * drops the lock, delivers, and takes the lock again, so a slow inner sink does not stop logging
 * threads from queueing behind it.
 *
 * A second lock covers delivery itself. Draining from two threads at once is a mistake, but it is a
 * mistake that would otherwise show up as corruption inside the user's sink rather than as the
 * serialisation they assumed they had.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/logging/sinks/queued.hpp>

#include <utility>

namespace catalyst::logging
{

    queued_sink::queued_sink(detail::sink_entry inner, queued_options options)
        : inner_(std::move(inner)), drop_category_(options.drop_category),
          // `block` would park the logging thread until the owner next drains: a stall when the
          // owner is busy, and a deadlock when the owner is the thread that logged. The nearest
          // policy that keeps the newest events is the one it gets instead.
          overflow_(options.overflow == overflow_policy::block ? overflow_policy::drop_oldest : options.overflow),
          queue_(options.capacity)
    {
    }

    queued_sink::~queued_sink() = default;

    void queued_sink::write(const log_event &event)
    {
        std::scoped_lock lock(mutex_);
        if (queue_.full())
        {
            if (overflow_ == overflow_policy::drop_newest)
            {
                ++dropped_;
                return;
            }
            queue_.discard_oldest();
            ++dropped_;
        }
        queue_.push(event);
    }

    std::size_t queued_sink::drain()
    {
        return drain(std::numeric_limits<std::size_t>::max());
    }

    std::size_t queued_sink::drain(std::size_t max_events)
    {
        // A sink that throws must not take down the thread that was only trying to draw a log panel.
        const auto deliver = [this](const log_event &event) noexcept {
            try
            {
                line_cache cache(event);
                inner_.write(event, cache);
            }
            catch (...)
            {
            }
        };

        std::scoped_lock delivery(delivery_mutex_);

        std::size_t delivered = 0;
        while (delivered < max_events)
        {
            std::unique_lock lock(mutex_);

            const std::uint64_t gap = dropped_ - reported_drops_;
            reported_drops_ = dropped_;

            if (queue_.empty())
            {
                // Still worth reporting a gap that opened after the last event was taken.
                if (gap > 0)
                {
                    lock.unlock();
                    deliver(detail::make_drop_notice(drop_category_, gap));
                }
                break;
            }

            const log_event notice = gap > 0 ? detail::make_drop_notice(drop_category_, gap) : log_event{};
            const log_event event = queue_.pop();
            ++delivered_;
            lock.unlock();

            if (gap > 0)
                deliver(notice);
            deliver(event);
            ++delivered;
        }
        return delivered;
    }

    void queued_sink::flush_inner()
    {
        std::scoped_lock delivery(delivery_mutex_);
        if (!inner_.flush)
            return;
        try
        {
            inner_.flush();
        }
        catch (...)
        {
        }
    }

    std::size_t queued_sink::pending() const
    {
        std::scoped_lock lock(mutex_);
        return queue_.size();
    }

    std::size_t queued_sink::capacity() const
    {
        std::scoped_lock lock(mutex_);
        return queue_.capacity();
    }

    std::uint64_t queued_sink::dropped() const
    {
        std::scoped_lock lock(mutex_);
        return dropped_;
    }

    std::uint64_t queued_sink::delivered() const
    {
        std::scoped_lock lock(mutex_);
        return delivered_;
    }

} // namespace catalyst::logging
