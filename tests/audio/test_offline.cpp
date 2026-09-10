/**
 * @file test_offline.cpp
 * @brief Exercises `offline_stream`, which renders on demand with no hardware and no threads.
 * @details Because it is fully deterministic, these tests assert on exact sample values rather than
 * on statistical properties: block segmentation, stream-position continuity across blocks,
 * interleaving, capture limits, the duplex input feed and the WAV writer are all verified
 * sample-for-sample.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "../test_common.hpp"

#include <catalyst/audio/offline.hpp>

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
    /// exactly `i`. Any error in block segmentation, position bookkeeping or interleaving shows up
    /// as a mismatch at a specific index.
    void render_ramp(render_block &block) noexcept
    {
        const frame_count base = block.position * block.output_channels;

        for (std::uint32_t frame = 0; frame < block.frames; ++frame)
        {
            for (channel_count channel = 0; channel < block.output_channels; ++channel)
            {
                const std::size_t index = static_cast<std::size_t>(frame) * block.output_channels + channel;
                block.output[index] = static_cast<sample>(base + index);
            }
        }
    }

    /// Copies capture straight to output so the offline input feed can be verified.
    void render_passthrough(render_block &block) noexcept
    {
        for (std::uint32_t frame = 0; frame < block.frames; ++frame)
        {
            const auto in = block.input_frame(frame);
            const auto out = block.output_frame(frame);

            for (channel_count channel = 0; channel < block.output_channels; ++channel)
                out[channel] = channel < in.size() ? in[channel] : sample{0};
        }
    }

    struct block_log
    {
        std::vector<std::uint32_t> sizes;
        std::vector<frame_count> positions;
        sample_rate_t sample_rate = 0;
        channel_count output_channels = 0;

        void operator()(render_block &block) noexcept
        {
            sizes.push_back(block.frames);
            positions.push_back(block.position);
            sample_rate = block.sample_rate;
            output_channels = block.output_channels;
            block.silence();
        }
    };

    offline_config config_for(channel_count channels, std::uint32_t block_frames)
    {
        offline_config config;
        config.sample_rate = 48000;
        config.output_channels = channels;
        config.block_frames = block_frames;
        return config;
    }

    std::uint32_t read_u32_le(const unsigned char *bytes)
    {
        return static_cast<std::uint32_t>(bytes[0]) |
               (static_cast<std::uint32_t>(bytes[1]) << 8) |
               (static_cast<std::uint32_t>(bytes[2]) << 16) |
               (static_cast<std::uint32_t>(bytes[3]) << 24);
    }

    std::uint16_t read_u16_le(const unsigned char *bytes)
    {
        return static_cast<std::uint16_t>(
            static_cast<std::uint16_t>(bytes[0]) | (static_cast<std::uint16_t>(bytes[1]) << 8));
    }

    // -----------------------------------------------------------------------------------------

    /// An open offline stream renders immediately: there is no clock to start, because the caller
    /// is the clock.
    void test_render_is_sample_exact()
    {
        auto opened = offline_stream::open(config_for(2, 64), &render_ramp);
        CT_REQUIRE(opened.has_value());

        CT_REQUIRE(opened->render(500) == 500);
        CT_REQUIRE(opened->position() == 500);

        const auto captured = opened->captured();
        CT_REQUIRE(captured.size() == 500u * 2u);
        CT_REQUIRE(opened->captured_frames() == 500);

        for (std::size_t i = 0; i < captured.size(); ++i)
            CT_REQUIRE(captured[i] == static_cast<sample>(i));
    }

    void test_config_validation()
    {
        const auto rejects = [](auto mutate) {
            offline_config config = config_for(2, 64);
            mutate(config);
            const auto result = offline_stream::open(config, &render_ramp);
            CT_REQUIRE(!result.has_value());
            CT_REQUIRE(result.error().code == error_code::invalid_config);
            CT_REQUIRE(result.error().backend == backend_kind::offline);
        };

        rejects([](offline_config &c) { c.sample_rate = 0; });
        rejects([](offline_config &c) { c.output_channels = 0; });
        rejects([](offline_config &c) {
            c.direction = stream_direction::input;
            c.input_channels = 0;
        });
    }

    /// A render is split into whole blocks with a short final block, and the position advances by
    /// exactly the frames delivered.
    void test_block_segmentation_and_position()
    {
        block_log log;

        auto opened = offline_stream::open(config_for(2, 64), log);
        CT_REQUIRE(opened.has_value());
        CT_REQUIRE(opened->render(200) == 200);

        // 200 frames at 64 per block => 64, 64, 64, 8.
        CT_REQUIRE(log.sizes.size() == 4);
        CT_REQUIRE(log.sizes[0] == 64);
        CT_REQUIRE(log.sizes[1] == 64);
        CT_REQUIRE(log.sizes[2] == 64);
        CT_REQUIRE(log.sizes[3] == 8);

        CT_REQUIRE(log.positions.size() == 4);
        CT_REQUIRE(log.positions[0] == 0);
        CT_REQUIRE(log.positions[1] == 64);
        CT_REQUIRE(log.positions[2] == 128);
        CT_REQUIRE(log.positions[3] == 192);

        CT_REQUIRE(log.sample_rate == 48000);
        CT_REQUIRE(log.output_channels == 2);

        // A second render continues the same timeline rather than restarting it.
        CT_REQUIRE(opened->render(64) == 64);
        CT_REQUIRE(log.positions.size() == 5);
        CT_REQUIRE(log.positions[4] == 200);
    }

    /// A new stream starts a new timeline, and the old one is unaffected by it.
    void test_each_stream_has_its_own_clock()
    {
        block_log first_log;
        block_log second_log;

        auto first = offline_stream::open(config_for(1, 32), first_log);
        CT_REQUIRE(first.has_value());
        CT_REQUIRE(first->render(96) == 96);
        CT_REQUIRE(first_log.positions.back() == 64);

        auto second = offline_stream::open(config_for(1, 32), second_log);
        CT_REQUIRE(second.has_value());
        CT_REQUIRE(second->render(32) == 32);
        CT_REQUIRE(second_log.positions.size() == 1);
        CT_REQUIRE(second_log.positions[0] == 0);

        CT_REQUIRE(first->position() == 96);
        CT_REQUIRE(second->position() == 32);
    }

    /// With no renderer the stream must produce silence, not leave the buffer undefined.
    void test_empty_renderer_renders_silence()
    {
        auto opened = offline_stream::open(config_for(2, 64), renderer{});
        CT_REQUIRE(opened.has_value());
        CT_REQUIRE(opened->render(128) == 128);

        const auto captured = opened->captured();
        CT_REQUIRE(captured.size() == 128u * 2u);

        for (const sample value : captured)
            CT_REQUIRE(value == 0.0f);
    }

    void test_capture_can_be_capped_disabled_and_cleared()
    {
        {
            auto config = config_for(2, 64);
            config.max_capture_frames = 100;

            auto opened = offline_stream::open(config, &render_ramp);
            CT_REQUIRE(opened.has_value());
            CT_REQUIRE(opened->render(500) == 500);

            // Rendering continues past the cap; only retention is limited.
            CT_REQUIRE(opened->captured().size() == 100u * 2u);
            CT_REQUIRE(opened->stats().frames_rendered == 500);
            CT_REQUIRE(opened->position() == 500);
        }

        {
            auto config = config_for(2, 64);
            config.capture = false;

            auto opened = offline_stream::open(config, &render_ramp);
            CT_REQUIRE(opened.has_value());
            CT_REQUIRE(opened->render(128) == 128);
            CT_REQUIRE(opened->captured().empty());
        }

        {
            auto opened = offline_stream::open(config_for(2, 64), &render_ramp);
            CT_REQUIRE(opened.has_value());
            CT_REQUIRE(opened->render(128) == 128);
            CT_REQUIRE(!opened->captured().empty());

            // Clearing the store does not rewind the stream.
            opened->clear_captured();
            CT_REQUIRE(opened->captured().empty());
            CT_REQUIRE(opened->position() == 128);
        }
    }

    /// Duplex: supplied capture data reaches the renderer, and reads past its end are zero-filled.
    void test_input_feed()
    {
        constexpr channel_count channels = 2;
        constexpr frame_count source_frames = 100;

        std::vector<sample> source(source_frames * channels);
        for (std::size_t i = 0; i < source.size(); ++i)
            source[i] = static_cast<sample>(i + 1);

        auto config = config_for(channels, 64);
        config.direction = stream_direction::duplex;
        config.input_channels = channels;
        config.input = source;

        auto opened = offline_stream::open(config, &render_passthrough);
        CT_REQUIRE(opened.has_value());
        CT_REQUIRE(opened->render(150) == 150);

        const auto captured = opened->captured();
        CT_REQUIRE(captured.size() == 150u * channels);

        for (std::size_t i = 0; i < source.size(); ++i)
            CT_REQUIRE(captured[i] == source[i]);

        // Beyond the supplied data the input reads as silence.
        for (std::size_t i = source.size(); i < captured.size(); ++i)
            CT_REQUIRE(captured[i] == 0.0f);

        const stream_info &info = opened->info();
        CT_REQUIRE(info.direction == stream_direction::duplex);
        CT_REQUIRE(info.input_channels == channels);
        CT_REQUIRE(info.output_channels == channels);
    }

    /// The configuration is honoured exactly. Nothing here negotiates, which is the whole point.
    void test_format_is_honoured_exactly()
    {
        auto config = config_for(1, 256);
        config.sample_rate = 32000;

        auto opened = offline_stream::open(config, &render_ramp);
        CT_REQUIRE(opened.has_value());

        const stream_info &info = opened->info();
        CT_REQUIRE(info.backend == backend_kind::offline);
        CT_REQUIRE(info.direction == stream_direction::output);
        CT_REQUIRE(info.sample_rate == 32000);
        CT_REQUIRE(info.output_channels == 1);
        CT_REQUIRE(info.input_channels == 0);
        CT_REQUIRE(info.output_layout == channel_layout::mono);
        CT_REQUIRE(info.block_frames == 256);
        CT_REQUIRE(!info.exclusive);
        CT_REQUIRE(info.device_id == "offline");

        // 256 frames at 32 kHz is exactly 8 ms.
        CT_REQUIRE(info.output_latency.count() > 0.0079);
        CT_REQUIRE(info.output_latency.count() < 0.0081);
    }

    /// Two independent runs of the same configuration must produce identical output. This is the
    /// property the whole offline renderer exists to provide.
    void test_runs_are_reproducible()
    {
        const auto run = []() {
            auto opened = offline_stream::open(config_for(2, 48), &render_ramp);
            CT_REQUIRE(opened.has_value());
            CT_REQUIRE(opened->render(321) == 321);

            const auto captured = opened->captured();
            return std::vector<sample>(captured.begin(), captured.end());
        };

        const auto first = run();
        const auto second = run();

        CT_REQUIRE(!first.empty());
        CT_REQUIRE(first.size() == second.size());
        CT_REQUIRE(std::memcmp(first.data(), second.data(), first.size() * sizeof(sample)) == 0);
    }

    void test_stats_track_rendering()
    {
        auto opened = offline_stream::open(config_for(2, 64), &render_ramp);
        CT_REQUIRE(opened.has_value());
        CT_REQUIRE(opened->render(256) == 256);

        auto stats = opened->stats();
        CT_REQUIRE(stats.frames_rendered == 256);
        CT_REQUIRE(stats.blocks == 4);

        // There is no deadline to miss when the caller owns the clock.
        CT_REQUIRE(stats.xruns == 0);

        opened->reset_stats();
        stats = opened->stats();
        CT_REQUIRE(stats.frames_rendered == 0);
        CT_REQUIRE(stats.blocks == 0);

        // Resetting the counters does not rewind the stream.
        CT_REQUIRE(opened->position() == 256);
    }

    /// The WAV must be a well-formed IEEE-float file whose payload matches what was rendered.
    void test_wav_output()
    {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "catalyst_audio_offline_test.wav";

        std::error_code ignored;
        std::filesystem::remove(path, ignored);

        constexpr channel_count channels = 2;
        constexpr frame_count frames = 128;

        {
            auto opened = offline_stream::open(config_for(channels, 64), &render_ramp);
            CT_REQUIRE(opened.has_value());
            CT_REQUIRE(opened->render(frames) == frames);
            CT_REQUIRE(opened->write_wav(path).has_value());
        }

        std::ifstream file(path, std::ios::binary);
        CT_REQUIRE(static_cast<bool>(file));

        std::vector<unsigned char> bytes(
            (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());

        constexpr std::size_t header_size = 58;
        const std::size_t data_bytes = frames * channels * sizeof(sample);
        CT_REQUIRE(bytes.size() == header_size + data_bytes);

        CT_REQUIRE(std::memcmp(bytes.data() + 0, "RIFF", 4) == 0);
        CT_REQUIRE(read_u32_le(bytes.data() + 4) == bytes.size() - 8);
        CT_REQUIRE(std::memcmp(bytes.data() + 8, "WAVE", 4) == 0);

        CT_REQUIRE(std::memcmp(bytes.data() + 12, "fmt ", 4) == 0);
        CT_REQUIRE(read_u32_le(bytes.data() + 16) == 18);
        CT_REQUIRE(read_u16_le(bytes.data() + 20) == 3); // WAVE_FORMAT_IEEE_FLOAT
        CT_REQUIRE(read_u16_le(bytes.data() + 22) == channels);
        CT_REQUIRE(read_u32_le(bytes.data() + 24) == 48000);
        CT_REQUIRE(read_u32_le(bytes.data() + 28) == 48000 * channels * sizeof(sample));
        CT_REQUIRE(read_u16_le(bytes.data() + 32) == channels * sizeof(sample));
        CT_REQUIRE(read_u16_le(bytes.data() + 34) == 32);

        CT_REQUIRE(std::memcmp(bytes.data() + 38, "fact", 4) == 0);
        CT_REQUIRE(read_u32_le(bytes.data() + 46) == frames);

        CT_REQUIRE(std::memcmp(bytes.data() + 50, "data", 4) == 0);
        CT_REQUIRE(read_u32_le(bytes.data() + 54) == data_bytes);

        // The payload is the same ramp the renderer produced.
        for (std::size_t i = 0; i < frames * channels; ++i)
        {
            sample value = 0.0f;
            std::memcpy(&value, bytes.data() + header_size + i * sizeof(sample), sizeof(sample));
            CT_REQUIRE(value == static_cast<sample>(i));
        }

        std::filesystem::remove(path, ignored);
    }

    /// Writing with nothing retained says so rather than producing an empty file the caller would
    /// only notice was empty on playing it.
    void test_wav_without_capture_is_refused()
    {
        const std::filesystem::path path =
            std::filesystem::temp_directory_path() / "catalyst_audio_should_not_exist.wav";

        std::error_code ignored;
        std::filesystem::remove(path, ignored);

        auto config = config_for(2, 64);
        config.capture = false;

        auto opened = offline_stream::open(config, &render_ramp);
        CT_REQUIRE(opened.has_value());
        CT_REQUIRE(opened->render(128) == 128);

        const auto written = opened->write_wav(path);
        CT_REQUIRE(!written.has_value());
        CT_REQUIRE(written.error().code == error_code::invalid_config);
        CT_REQUIRE(!std::filesystem::exists(path));
    }

} // namespace

int main()
{
    test_render_is_sample_exact();
    test_config_validation();
    test_block_segmentation_and_position();
    test_each_stream_has_its_own_clock();
    test_empty_renderer_renders_silence();
    test_capture_can_be_capped_disabled_and_cleared();
    test_input_feed();
    test_format_is_honoured_exactly();
    test_runs_are_reproducible();
    test_stats_track_rendering();
    test_wav_output();
    test_wav_without_capture_is_refused();

    return 0;
}
