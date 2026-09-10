/**
 * @file test_offline.cpp
 * @brief Exercises the offline backend, which renders on demand with no hardware and no threads.
 * Because it is fully deterministic, these tests assert on exact sample values rather than on
 * statistical properties: block segmentation, stream-time continuity across blocks, interleaving,
 * capture limits, duplex input feed and the WAV writer are all verified sample-for-sample.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "../test_common.hpp"

#include <catalyst/audio/engine.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <vector>

using namespace catalyst;
using namespace catalyst::audio;

namespace
{

    /// Writes a globally unique ramp: the sample at interleaved index `i` of the whole stream is
    /// exactly `i`. Any error in block segmentation, stream-time bookkeeping or interleaving shows
    /// up as a mismatch at a specific index.
    void render_ramp(render_context &context) noexcept
    {
        const uint64_t base = context.stream_time_frames * context.output_channels;

        for (uint32_t frame = 0; frame < context.frames; ++frame)
        {
            for (uint32_t channel = 0; channel < context.output_channels; ++channel)
            {
                const size_t index = static_cast<size_t>(frame) * context.output_channels + channel;
                context.output[index] = static_cast<float>(base + index);
            }
        }
    }

    struct block_log
    {
        std::vector<uint32_t> sizes;
        std::vector<uint64_t> times;
        uint32_t sample_rate = 0;
        uint32_t output_channels = 0;
    };

    void render_logging(render_context &context) noexcept
    {
        auto *log = static_cast<block_log *>(context.user);
        log->sizes.push_back(context.frames);
        log->times.push_back(context.stream_time_frames);
        log->sample_rate = context.sample_rate;
        log->output_channels = context.output_channels;

        for (uint32_t i = 0; i < context.frames * context.output_channels; ++i)
            context.output[i] = 0.0f;
    }

    /// Copies capture straight to output so the offline input feed can be verified.
    void render_passthrough(render_context &context) noexcept
    {
        for (uint32_t frame = 0; frame < context.frames; ++frame)
        {
            for (uint32_t channel = 0; channel < context.output_channels; ++channel)
            {
                const size_t out_index = static_cast<size_t>(frame) * context.output_channels + channel;

                float value = 0.0f;
                if (context.input && channel < context.input_channels)
                {
                    const size_t in_index =
                        static_cast<size_t>(frame) * context.input_channels + channel;
                    value = context.input[in_index];
                }

                context.output[out_index] = value;
            }
        }
    }

    engine_config offline_config(uint32_t channels, uint32_t frames_per_buffer)
    {
        engine_config config;
        config.preferred_backend = engine_backend::offline;
        config.sample_rate = 48000;
        config.output_channels = channels;
        config.frames_per_buffer = frames_per_buffer;
        return config;
    }

    uint32_t read_u32_le(const unsigned char *bytes)
    {
        return static_cast<uint32_t>(bytes[0]) |
               (static_cast<uint32_t>(bytes[1]) << 8) |
               (static_cast<uint32_t>(bytes[2]) << 16) |
               (static_cast<uint32_t>(bytes[3]) << 24);
    }

    uint16_t read_u16_le(const unsigned char *bytes)
    {
        return static_cast<uint16_t>(
            static_cast<uint16_t>(bytes[0]) | (static_cast<uint16_t>(bytes[1]) << 8));
    }

    // -----------------------------------------------------------------------------------------

    /// Every sample of the whole stream must equal its own interleaved index, across many blocks.
    void test_render_is_sample_exact()
    {
        auto config = offline_config(2, 64);
        config.callback = &render_ramp;

        engine audio;
        CT_REQUIRE(audio.initialize(config).has_value());
        CT_REQUIRE(audio.start().has_value());

        const auto rendered = audio.render(500);
        CT_REQUIRE(rendered.has_value());
        CT_REQUIRE(*rendered == 500);

        const auto captured = audio.captured_output();
        CT_REQUIRE(captured.size() == 500u * 2u);

        for (size_t i = 0; i < captured.size(); ++i)
            CT_REQUIRE(captured[i] == static_cast<float>(i));
    }

    /// A render is split into whole blocks with a short final block, and stream time advances by
    /// exactly the frames delivered.
    void test_block_segmentation_and_stream_time()
    {
        block_log log;

        auto config = offline_config(2, 64);
        config.callback = &render_logging;
        config.user = &log;

        engine audio;
        CT_REQUIRE(audio.initialize(config).has_value());
        CT_REQUIRE(audio.start().has_value());
        CT_REQUIRE(audio.render(200).has_value());

        // 200 frames at 64 per block => 64, 64, 64, 8.
        CT_REQUIRE(log.sizes.size() == 4);
        CT_REQUIRE(log.sizes[0] == 64);
        CT_REQUIRE(log.sizes[1] == 64);
        CT_REQUIRE(log.sizes[2] == 64);
        CT_REQUIRE(log.sizes[3] == 8);

        CT_REQUIRE(log.times.size() == 4);
        CT_REQUIRE(log.times[0] == 0);
        CT_REQUIRE(log.times[1] == 64);
        CT_REQUIRE(log.times[2] == 128);
        CT_REQUIRE(log.times[3] == 192);

        CT_REQUIRE(log.sample_rate == 48000);
        CT_REQUIRE(log.output_channels == 2);

        // A second render continues the same timeline rather than restarting it.
        CT_REQUIRE(audio.render(64).has_value());
        CT_REQUIRE(log.times.size() == 5);
        CT_REQUIRE(log.times[4] == 200);
    }

    /// Re-initializing restarts the stream clock.
    void test_stream_time_resets_on_reinitialize()
    {
        block_log log;

        auto config = offline_config(1, 32);
        config.callback = &render_logging;
        config.user = &log;

        engine audio;
        CT_REQUIRE(audio.initialize(config).has_value());
        CT_REQUIRE(audio.start().has_value());
        CT_REQUIRE(audio.render(96).has_value());
        CT_REQUIRE(log.times.back() == 64);

        audio.shutdown();
        log.times.clear();

        CT_REQUIRE(audio.initialize(config).has_value());
        CT_REQUIRE(audio.start().has_value());
        CT_REQUIRE(audio.render(32).has_value());
        CT_REQUIRE(log.times.size() == 1);
        CT_REQUIRE(log.times[0] == 0);
    }

    /// With no callback installed the engine must produce silence, not leave the buffer undefined.
    void test_absent_callback_renders_silence()
    {
        const auto config = offline_config(2, 64);

        engine audio;
        CT_REQUIRE(audio.initialize(config).has_value());
        CT_REQUIRE(audio.start().has_value());
        CT_REQUIRE(audio.render(128).has_value());

        const auto captured = audio.captured_output();
        CT_REQUIRE(captured.size() == 128u * 2u);

        for (const float sample : captured)
            CT_REQUIRE(sample == 0.0f);
    }

    void test_capture_can_be_capped_and_disabled()
    {
        {
            auto config = offline_config(2, 64);
            config.callback = &render_ramp;
            config.offline.max_capture_frames = 100;

            engine audio;
            CT_REQUIRE(audio.initialize(config).has_value());
            CT_REQUIRE(audio.start().has_value());
            CT_REQUIRE(audio.render(500).has_value());

            // Rendering continues past the cap; only retention is limited.
            CT_REQUIRE(audio.captured_output().size() == 100u * 2u);
            CT_REQUIRE(audio.stats().frames_rendered == 500);
        }

        {
            auto config = offline_config(2, 64);
            config.callback = &render_ramp;
            config.offline.capture_output = false;

            engine audio;
            CT_REQUIRE(audio.initialize(config).has_value());
            CT_REQUIRE(audio.start().has_value());
            CT_REQUIRE(audio.render(128).has_value());
            CT_REQUIRE(audio.captured_output().empty());
        }
    }

    /// Duplex: supplied capture data reaches the callback, and reads past its end are zero-filled.
    void test_offline_input_feed()
    {
        constexpr uint32_t channels = 2;
        constexpr uint64_t source_frames = 100;

        std::vector<float> source(source_frames * channels);
        for (size_t i = 0; i < source.size(); ++i)
            source[i] = static_cast<float>(i + 1);

        auto config = offline_config(channels, 64);
        config.direction = stream_direction::duplex;
        config.input_channels = channels;
        config.callback = &render_passthrough;
        config.offline.input_frames = source.data();
        config.offline.input_frame_count = source_frames;

        engine audio;
        CT_REQUIRE(audio.initialize(config).has_value());
        CT_REQUIRE(audio.start().has_value());
        CT_REQUIRE(audio.render(150).has_value());

        const auto captured = audio.captured_output();
        CT_REQUIRE(captured.size() == 150u * channels);

        for (size_t i = 0; i < source.size(); ++i)
            CT_REQUIRE(captured[i] == source[i]);

        // Beyond the supplied data the input reads as silence.
        for (size_t i = source.size(); i < captured.size(); ++i)
            CT_REQUIRE(captured[i] == 0.0f);

        const auto info = audio.info();
        CT_REQUIRE(info.direction == stream_direction::duplex);
        CT_REQUIRE(info.input_channels == channels);
        CT_REQUIRE(info.output_channels == channels);
    }

    /// Two independent runs of the same configuration must produce identical output. This is the
    /// property the whole offline backend exists to provide.
    void test_runs_are_reproducible()
    {
        const auto run = []() {
            auto config = offline_config(2, 48);
            config.callback = &render_ramp;

            engine audio;
            CT_REQUIRE(audio.initialize(config).has_value());
            CT_REQUIRE(audio.start().has_value());
            CT_REQUIRE(audio.render(321).has_value());

            const auto captured = audio.captured_output();
            return std::vector<float>(captured.begin(), captured.end());
        };

        const auto first = run();
        const auto second = run();

        CT_REQUIRE(!first.empty());
        CT_REQUIRE(first.size() == second.size());
        CT_REQUIRE(std::memcmp(first.data(), second.data(), first.size() * sizeof(float)) == 0);
    }

    void test_stats_track_rendering()
    {
        auto config = offline_config(2, 64);
        config.callback = &render_ramp;

        engine audio;
        CT_REQUIRE(audio.initialize(config).has_value());
        CT_REQUIRE(audio.start().has_value());
        CT_REQUIRE(audio.render(256).has_value());

        auto stats = audio.stats();
        CT_REQUIRE(stats.frames_rendered == 256);
        CT_REQUIRE(stats.callback_count == 4);
        CT_REQUIRE(stats.xruns == 0);

        audio.reset_stats();
        stats = audio.stats();
        CT_REQUIRE(stats.frames_rendered == 0);
        CT_REQUIRE(stats.callback_count == 0);
    }

    /// The WAV written on shutdown must be a well-formed IEEE-float file whose payload matches
    /// what was rendered.
    void test_wav_output()
    {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "catalyst_audio_offline_test.wav";

        std::error_code ignored;
        std::filesystem::remove(path, ignored);

        constexpr uint32_t channels = 2;
        constexpr uint64_t frames = 128;

        auto config = offline_config(channels, 64);
        config.callback = &render_ramp;
        config.offline.wav_path = path.string();

        {
            engine audio;
            CT_REQUIRE(audio.initialize(config).has_value());
            CT_REQUIRE(audio.start().has_value());
            CT_REQUIRE(audio.render(frames).has_value());
            audio.shutdown();
        }

        std::ifstream file(path, std::ios::binary);
        CT_REQUIRE(static_cast<bool>(file));

        std::vector<unsigned char> bytes(
            (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        constexpr size_t header_size = 58;
        const size_t data_bytes = frames * channels * sizeof(float);
        CT_REQUIRE(bytes.size() == header_size + data_bytes);

        CT_REQUIRE(std::memcmp(bytes.data() + 0, "RIFF", 4) == 0);
        CT_REQUIRE(read_u32_le(bytes.data() + 4) == bytes.size() - 8);
        CT_REQUIRE(std::memcmp(bytes.data() + 8, "WAVE", 4) == 0);

        CT_REQUIRE(std::memcmp(bytes.data() + 12, "fmt ", 4) == 0);
        CT_REQUIRE(read_u32_le(bytes.data() + 16) == 18);
        CT_REQUIRE(read_u16_le(bytes.data() + 20) == 3); // WAVE_FORMAT_IEEE_FLOAT
        CT_REQUIRE(read_u16_le(bytes.data() + 22) == channels);
        CT_REQUIRE(read_u32_le(bytes.data() + 24) == 48000);
        CT_REQUIRE(read_u32_le(bytes.data() + 28) == 48000 * channels * sizeof(float));
        CT_REQUIRE(read_u16_le(bytes.data() + 32) == channels * sizeof(float));
        CT_REQUIRE(read_u16_le(bytes.data() + 34) == 32);

        CT_REQUIRE(std::memcmp(bytes.data() + 38, "fact", 4) == 0);
        CT_REQUIRE(read_u32_le(bytes.data() + 46) == frames);

        CT_REQUIRE(std::memcmp(bytes.data() + 50, "data", 4) == 0);
        CT_REQUIRE(read_u32_le(bytes.data() + 54) == data_bytes);

        // The payload is the same ramp the callback produced.
        for (size_t i = 0; i < frames * channels; ++i)
        {
            float sample = 0.0f;
            std::memcpy(&sample, bytes.data() + header_size + i * sizeof(float), sizeof(float));
            CT_REQUIRE(sample == static_cast<float>(i));
        }

        std::filesystem::remove(path, ignored);
    }

} // namespace

int main()
{
    test_render_is_sample_exact();
    test_block_segmentation_and_stream_time();
    test_stream_time_resets_on_reinitialize();
    test_absent_callback_renders_silence();
    test_capture_can_be_capped_and_disabled();
    test_offline_input_feed();
    test_runs_are_reproducible();
    test_stats_track_rendering();
    test_wav_output();

    return 0;
}
