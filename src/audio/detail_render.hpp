/**
 * @file detail_render.hpp
 * @brief Internal helpers shared by every audio backend and by the offline renderer: the atomic
 * counters behind `stream_stats`, and the dispatcher that builds a `render_block` and invokes the
 * caller's renderer.
 * @details Centralising these keeps stream-position bookkeeping, silence fallback and load
 * measurement identical across WASAPI, ASIO, null and offline - so a stat means the same thing
 * whichever produced it, and a renderer sees the same block shape everywhere.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/audio/block.hpp>
#include <catalyst/audio/stream.hpp>
#include <catalyst/audio/types.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <span>

namespace catalyst::audio::detail
{

    /**
     * @class stats_block
     * @brief The health counters for one stream.
     * @details Published with relaxed atomics. The render thread is the only writer of the timing
     * fields, so nothing on the real-time path does a read-modify-write and there is no CAS loop to
     * contend. `add_xrun` may be called from any thread. `reset()` racing a live render thread is
     * benign - it can lose at most one block.
     */
    class stats_block
    {
    public:
        /** @brief Render thread. Marks the start of a block, for load measurement. */
        void begin_block() noexcept { block_start_ = audio_clock::now(); }

        /** @brief Render thread. Records a completed block of @p frames at @p rate. */
        void end_block(std::uint32_t frames, sample_rate_t rate) noexcept
        {
            const seconds elapsed = audio_clock::now() - block_start_;

            if (elapsed > peak_)
                peak_ = elapsed;

            if (rate != 0 && frames != 0)
            {
                const double budget = static_cast<double>(frames) / static_cast<double>(rate);
                const double load = elapsed.count() / budget;
                if (load > peak_load_)
                    peak_load_ = load;
            }

            frames_.fetch_add(frames, std::memory_order_relaxed);
            blocks_.fetch_add(1, std::memory_order_relaxed);
            last_seconds_.store(elapsed.count(), std::memory_order_relaxed);
            peak_seconds_.store(peak_.count(), std::memory_order_relaxed);
            peak_load_published_.store(peak_load_, std::memory_order_relaxed);
        }

        /** @brief Any thread. Records audio that was dropped or not produced in time. */
        void add_xrun(std::uint64_t count = 1) noexcept
        {
            xruns_.fetch_add(count, std::memory_order_relaxed);
        }

        /** @brief Any thread. Records a device topology change. */
        void add_device_change() noexcept
        {
            device_changes_.fetch_add(1, std::memory_order_relaxed);
        }

        /**
         * @brief How many xruns happened since this was last asked, for coalescing into one event.
         * @details Reading and advancing the watermark in one exchange, so two pumps cannot report
         * the same xrun twice or miss one between them.
         */
        [[nodiscard]] std::uint64_t take_xruns() noexcept
        {
            const std::uint64_t total = xruns_.load(std::memory_order_relaxed);
            const std::uint64_t reported = reported_xruns_.exchange(total, std::memory_order_relaxed);
            return total >= reported ? total - reported : 0;
        }

        [[nodiscard]] stream_stats snapshot() const noexcept
        {
            stream_stats out;
            out.frames_rendered = frames_.load(std::memory_order_relaxed);
            out.blocks = blocks_.load(std::memory_order_relaxed);
            out.xruns = xruns_.load(std::memory_order_relaxed);
            out.device_changes = device_changes_.load(std::memory_order_relaxed);
            out.last_block = seconds{last_seconds_.load(std::memory_order_relaxed)};
            out.peak_block = seconds{peak_seconds_.load(std::memory_order_relaxed)};
            out.peak_load = peak_load_published_.load(std::memory_order_relaxed);
            return out;
        }

        void reset() noexcept
        {
            peak_ = seconds{0.0};
            peak_load_ = 0.0;

            frames_.store(0, std::memory_order_relaxed);
            blocks_.store(0, std::memory_order_relaxed);
            xruns_.store(0, std::memory_order_relaxed);
            reported_xruns_.store(0, std::memory_order_relaxed);
            device_changes_.store(0, std::memory_order_relaxed);
            last_seconds_.store(0.0, std::memory_order_relaxed);
            peak_seconds_.store(0.0, std::memory_order_relaxed);
            peak_load_published_.store(0.0, std::memory_order_relaxed);
        }

    private:
        // Render-thread-owned scratch; never read by another thread.
        audio_time block_start_{};
        seconds peak_{0.0};
        double peak_load_ = 0.0;

        std::atomic<frame_count> frames_{0};
        std::atomic<std::uint64_t> blocks_{0};
        std::atomic<std::uint64_t> xruns_{0};
        std::atomic<std::uint64_t> reported_xruns_{0};
        std::atomic<std::uint64_t> device_changes_{0};
        std::atomic<double> last_seconds_{0.0};
        std::atomic<double> peak_seconds_{0.0};
        std::atomic<double> peak_load_published_{0.0};
    };

    /**
     * @class render_dispatcher
     * @brief Invokes the caller's renderer for one block, maintaining stream position and stats.
     * @details Owns the monotonic frame counter behind `render_block::position`, which is the only
     * correct clock for a renderer: a 64-bit integer stays exact for longer than any process runs,
     * where a float accumulator drifts and a float counter stops counting at 2^24.
     *
     * Guarantees the output buffer is fully written even when no renderer was given, so a stream
     * with an empty `renderer` plays silence rather than whatever the driver's buffer held.
     */
    class render_dispatcher
    {
    public:
        render_dispatcher(renderer render, stats_block &stats) noexcept
            : render_(render), stats_(&stats) {}

        /** @brief Render thread. Either span may be empty. */
        void dispatch(
            std::span<sample> output,
            std::span<const sample> input,
            std::uint32_t frames,
            channel_count output_channels,
            channel_count input_channels,
            sample_rate_t rate) noexcept
        {
            if (frames == 0)
                return;

            if (!render_)
            {
                std::fill(output.begin(), output.end(), sample{0});
                position_.fetch_add(frames, std::memory_order_relaxed);
                return;
            }

            render_block block;
            block.output = output;
            block.input = input;
            block.frames = frames;
            block.output_channels = output_channels;
            block.input_channels = input_channels;
            block.sample_rate = rate;
            block.position = position_.load(std::memory_order_relaxed);

            stats_->begin_block();
            render_(block);
            stats_->end_block(frames, rate);

            position_.fetch_add(frames, std::memory_order_relaxed);
        }

        /** @brief Frames dispatched since the last `rewind()`. */
        [[nodiscard]] frame_count position() const noexcept
        {
            return position_.load(std::memory_order_relaxed);
        }

        void rewind() noexcept { position_.store(0, std::memory_order_relaxed); }

        [[nodiscard]] bool has_renderer() const noexcept { return static_cast<bool>(render_); }

    private:
        renderer render_{};
        stats_block *stats_ = nullptr;
        std::atomic<frame_count> position_{0};
    };

} // namespace catalyst::audio::detail
