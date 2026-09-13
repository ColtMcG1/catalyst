/**
 * @file async.cpp
 * @brief Implements the queueing sink declared in async.hpp.
 * @details One mutex covers the ring and the counters; two condition variables carry the two
 * directions of traffic. `to_worker_` wakes the worker when there is something to write, a flush to
 * perform or a shutdown to notice. `to_caller_` wakes logging threads: one blocked on a full queue
 * waiting for room, or one inside `flush()` waiting for its turn to come round.
 *
 * The accounting is three counters that always satisfy `processed_ + queue_.size() == enqueued_`. An
 * event discarded to make room counts as processed, because nothing is ever going to write it, and a
 * `flush()` waiting for everything queued before it would otherwise wait forever.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/logging/sinks/async.hpp>

#include <utility>

namespace catalyst::logging
{

    async_sink::async_sink(detail::sink_entry inner, async_options options)
        : inner_(std::move(inner)), drop_category_(options.drop_category), overflow_(options.overflow),
          flush_timeout_(options.flush_timeout), queue_(options.capacity)
    {
        worker_ = std::thread(&async_sink::run, this);
    }

    async_sink::~async_sink()
    {
        {
            std::scoped_lock lock(mutex_);
            stopping_ = true;
        }
        // The worker drains what is queued before it looks at stopping_, so shutdown loses nothing
        // that was accepted. Anything still blocked on a full queue gives up instead.
        to_worker_.notify_all();
        to_caller_.notify_all();
        if (worker_.joinable())
            worker_.join();
    }

    void async_sink::write(const log_event &event)
    {
        std::unique_lock lock(mutex_);
        if (stopping_)
            return;

        if (queue_.full())
        {
            switch (overflow_)
            {
            case overflow_policy::drop_newest:
                ++dropped_;
                return;

            case overflow_policy::drop_oldest:
                queue_.discard_oldest();
                ++dropped_;
                ++processed_; // Nothing will write it, so it is accounted for here instead.
                break;

            case overflow_policy::block:
                to_caller_.wait(lock, [this] { return !queue_.full() || stopping_; });
                if (stopping_)
                {
                    ++dropped_;
                    return;
                }
                break;
            }
        }

        queue_.push(event);
        ++enqueued_;
        lock.unlock();
        to_worker_.notify_one();
    }

    void async_sink::flush()
    {
        static_cast<void>(flush_within(flush_timeout_));
    }

    bool async_sink::flush_within(std::chrono::milliseconds timeout)
    {
        std::unique_lock lock(mutex_);
        if (stopping_)
            return false;
        const std::uint64_t ticket = ++flush_requests_;
        // Everything queued as of now, so a busy logger cannot keep pushing the target away.
        flush_target_ = enqueued_;
        to_worker_.notify_all();

        const auto done = [this, ticket] { return flushes_done_ >= ticket || stopping_; };
        if (timeout <= std::chrono::milliseconds::zero())
        {
            to_caller_.wait(lock, done);
            return !stopping_;
        }
        // A flush that gives up still leaves its ticket outstanding: the worker will satisfy it in
        // its own time, and the next flush takes a later one, so nothing is left permanently waiting.
        return to_caller_.wait_for(lock, timeout, done) && !stopping_;
    }

    std::size_t async_sink::pending() const
    {
        std::scoped_lock lock(mutex_);
        return queue_.size();
    }

    std::size_t async_sink::capacity() const
    {
        std::scoped_lock lock(mutex_);
        return queue_.capacity();
    }

    std::uint64_t async_sink::dropped() const
    {
        std::scoped_lock lock(mutex_);
        return dropped_;
    }

    void async_sink::run()
    {
        // A sink that throws must not take the process down from a thread nobody is watching.
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

        std::unique_lock lock(mutex_);
        for (;;)
        {
            to_worker_.wait(lock,
                            [this] { return !queue_.empty() || stopping_ || flushes_done_ < flush_requests_; });

            if (!queue_.empty())
            {
                const std::uint64_t gap = dropped_ - reported_drops_;
                const log_event notice = gap > 0 ? detail::make_drop_notice(drop_category_, gap) : log_event{};
                reported_drops_ = dropped_;
                const log_event event = queue_.pop();

                lock.unlock();
                if (gap > 0)
                    deliver(notice);
                deliver(event);
                lock.lock();

                ++processed_;
                // Wakes both a flush waiting on the drain and a producer waiting for room.
                to_caller_.notify_all();
                continue;
            }

            if (flushes_done_ < flush_requests_ && processed_ >= flush_target_)
            {
                const std::uint64_t done = flush_requests_;
                lock.unlock();
                if (inner_.flush)
                {
                    try
                    {
                        inner_.flush();
                    }
                    catch (...)
                    {
                    }
                }
                lock.lock();
                flushes_done_ = done;
                to_caller_.notify_all();
                continue;
            }

            if (stopping_)
                break;
        }
    }

} // namespace catalyst::logging
