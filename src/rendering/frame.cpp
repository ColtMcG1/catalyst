/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief `frame_ring` and `frame`.
 * @details Built entirely on the public API - `create_command_pool`, `reset_command_pool`,
 * `timeline_point::wait` - and on purpose: the ring is bookkeeping, not a backend concept, and a
 * backend that had to implement it would implement it the same way three times. It sits above the
 * module lock rather than inside it, so nothing here holds a lock across the wait in `begin`.
 */

#include <catalyst/rendering/frame.hpp>

#include <catalyst/rendering/queue.hpp>

#include "detail_log.hpp"

#include <algorithm>
#include <string>
#include <utility>

namespace catalyst::rendering
{

    namespace
    {
        constexpr std::size_t kinds = queue_kind_count;

        std::string pool_name(const char *prefix, std::uint32_t slot, std::uint32_t worker, queue_kind kind)
        {
            std::string name = prefix ? prefix : "frame_ring";
            name += " frame ";
            name += std::to_string(slot);
            name += " worker ";
            name += std::to_string(worker);
            name += ' ';
            name += to_string(kind);
            return name;
        }
    } // namespace

    // -------------------------------------------------------------------------
    // frame
    // -------------------------------------------------------------------------

    command_pool frame::pool(std::uint32_t worker, queue_kind kind) const noexcept
    {
        if (!ring_)
            return {};
        return ring_->pool_at(slot_, worker, kind);
    }

    void frame::end(const timeline_point &done) noexcept
    {
        end(std::span<const timeline_point>{&done, 1});
    }

    void frame::end(std::span<const timeline_point> done) noexcept
    {
        if (!ring_)
            return;
        ring_->record_end(slot_, done);
    }

    // -------------------------------------------------------------------------
    // frame_ring
    // -------------------------------------------------------------------------

    std::expected<frame_ring, error> frame_ring::create(const device &dev, const frame_ring_desc &desc)
    {
        if (!is_valid(dev))
            return std::unexpected(make_error(error_code::invalid_argument, "frame_ring::create"));

        frame_ring ring;
        ring.device_ = dev;
        ring.frames_in_flight_ = std::clamp(desc.frames_in_flight, 1u, max_frames_in_flight);
        ring.workers_ = std::clamp(desc.workers, 1u, max_frame_workers);

        const std::size_t count = static_cast<std::size_t>(ring.frames_in_flight_) * ring.workers_ * kinds;
        ring.pools_.reserve(count);
        ring.points_.resize(ring.frames_in_flight_);

        for (std::uint32_t slot = 0; slot < ring.frames_in_flight_; ++slot)
        {
            for (std::uint32_t worker = 0; worker < ring.workers_; ++worker)
            {
                for (std::size_t k = 0; k < kinds; ++k)
                {
                    const auto kind = static_cast<queue_kind>(k);
                    const std::string name = pool_name(desc.debug_name, slot, worker, kind);
                    command_pool pool = create_command_pool(dev, {.queue = kind, .debug_name = name.c_str()});
                    if (!is_valid(pool))
                    {
                        // Partially built rings are worse than none: a caller that ignored the
                        // error would get an invalid pool from `frame::pool` several frames later,
                        // a long way from the cause.
                        ring.destroy();
                        return std::unexpected(make_error(error_code::out_of_host_memory, "frame_ring::create"));
                    }
                    ring.pools_.push_back(pool);
                }
            }
        }

        logging::debug<detail::render_log>("frame_ring: {} frames in flight, {} worker(s), {} pools",
                                           ring.frames_in_flight_, ring.workers_, ring.pools_.size());
        return ring;
    }

    frame_ring::frame_ring(frame_ring &&other) noexcept
        : device_(other.device_), frames_in_flight_(other.frames_in_flight_), workers_(other.workers_),
          next_index_(other.next_index_), open_(other.open_), open_slot_(other.open_slot_),
          pools_(std::move(other.pools_)), points_(std::move(other.points_))
    {
        other.device_ = {};
        other.frames_in_flight_ = 0;
        other.workers_ = 0;
        other.next_index_ = 0;
        other.open_ = false;
    }

    frame_ring &frame_ring::operator=(frame_ring &&other) noexcept
    {
        if (this == &other)
            return *this;
        destroy();

        device_ = other.device_;
        frames_in_flight_ = other.frames_in_flight_;
        workers_ = other.workers_;
        next_index_ = other.next_index_;
        open_ = other.open_;
        open_slot_ = other.open_slot_;
        pools_ = std::move(other.pools_);
        points_ = std::move(other.points_);

        other.device_ = {};
        other.frames_in_flight_ = 0;
        other.workers_ = 0;
        other.next_index_ = 0;
        other.open_ = false;
        return *this;
    }

    frame_ring::~frame_ring()
    {
        destroy();
    }

    void frame_ring::destroy() noexcept
    {
        for (command_pool &pool : pools_)
            destroy_command_pool(pool); // Waits for anything of its own still in flight.
        pools_.clear();
        points_.clear();
        device_ = {};
        frames_in_flight_ = 0;
        workers_ = 0;
        open_ = false;
    }

    command_pool frame_ring::pool_at(std::uint32_t slot, std::uint32_t worker, queue_kind kind) const noexcept
    {
        if (slot >= frames_in_flight_ || worker >= workers_)
            return {};
        const std::size_t index = ((static_cast<std::size_t>(slot) * workers_) + worker) * kinds +
                                  static_cast<std::size_t>(kind);
        return index < pools_.size() ? pools_[index] : command_pool{};
    }

    void frame_ring::record_end(std::uint32_t slot, std::span<const timeline_point> done) noexcept
    {
        if (slot >= points_.size())
            return;

        slot_points &points = points_[slot];
        for (const timeline_point &point : done)
        {
            if (!point.valid())
                continue;
            timeline_point &slot_point = points[static_cast<std::size_t>(point.queue())];
            slot_point = latest(slot_point, point);
        }

        if (slot == open_slot_)
            open_ = false;
    }

    std::expected<frame, error> frame_ring::begin()
    {
        if (!valid())
            return std::unexpected(make_error(error_code::invalid_argument, "frame_ring::begin"));
        if (is_device_lost(device_))
            return std::unexpected(make_error(error_code::device_lost, "frame_ring::begin"));

        if (open_)
        {
            // The caller forgot `frame::end`. Closing the open frame against everything currently
            // submitted is always safe - it can only wait for more than it had to - and is a great
            // deal safer than resetting pools whose lists may still be executing.
            logging::warn<detail::render_log>(
                "frame_ring: frame {} was never ended; closing it against all outstanding work",
                next_index_ - 1);
            timeline_point all[queue_kind_count];
            for (std::size_t k = 0; k < kinds; ++k)
                all[k] = last_submitted(get_queue(device_, static_cast<queue_kind>(k)));
            record_end(open_slot_, all);
        }

        const std::uint64_t index = next_index_;
        const auto slot = static_cast<std::uint32_t>(index % frames_in_flight_);

        // The honest stall. Everything this slot's pools recorded `frames_in_flight_` frames ago
        // has to have finished before the pools can be recycled.
        for (const timeline_point &point : points_[slot])
        {
            if (auto ok = point.wait(); !ok)
                return std::unexpected(ok.error());
        }
        points_[slot] = {};

        for (std::uint32_t worker = 0; worker < workers_; ++worker)
        {
            for (std::size_t k = 0; k < kinds; ++k)
            {
                const command_pool pool = pool_at(slot, worker, static_cast<queue_kind>(k));
                if (auto ok = reset_command_pool(pool); !ok)
                {
                    // The wait above should have made this impossible, so it means something was
                    // submitted from this slot's pools without its point ever reaching `end`.
                    logging::error<detail::render_log>("frame_ring: pool {} of frame slot {} could not be reset: {}",
                                                       worker, slot, ok.error());
                    return std::unexpected(ok.error());
                }
            }
        }

        next_index_ = index + 1;
        open_ = true;
        open_slot_ = slot;
        return frame{this, slot, index};
    }

    std::expected<void, error> frame_ring::wait_all()
    {
        if (!valid())
            return std::unexpected(make_error(error_code::invalid_argument, "frame_ring::wait_all"));

        std::expected<void, error> result;
        for (slot_points &points : points_)
        {
            for (const timeline_point &point : points)
            {
                if (auto ok = point.wait(); !ok && result)
                    result = std::unexpected(ok.error());
            }
            points = {};
        }
        if (!result)
            return result;

        for (const command_pool &pool : pools_)
        {
            if (auto ok = reset_command_pool(pool); !ok)
                return ok;
        }
        open_ = false;
        return {};
    }

} // namespace catalyst::rendering
