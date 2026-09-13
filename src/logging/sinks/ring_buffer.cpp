/**
 * @file ring_buffer.cpp
 * @brief Implements `ring_buffer_sink`.
 * @details The events sit in a fixed vector in insertion order, and every read walks it from the
 * oldest entry, which is why `collect_locked` is the only place the wrap-around is spelled out.
 * License: MIT (see LICENSE).
 */

#include <catalyst/logging/sinks/ring_buffer.hpp>

#include <algorithm>

namespace catalyst::logging
{

    ring_buffer_sink::ring_buffer_sink(std::size_t capacity) : buffer_(std::max<std::size_t>(capacity, 1)) {}

    void ring_buffer_sink::write(const log_event &event)
    {
        std::scoped_lock lock(mutex_);
        buffer_[index_] = event;
        index_ = (index_ + 1) % buffer_.size();
        size_ = std::min(size_ + 1, buffer_.size());
        ++written_;
    }

    std::size_t ring_buffer_sink::capacity() const noexcept
    {
        return buffer_.size();
    }

    std::size_t ring_buffer_sink::size() const
    {
        std::scoped_lock lock(mutex_);
        return size_;
    }

    std::uint64_t ring_buffer_sink::written() const
    {
        std::scoped_lock lock(mutex_);
        return written_;
    }

    std::vector<log_event> ring_buffer_sink::snapshot() const
    {
        std::scoped_lock lock(mutex_);
        return collect_locked(0);
    }

    std::vector<log_event> ring_buffer_sink::since(std::uint64_t sequence) const
    {
        std::scoped_lock lock(mutex_);
        return collect_locked(sequence);
    }

    void ring_buffer_sink::clear()
    {
        std::scoped_lock lock(mutex_);
        index_ = 0;
        size_ = 0;
    }

    std::vector<log_event> ring_buffer_sink::collect_locked(std::uint64_t after) const
    {
        std::vector<log_event> events;
        events.reserve(size_);
        const std::size_t first = (index_ + buffer_.size() - size_) % buffer_.size();
        for (std::size_t offset = 0; offset < size_; ++offset)
        {
            const log_event &e = buffer_[(first + offset) % buffer_.size()];
            // `after == 0` is "everything": the router stamps sequences from 1, but an event
            // written straight to a sink, as a test does, keeps the 0 it was built with.
            if (after == 0 || e.sequence > after)
                events.push_back(e);
        }
        return events;
    }

} // namespace catalyst::logging
