/**
 * @file offline.hpp
 * @brief A stream with no hardware, no threads, and a clock the caller turns by hand.
 * @details This is how the audio path is tested. Ask for exactly 48000 frames and you get exactly
 * 48000 frames, on the calling thread, with no device to renegotiate the format and no scheduler to
 * decide when. The same @ref renderer runs here and on a real device, so what CI checks is the code
 * that will actually play.
 *
 * It is a separate type from @ref stream, rather than a backend of it, because the two do not have
 * the same operations. The old surface put `render()` and `captured_output()` on the engine and had
 * them fail with `unsupported_operation` on the four backends out of six that own their own clock -
 * so the type's signature promised something four-sixths of its instances could not do. Splitting
 * them deleted that error code's largest use, and each type is now honest about what it is: a
 * @ref stream is clocked by a device and has @ref stream::start and @ref stream::stop; an
 * @ref offline_stream is clocked by @ref offline_stream::render and has neither, because "started"
 * meant nothing for a thing that only ever moves when told to.
 *
 * Capture is likewise explicit. Where the old backend wrote a WAV during `shutdown()` if a path had
 * been set and retention happened to be on - best effort, no way to report failure, in a `noexcept`
 * teardown - @ref offline_stream::write_wav is a call that says what it did.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/audio/block.hpp>
#include <catalyst/audio/error.hpp>
#include <catalyst/audio/stream.hpp>
#include <catalyst/audio/types.hpp>

#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <span>

namespace catalyst::audio
{

    /**
     * @struct offline_config
     * @brief What to render, and what to keep.
     * @details Unlike @ref stream_config every field here is honoured exactly. There is no device
     * to negotiate with, and reproducibility is the point of the type - so there is no
     * `allow_format_fallback`, and @ref offline_stream::info always matches what was asked for.
     */
    struct offline_config
    {
        /** @brief Which buffers the renderer is given. Duplex works here, unlike on WASAPI: with
         * one clock and no drift there is nothing to compensate for. */
        stream_direction direction = stream_direction::output;

        /** @brief Frames per second. Honoured exactly. */
        sample_rate_t sample_rate = 48000;

        /** @brief Channels to render. */
        channel_count output_channels = 2;

        /** @brief Channels to capture from @ref input. */
        channel_count input_channels = 0;

        /**
         * @brief Frames per call to the renderer.
         * @details A request for more frames than this is split into blocks of this size, so a
         * renderer sees the same block sizes it would from a device - which is what makes a test
         * that passes offline mean something about the live path.
         */
        std::uint32_t block_frames = 512;

        /** @brief Retain rendered output in memory, readable through
         * @ref offline_stream::captured. */
        bool capture = true;

        /**
         * @brief Cap on retained frames; zero means unbounded.
         * @details Rendering continues past the cap - the stream is still exercised, and stats
         * still count - but the excess is not stored. This is what keeps a long soak test from
         * being a memory test.
         */
        frame_count max_capture_frames = 0;

        /**
         * @brief Interleaved capture data fed to @ref render_block::input.
         * @details Caller-owned and must outlive the stream. Reads past the end are zero-filled, so
         * a renderer always sees a full block. Ignored unless @ref direction has input.
         */
        std::span<const sample> input;
    };

    /**
     * @class offline_stream
     * @brief A deterministic renderer: it advances only when @ref render is called, and by exactly
     * as much as it is asked to.
     * @details Single-threaded. Everything happens on the thread that calls @ref render, including
     * the renderer itself - so a test may assert on state the renderer touched immediately
     * afterwards, with no synchronisation and no waiting.
     */
    class offline_stream
    {
    public:
        /**
         * @brief Opens a deterministic stream.
         * @param config What to render. See @ref offline_config.
         * @param render The callable that fills each block. It must outlive the stream. The
         * real-time rules in block.hpp do not bind here - there is no deadline to miss - but a
         * renderer written to them is the one worth testing.
         * @return An open stream, ready to @ref render. Fails only with
         * @ref error_code::invalid_config.
         */
        [[nodiscard]] static std::expected<offline_stream, error> open(
            const offline_config &config,
            renderer render);

        ~offline_stream();

        offline_stream(offline_stream &&) noexcept;
        offline_stream &operator=(offline_stream &&) noexcept;

        offline_stream(const offline_stream &) = delete;
        offline_stream &operator=(const offline_stream &) = delete;

        /**
         * @brief Renders exactly @p frames frames, calling the renderer once per block.
         * @param frames How far to advance. Zero is a no-op.
         * @return @p frames. Returned rather than assumed so a loop can be written the same way it
         * would be against a source that might fall short.
         * @note No `std::expected`: there is nothing here that can fail. The stream has no device
         * to lose, no thread to fail to start and no format to renegotiate.
         */
        frame_count render(frame_count frames);

        /**
         * @brief The interleaved output retained so far.
         * @return Empty when @ref offline_config::capture is false or the stream has no output.
         * @warning Invalidated by the next @ref render, which may reallocate. Copy what you need,
         * or read it between calls.
         */
        [[nodiscard]] std::span<const sample> captured() const noexcept;

        /** @brief How many frames of output are retained. Divides @ref captured by the channel
         * count. */
        [[nodiscard]] frame_count captured_frames() const noexcept;

        /** @brief Discards retained output, keeping the stream open and its position unchanged. */
        void clear_captured() noexcept;

        /**
         * @brief Writes the retained output as a 32-bit float WAV.
         * @param path Where to write it. Parent directories are not created.
         * @return Nothing, or @ref error_code::io_failure if the file could not be written, or
         * @ref error_code::invalid_config if nothing was retained to write.
         * @details Emits the 18-byte `fmt ` chunk and the `fact` chunk that
         * `WAVE_FORMAT_IEEE_FLOAT` requires, rather than the abbreviated PCM header, so strict
         * readers accept the file.
         */
        std::expected<void, error> write_wav(const std::filesystem::path &path) const;

        /** @brief The format, which is exactly what was asked for. */
        [[nodiscard]] const stream_info &info() const noexcept;

        /** @brief A snapshot of the counters. @ref stream_stats::xruns is always zero here: there
         * is no deadline to miss. */
        [[nodiscard]] stream_stats stats() const noexcept;

        /** @brief Zeroes the counters. Does not rewind @ref render_block::position. */
        void reset_stats() noexcept;

        /** @brief Frames rendered since the stream opened. Matches the next block's
         * @ref render_block::position. */
        [[nodiscard]] frame_count position() const noexcept;

    private:
        struct impl;
        explicit offline_stream(std::unique_ptr<impl> state) noexcept;

        std::unique_ptr<impl> impl_;
    };

} // namespace catalyst::audio
