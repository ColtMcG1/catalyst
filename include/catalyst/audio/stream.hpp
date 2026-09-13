/**
 * @file stream.hpp
 * @brief An open audio device: what to ask for, what was negotiated, how it is doing, and the
 * object itself.
 * @details A @ref stream is opened by a factory that returns one only on success, so a stream that
 * exists is a stream that is open. That is the whole reason the old `initialize()` / `shutdown()`
 * pair is gone, and with it `is_initialized()`, `not_initialized` and `already_initialized`: three
 * error codes and a query that existed only to describe an object that had been constructed but did
 * not yet mean anything. Now the constructor is the negotiation, the destructor is the close, and
 * the states a caller can observe are the two that matter - open, and open and running.
 *
 * A stream is moveable and not copyable. Moving it is safe at any time, including while running.
 *
 * The threads involved, because two of them are not the caller's:
 *
 *   - **The caller's thread** opens, starts, stops and pumps. None of those are safe to call
 *     concurrently on the same stream; drive one stream from one thread.
 *   - **The driver's render thread** calls the @ref renderer, and nothing else. It is created by
 *     @ref start and joined by @ref stop.
 *   - **A platform notification thread** observes devices appearing and disappearing. It only ever
 *     pushes onto an internal queue; @ref pump drains it onto the caller's bus, on the caller's
 *     thread. See events.hpp.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/audio/backend.hpp>
#include <catalyst/audio/block.hpp>
#include <catalyst/audio/device.hpp>
#include <catalyst/audio/error.hpp>
#include <catalyst/audio/types.hpp>

#include <cstdint>
#include <expected>
#include <memory>
#include <string>
#include <string_view>

namespace catalyst
{
    namespace events
    {
        class bus;
    }
} // namespace catalyst

namespace catalyst::audio
{

    /**
     * @struct stream_config
     * @brief What to ask a device for.
     * @details Every field is a request, not a guarantee. A device is free to refuse the rate, the
     * channel count or the block size, and unless @ref allow_format_fallback is false the stream
     * will accept what it offers instead of failing. Read @ref stream::info after opening to find
     * out what was actually agreed - never assume this struct describes the running stream.
     */
    struct stream_config
    {
        /** @brief Which platform API to drive. @ref backend_kind::automatic picks
         * @ref default_backend. */
        backend_kind backend = backend_kind::automatic;

        /** @brief Which endpoint to open. Defaults to the system default for @ref direction. */
        device_selector device{};

        /** @brief Which half, or both halves, of the device to open. */
        stream_direction direction = stream_direction::output;

        /** @brief Frames per second to ask for. */
        sample_rate_t sample_rate = 48000;

        /** @brief Channels to render. Must be non-zero for a direction that has output. */
        channel_count output_channels = 2;

        /** @brief Channels to capture. Must be non-zero for a direction that has input. */
        channel_count input_channels = 0;

        /**
         * @brief Frames per callback to ask for.
         * @details A hint, and a trade: smaller means lower latency and less time to make the
         * deadline. Read @ref stream_info::block_frames for what the device agreed to, and never
         * assume it in the renderer - @ref render_block::frames is the only truth per call.
         */
        std::uint32_t block_frames = 512;

        /**
         * @brief Take exclusive ownership of the device for lower latency.
         * @details The device may refuse, or already be held by another process, which fails with
         * @ref error_code::device_busy. Exclusive mode also bypasses the system mixer, so nothing
         * else on the machine is audible while the stream is open.
         */
        bool exclusive = false;

        /**
         * @brief Accept a different rate, channel count or format if the device refuses this one.
         * @details When false, a mismatch fails with @ref error_code::format_unsupported and the
         * error carries what the device would have accepted.
         */
        bool allow_format_fallback = true;

        /**
         * @brief Where device and stream events go. Optional; null publishes nothing.
         * @details Must outlive the stream. Events are queued as they are observed and published
         * from @ref stream::pump, on the thread that calls it. See events.hpp.
         */
        events::bus *bus = nullptr;
    };

    /**
     * @struct stream_info
     * @brief The format a stream actually negotiated.
     * @details Read it after opening. The device is entitled to refuse everything that was asked
     * for, and with @ref stream_config::allow_format_fallback left on it will do so silently and
     * successfully - which is the correct behaviour and the reason this struct exists.
     */
    struct stream_info
    {
        /** @brief The backend driving it. Resolved, never @ref backend_kind::automatic. */
        backend_kind backend = backend_kind::automatic;

        /** @brief The direction that was opened. */
        stream_direction direction = stream_direction::output;

        /** @brief The rate that was agreed. */
        sample_rate_t sample_rate = 0;

        /** @brief Channels being rendered. Zero for a capture-only stream. */
        channel_count output_channels = 0;

        /** @brief Channels being captured. Zero for a render-only stream. */
        channel_count input_channels = 0;

        /** @brief The layout the output channel count conventionally means, for display. */
        channel_layout output_layout = channel_layout::unspecified;

        /** @brief Nominal frames per callback. Actual block sizes vary; see
         * @ref render_block::frames. */
        std::uint32_t block_frames = 0;

        /** @brief How far behind the speaker the renderer is working. */
        seconds output_latency{0.0};

        /** @brief How far behind the microphone the renderer is working. */
        seconds input_latency{0.0};

        /** @brief True if the device was opened exclusively. */
        bool exclusive = false;

        /** @brief The endpoint's stable id - the one to persist for next time. */
        std::string device_id;

        /** @brief The endpoint's display name. */
        std::string device_name;
    };

    /**
     * @struct stream_stats
     * @brief Health counters for a running stream. Sampled atomically; cheap enough to poll each
     * frame.
     */
    struct stream_stats
    {
        /** @brief Frames delivered since the stream opened or @ref stream::reset_stats. */
        frame_count frames_rendered = 0;

        /** @brief Blocks delivered. Divide into @ref frames_rendered for the average block size. */
        std::uint64_t blocks = 0;

        /**
         * @brief Blocks the stream could not service in time, or device events that dropped audio.
         * @details Any non-zero value here was audible. Also published as an @ref xrun_event.
         */
        std::uint64_t xruns = 0;

        /** @brief Device topology changes observed. */
        std::uint64_t device_changes = 0;

        /** @brief How long the last block took to render. */
        seconds last_block{0.0};

        /** @brief The longest any block took. */
        seconds peak_block{0.0};

        /**
         * @brief Peak block cost as a fraction of the block's wall-clock budget.
         * @details 1.0 means a block took exactly as long as it lasts, which is already too long.
         * Sustained values above about 0.7 will glitch on a machine doing anything else.
         */
        double peak_load = 0.0;
    };

    /**
     * @class stream
     * @brief An open audio device, delivering blocks to a renderer on the driver's thread.
     * @details Built by @ref open, closed by the destructor. See the file comment for the threads.
     */
    class stream
    {
    public:
        /**
         * @brief Opens a device and negotiates a format.
         * @param config What to ask for. See @ref stream_config.
         * @param render The callable that fills each block. It must outlive the stream - @ref
         * renderer refers to it rather than owning it - and must obey the real-time rules in
         * block.hpp. An empty renderer is accepted and renders silence.
         * @return An open, silent stream; call @ref start to begin delivering blocks. On failure,
         * nothing is left open.
         * @note @ref backend_kind::offline is not a valid backend here. A stream the caller
         * advances by hand is @ref offline_stream, which has a different set of operations because
         * it is a different thing; asking for it here fails with
         * @ref error_code::backend_unavailable.
         */
        [[nodiscard]] static std::expected<stream, error> open(const stream_config &config, renderer render);

        /** @brief Stops the stream, joins the render thread and closes the device. */
        ~stream();

        stream(stream &&) noexcept;
        stream &operator=(stream &&) noexcept;

        stream(const stream &) = delete;
        stream &operator=(const stream &) = delete;

        /**
         * @brief Begins delivering blocks to the renderer.
         * @return Nothing, or @ref error_code::thread_failure if the render thread could not start.
         * @note Idempotent while running.
         */
        std::expected<void, error> start();

        /**
         * @brief Stops delivering blocks and joins the render thread.
         * @details Safe to call when already stopped, and safe to call from a bus listener inside
         * @ref pump - which is where a program handling @ref device_lost_event will call it.
         */
        void stop() noexcept;

        /** @brief True between a successful @ref start and a @ref stop, or until the device is
         * lost. */
        [[nodiscard]] bool is_running() const noexcept;

        /**
         * @brief Publishes everything the stream has observed since the last call, onto the bus
         * given in @ref stream_config::bus.
         * @details Call it once a frame. Does nothing at all when no bus was given, so a program
         * that does not want events pays nothing for them. See events.hpp for what arrives and why
         * it arrives here rather than on the thread that noticed.
         */
        void pump();

        /** @brief What was negotiated. Fixed for the life of the stream. */
        [[nodiscard]] const stream_info &info() const noexcept;

        /** @brief A snapshot of the health counters. */
        [[nodiscard]] stream_stats stats() const noexcept;

        /** @brief Zeroes the health counters. Safe while running. */
        void reset_stats() noexcept;

        /** @brief The backend driving this stream. Shorthand for `info().backend`. */
        [[nodiscard]] backend_kind backend() const noexcept;

        /** @brief The backend's display name, e.g. "WASAPI". Shorthand for `name(backend())`. */
        [[nodiscard]] std::string_view backend_name() const noexcept;

    private:
        struct impl;
        explicit stream(std::unique_ptr<impl> state) noexcept;

        std::unique_ptr<impl> impl_;
    };

} // namespace catalyst::audio
