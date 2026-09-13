/**
 * @file detail_backend_base.hpp
 * @brief The state and reporting every live backend has, so that a backend only has to contain the
 * part of itself that is actually about its platform API.
 * @details The `backend` interface in detail_backend.hpp says what a backend must answer; it says
 * nothing about how, and each backend used to answer the easy two thirds of it the same way, from
 * the same six members. WASAPI and ASIO had separate, character-for-character identical
 * `is_running`, `stats`, `reset_stats`, `take_xruns` and `failure` - and ASIO, having no copy of
 * WASAPI's `take_failure`, silently had no way to report that its driver had died at all.
 *
 * This base holds those members once. What remains in a backend is device selection, format
 * negotiation and the render path: the things that genuinely differ between WASAPI's COM endpoints,
 * ASIO's driver callbacks and the null backend's nothing.
 *
 * The one subtlety is @ref notice_publisher. A platform can notify us that a device vanished from a
 * thread we do not own, at a moment when the backend is already being torn down, so the state such
 * a notification touches cannot be the backend's own. It is held by `shared_ptr` on both sides
 * instead: teardown retires it and drops its reference, and whatever is still in flight keeps it
 * alive for exactly as long as it needs.
 * License: MIT (see LICENSE).
 */

#pragma once

#include "detail_backend.hpp"
#include "detail_render.hpp"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

namespace catalyst::audio::detail
{

    /**
     * @class notice_publisher
     * @brief Turns a platform notification into a `device_notice` on the queue `stream::pump()`
     * drains, and counts it, from any thread and without touching the backend.
     * @details The device identifier is fixed at construction rather than read from the backend,
     * so a notification racing `close()` cannot observe a string being cleared underneath it.
     */
    class notice_publisher
    {
    public:
        notice_publisher(notice_queue *notices, stream_direction direction, std::string device_id) noexcept
            : notices_(notices), direction_(direction), device_id_(std::move(device_id))
        {
        }

        notice_publisher(const notice_publisher &) = delete;
        notice_publisher &operator=(const notice_publisher &) = delete;

        /** @brief Any thread. Reports @p what about the device this stream opened. */
        void fire(device_notice::kind what) noexcept { emit(what, device_id_); }

        /**
         * @brief Any thread. Reports @p what about some other device the platform named.
         * @details Takes a view rather than a string so that copying the identifier happens inside
         * this object's handler and not in the caller's frame - the callers are `noexcept` driver
         * callbacks, where an allocation failure one frame too early ends the process.
         */
        void fire(device_notice::kind what, std::string_view device_id) noexcept
        {
            emit(what, device_id);
        }

        /**
         * @brief Any thread. Counts a change that has no notice shape - a driver changing the
         * sample rate underneath a running stream - so `stream_stats::device_changes` sees it.
         */
        void note() noexcept { changes_.fetch_add(1, std::memory_order_relaxed); }

        /** @brief Stops delivery. The counter keeps working, so a late change is still counted. */
        void retire() noexcept { active_.store(false, std::memory_order_release); }

        [[nodiscard]] std::uint64_t changes() const noexcept
        {
            return changes_.load(std::memory_order_relaxed);
        }

    private:
        void emit(device_notice::kind what, std::string_view device_id) noexcept
        {
            changes_.fetch_add(1, std::memory_order_relaxed);

            if (!active_.load(std::memory_order_acquire) || !notices_)
                return;

            try
            {
                // Queued rather than delivered: this may be a driver's notification thread, and
                // nothing the application wrote should be made to run on it. `pump()` publishes.
                device_notice notice;
                notice.what = what;
                notice.device_id = std::string(device_id);
                notice.direction = direction_;
                notice.time = audio_clock::now();
                notices_->push(std::move(notice));
            }
            catch (...)
            {
                // Allocating the identifier is the only thing here that can fail, and a dropped
                // notice costs one event. Throwing out of a driver callback would cost the process.
            }
        }

        std::atomic<bool> active_{true};
        std::atomic<std::uint64_t> changes_{0};

        notice_queue *notices_ = nullptr;
        stream_direction direction_ = stream_direction::output;
        const std::string device_id_;
    };

    /**
     * @class backend_base
     * @brief What every backend is made of below its platform API: the request, the render
     * dispatcher and its stats, the identity of the device it settled on, and the two channels a
     * failure or a topology change reaches the caller through.
     * @details A derived backend supplies `enumerate_devices`, `open`, `start`, `stop`, `close` and
     * `info`; everything else on the `backend` interface is answered from here.
     */
    class backend_base : public backend
    {
    public:
        backend_base(backend_kind kind, open_request request) noexcept
            : kind_(kind), request_(std::move(request)), dispatcher_(request_.render, stats_)
        {
        }

        [[nodiscard]] backend_kind kind() const noexcept final { return kind_; }

        [[nodiscard]] bool is_running() const noexcept override
        {
            return running_.load(std::memory_order_acquire);
        }

        [[nodiscard]] stream_stats stats() const noexcept override
        {
            stream_stats out = stats_.snapshot();

            // The publisher outlives the backend and counts changes that arrive during teardown,
            // so it, not `stats_`, is the authority on how many there have been.
            if (notices_)
            {
                const std::uint64_t total = notices_->changes();
                const std::uint64_t baseline = change_baseline_.load(std::memory_order_relaxed);
                out.device_changes = total >= baseline ? total - baseline : 0;
            }

            return out;
        }

        void reset_stats() noexcept override
        {
            if (notices_)
                change_baseline_.store(notices_->changes(), std::memory_order_relaxed);
            stats_.reset();
        }

        [[nodiscard]] std::uint64_t take_xruns() noexcept override { return stats_.take_xruns(); }

        /// The failure the render thread found, if any, clearing it so it is reported once.
        [[nodiscard]] std::optional<error> take_failure() noexcept override
        {
            const error_code code = stream_error_.exchange(error_code::none, std::memory_order_relaxed);
            if (code == error_code::none)
                return std::nullopt;
            return make_error(code, kind_);
        }

    protected:
        /** @brief Every failure from a backend names that backend, so the caller's log line does. */
        [[nodiscard]] std::unexpected<error> failure(error_code code) const noexcept
        {
            return std::unexpected(make_error(code, kind_));
        }

        /**
         * @brief Records why the stream died, once, from whichever thread found out.
         * @details Only the first failure is kept: a device that vanishes will usually produce a
         * cascade of them, and the first one is the one that says what happened.
         */
        void fail_stream(error_code code) noexcept
        {
            auto expected = error_code::none;
            if (!stream_error_.compare_exchange_strong(expected, code, std::memory_order_relaxed))
                return;

            if (code == error_code::device_lost && notices_)
                notices_->fire(device_notice::kind::lost);
        }

        /** @brief Clears any recorded failure, so a restarted stream does not report an old one. */
        void clear_failure() noexcept
        {
            stream_error_.store(error_code::none, std::memory_order_relaxed);
        }

        /**
         * @brief Records which device was opened and arms notice delivery for it.
         * @details Called as soon as a backend has chosen a device, before any format negotiation,
         * because negotiation can fail and the identity is what says which device refused.
         */
        void identify(std::string device_id, std::string device_name)
        {
            device_id_ = std::move(device_id);
            device_name_ = device_name.empty() ? device_id_ : std::move(device_name);

            notices_ = std::make_shared<notice_publisher>(
                request_.notices, request_.direction, device_id_);
            change_baseline_.store(0, std::memory_order_relaxed);
        }

        /** @brief Marks the stream ready to start, with a rewound position and clean stats. */
        void mark_opened() noexcept
        {
            dispatcher_.rewind();
            stats_.reset();
            opened_ = true;
        }

        /** @brief Retires notice delivery and forgets the device. Safe when never opened. */
        void mark_closed() noexcept
        {
            if (notices_)
            {
                notices_->retire();
                notices_.reset();
            }

            device_id_.clear();
            device_name_.clear();
            set_rate(0);
            opened_ = false;
        }

        /** @brief The fields of `stream_info` that do not depend on the platform API. */
        [[nodiscard]] stream_info base_info() const
        {
            stream_info out;
            out.backend = kind_;
            out.direction = request_.direction;
            out.sample_rate = rate();
            out.device_id = device_id_;
            out.device_name = device_name_;
            return out;
        }

        /**
         * @brief The rate the stream is actually running at.
         * @details Atomic because a driver may tell us on its own thread that the rate changed
         * underneath us - ASIO does - while `info()` is answering on the caller's.
         */
        [[nodiscard]] sample_rate_t rate() const noexcept
        {
            return sample_rate_.load(std::memory_order_relaxed);
        }

        void set_rate(sample_rate_t rate) noexcept
        {
            sample_rate_.store(rate, std::memory_order_relaxed);
        }

        [[nodiscard]] const std::shared_ptr<notice_publisher> &publisher() const noexcept
        {
            return notices_;
        }

        const backend_kind kind_;
        open_request request_{};
        stats_block stats_;
        render_dispatcher dispatcher_;

        std::string device_id_;
        std::string device_name_;

        bool opened_ = false;
        std::atomic<bool> running_{false};

    private:
        std::atomic<sample_rate_t> sample_rate_{0};
        std::shared_ptr<notice_publisher> notices_;
        std::atomic<std::uint64_t> change_baseline_{0};
        std::atomic<error_code> stream_error_{error_code::none};
    };

} // namespace catalyst::audio::detail
