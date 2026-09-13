/**
 * @file event_ring.cpp
 * @brief Implements the shared queue declared in detail/event_ring.hpp.
 * License: MIT (see LICENSE).
 */

#include <catalyst/logging/detail/event_ring.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <thread>
#include <utility>

namespace catalyst::logging::detail
{

    event_ring::event_ring(std::size_t capacity) : slots_(std::max<std::size_t>(capacity, 1))
    {
    }

    void event_ring::push(const log_event &event)
    {
        // Assigned into the slot rather than replacing it, so the string already there keeps the
        // capacity it grew to and a steady stream of events stops allocating.
        slots_[(head_ + count_) % slots_.size()] = event;
        ++count_;
    }

    log_event event_ring::pop()
    {
        log_event event = std::move(slots_[head_]);
        head_ = (head_ + 1) % slots_.size();
        --count_;
        return event;
    }

    void event_ring::discard_oldest() noexcept
    {
        head_ = (head_ + 1) % slots_.size();
        --count_;
    }

    log_event make_drop_notice(std::string_view category, std::uint64_t count)
    {
        log_event notice;
        notice.level = log_level::warn;
        notice.category = category;
        notice.message = std::format("log queue full: {} event{} dropped", count, count == 1 ? "" : "s");
        notice.timestamp = std::chrono::system_clock::now();
        notice.thread_id = std::this_thread::get_id();
        return notice;
    }

} // namespace catalyst::logging::detail
