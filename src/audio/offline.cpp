/**
 * @file offline.cpp
 * @brief The deterministic renderer, and the float WAV writer it hands captured output to.
 * @details No device, no thread, no negotiation: the configuration is honoured exactly and the
 * stream advances only when `render()` is called. Written directly against the public surface
 * rather than behind the backend interface, because it has no device to abstract - which is also
 * why the type stopped being a backend when the surface was rewritten.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "detail_render.hpp"

#include <catalyst/audio/offline.hpp>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <utility>
#include <vector>

namespace catalyst::audio
{

    namespace
    {

        void put_u32_le(std::vector<std::uint8_t> &out, std::uint32_t value)
        {
            out.push_back(static_cast<std::uint8_t>(value & 0xFF));
            out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
            out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
            out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
        }

        void put_u16_le(std::vector<std::uint8_t> &out, std::uint16_t value)
        {
            out.push_back(static_cast<std::uint8_t>(value & 0xFF));
            out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
        }

        void put_tag(std::vector<std::uint8_t> &out, const char (&tag)[5])
        {
            out.insert(out.end(), tag, tag + 4);
        }

        /**
         * @brief Writes interleaved float32 samples as a `WAVE_FORMAT_IEEE_FLOAT` `.wav`.
         * @details Emits the 18-byte `fmt ` chunk and the `fact` chunk that the format tag requires,
         * rather than the abbreviated 16-byte PCM header, so strict readers accept the file.
         */
        bool write_float_wav(
            const std::filesystem::path &path,
            std::span<const sample> interleaved,
            sample_rate_t rate,
            channel_count channels)
        {
            if (channels == 0 || rate == 0)
                return false;

            const std::uint64_t data_bytes = static_cast<std::uint64_t>(interleaved.size()) * sizeof(sample);
            const std::uint64_t frames = interleaved.size() / channels;

            // RIFF sizes are 32-bit; refuse rather than silently truncate.
            if (data_bytes + 64 > 0xFFFFFFFFull)
                return false;

            const std::uint32_t block_align = static_cast<std::uint32_t>(channels * sizeof(sample));

            std::vector<std::uint8_t> header;
            header.reserve(64);

            put_tag(header, "RIFF");
            // "WAVE" + fmt chunk (8 + 18) + fact chunk (8 + 4) + data header (8) = 50
            put_u32_le(header, static_cast<std::uint32_t>(50 + data_bytes));
            put_tag(header, "WAVE");

            put_tag(header, "fmt ");
            put_u32_le(header, 18);
            put_u16_le(header, 3); // WAVE_FORMAT_IEEE_FLOAT
            put_u16_le(header, static_cast<std::uint16_t>(channels));
            put_u32_le(header, rate);
            put_u32_le(header, rate * block_align); // byte rate
            put_u16_le(header, static_cast<std::uint16_t>(block_align));
            put_u16_le(header, 32); // bits per sample
            put_u16_le(header, 0);  // cbSize

            put_tag(header, "fact");
            put_u32_le(header, 4);
            put_u32_le(header, static_cast<std::uint32_t>(frames));

            put_tag(header, "data");
            put_u32_le(header, static_cast<std::uint32_t>(data_bytes));

            std::ofstream file(path, std::ios::binary | std::ios::trunc);
            if (!file)
                return false;

            file.write(reinterpret_cast<const char *>(header.data()), static_cast<std::streamsize>(header.size()));

            if (!interleaved.empty())
            {
                file.write(
                    reinterpret_cast<const char *>(interleaved.data()),
                    static_cast<std::streamsize>(data_bytes));
            }

            file.flush();
            return static_cast<bool>(file);
        }

    } // namespace

    /**
     * @struct offline_stream::impl
     * @brief The scratch buffers, the capture store and the dispatcher.
     */
    struct offline_stream::impl
    {
        offline_config config{};
        stream_info info{};

        detail::stats_block stats;
        detail::render_dispatcher dispatcher;

        std::vector<sample> output_scratch;
        std::vector<sample> input_scratch;
        std::vector<sample> captured;
        frame_count input_cursor = 0;

        explicit impl(renderer render) : dispatcher(render, stats) {}
    };

    std::expected<offline_stream, error> offline_stream::open(const offline_config &config, renderer render)
    {
        const auto invalid = [] { return std::unexpected(make_error(error_code::invalid_config, backend_kind::offline)); };

        if (config.sample_rate == 0)
            return invalid();

        const channel_count output_channels = has_output(config.direction) ? config.output_channels : 0;
        const channel_count input_channels = has_input(config.direction) ? config.input_channels : 0;

        if (output_channels == 0 && input_channels == 0)
            return invalid();

        if (has_output(config.direction) && config.output_channels == 0)
            return invalid();

        if (has_input(config.direction) && config.input_channels == 0)
            return invalid();

        auto state = std::make_unique<impl>(render);
        state->config = config;

        const std::uint32_t block_frames = config.block_frames != 0 ? config.block_frames : 512;

        // The offline renderer never renegotiates: reproducibility is the entire point of it, so
        // `info()` is the configuration rather than a compromise with a device.
        state->info.backend = backend_kind::offline;
        state->info.direction = config.direction;
        state->info.sample_rate = config.sample_rate;
        state->info.output_channels = output_channels;
        state->info.input_channels = input_channels;
        state->info.output_layout = layout_for(output_channels);
        state->info.block_frames = block_frames;
        state->info.output_latency = output_channels ? frames_to_time(block_frames, config.sample_rate) : seconds{0.0};
        state->info.input_latency = input_channels ? frames_to_time(block_frames, config.sample_rate) : seconds{0.0};
        state->info.exclusive = false;
        state->info.device_id = "offline";
        state->info.device_name = "Offline Renderer";

        if (output_channels > 0)
            state->output_scratch.assign(static_cast<std::size_t>(block_frames) * output_channels, 0.0f);

        if (input_channels > 0)
            state->input_scratch.assign(static_cast<std::size_t>(block_frames) * input_channels, 0.0f);

        if (config.capture && output_channels > 0 && config.max_capture_frames != 0)
        {
            state->captured.reserve(
                static_cast<std::size_t>(config.max_capture_frames * output_channels));
        }

        return offline_stream{std::move(state)};
    }

    offline_stream::offline_stream(std::unique_ptr<impl> state) noexcept : impl_(std::move(state)) {}

    offline_stream::offline_stream(offline_stream &&) noexcept = default;
    offline_stream &offline_stream::operator=(offline_stream &&) noexcept = default;
    offline_stream::~offline_stream() = default;

    frame_count offline_stream::render(frame_count frames)
    {
        if (!impl_ || frames == 0)
            return 0;

        impl &state = *impl_;

        const std::uint32_t block_frames = state.info.block_frames;
        const channel_count output_channels = state.info.output_channels;
        const channel_count input_channels = state.info.input_channels;

        frame_count remaining = frames;

        while (remaining > 0)
        {
            const std::uint32_t block = static_cast<std::uint32_t>(std::min<frame_count>(remaining, block_frames));

            std::span<const sample> input;
            if (input_channels > 0)
            {
                const std::size_t needed = static_cast<std::size_t>(block) * input_channels;
                std::fill_n(state.input_scratch.begin(), needed, 0.0f);

                // Zero-fill past the end of the supplied capture data, so the renderer always sees
                // a full block rather than a short one it would have to special-case.
                const frame_count supplied = input_channels != 0
                                                 ? state.config.input.size() / input_channels
                                                 : 0;

                if (state.input_cursor < supplied)
                {
                    const frame_count available = supplied - state.input_cursor;
                    const frame_count copy_frames = std::min<frame_count>(available, block);
                    const std::size_t copy_samples = static_cast<std::size_t>(copy_frames) * input_channels;
                    const auto first = state.config.input.begin() +
                                       static_cast<std::ptrdiff_t>(state.input_cursor * input_channels);

                    std::copy_n(first, copy_samples, state.input_scratch.begin());
                }

                state.input_cursor += block;
                input = std::span<const sample>(state.input_scratch.data(), needed);
            }

            std::span<sample> output;
            if (output_channels > 0)
            {
                output = std::span<sample>(
                    state.output_scratch.data(), static_cast<std::size_t>(block) * output_channels);
            }

            state.dispatcher.dispatch(
                output, input, block, output_channels, input_channels, state.info.sample_rate);

            if (!output.empty() && state.config.capture)
            {
                frame_count storable = block;

                if (state.config.max_capture_frames != 0)
                {
                    const frame_count held = state.captured.size() / output_channels;
                    storable = held >= state.config.max_capture_frames
                                   ? 0
                                   : std::min<frame_count>(block, state.config.max_capture_frames - held);
                }

                if (storable != 0)
                {
                    const std::size_t samples = static_cast<std::size_t>(storable) * output_channels;
                    state.captured.insert(
                        state.captured.end(), output.begin(), output.begin() + static_cast<std::ptrdiff_t>(samples));
                }
            }

            remaining -= block;
        }

        return frames;
    }

    std::span<const sample> offline_stream::captured() const noexcept
    {
        if (!impl_)
            return {};

        return std::span<const sample>(impl_->captured);
    }

    frame_count offline_stream::captured_frames() const noexcept
    {
        if (!impl_ || impl_->info.output_channels == 0)
            return 0;

        return impl_->captured.size() / impl_->info.output_channels;
    }

    void offline_stream::clear_captured() noexcept
    {
        if (impl_)
            impl_->captured.clear();
    }

    std::expected<void, error> offline_stream::write_wav(const std::filesystem::path &path) const
    {
        if (!impl_)
            return std::unexpected(make_error(error_code::invalid_config, backend_kind::offline));

        if (impl_->captured.empty())
        {
            // Nothing was retained, so there is no file to write. Saying so is better than writing
            // a valid, empty WAV that the caller will not notice is empty until they play it.
            return std::unexpected(make_error(error_code::invalid_config, backend_kind::offline));
        }

        const bool written = write_float_wav(
            path,
            std::span<const sample>(impl_->captured),
            impl_->info.sample_rate,
            impl_->info.output_channels);

        if (!written)
            return std::unexpected(make_error(error_code::io_failure, backend_kind::offline));

        return {};
    }

    const stream_info &offline_stream::info() const noexcept
    {
        static const stream_info empty{};
        return impl_ ? impl_->info : empty;
    }

    stream_stats offline_stream::stats() const noexcept
    {
        if (!impl_)
            return {};

        return impl_->stats.snapshot();
    }

    void offline_stream::reset_stats() noexcept
    {
        if (impl_)
            impl_->stats.reset();
    }

    frame_count offline_stream::position() const noexcept
    {
        if (!impl_)
            return 0;

        return impl_->dispatcher.position();
    }

} // namespace catalyst::audio
