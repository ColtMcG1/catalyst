/**
 * @file main.cpp
 * @brief Plays a 440 Hz test tone through the Catalyst audio module.
 * @details Shows the shape of an audio program: enumerate what is there, open a stream against it,
 * hand it a renderer, start it, and pump a bus once a frame so device events arrive somewhere safe.
 *
 * Two details are worth copying rather than skimming. The tone is derived from
 * `render_block::position` and the *negotiated* sample rate rather than from a private counter, so
 * the pitch stays correct even when the device refuses the rate that was asked for. And nothing is
 * logged from inside the renderer: that runs on the device's real-time thread, where formatting a
 * message is exactly the kind of work that produces the xruns it would be reporting.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/catalyst.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <limits>
#include <thread>

namespace audio = catalyst::audio;
namespace events = catalyst::events;
namespace logging = catalyst::logging;

namespace
{

    /** @brief Names this example in the log's category column. */
    struct example_log
    {
        static constexpr const char *name = "audio_playback";
    };

    constexpr double two_pi = 6.2831853071795864769;

    /**
     * @struct tone
     * @brief The renderer's state. Owned by main, referred to by the renderer, read on the
     * real-time thread.
     */
    struct tone
    {
        double frequency_hz = 440.0;
        audio::frame_count fade_frames = 0;

        /// Written by main, read by the render thread; atomic for that reason and no other.
        std::atomic<audio::frame_count> stop_begin{std::numeric_limits<audio::frame_count>::max()};

        /** @brief The gain envelope at an absolute frame index: fade in, hold, fade out. */
        [[nodiscard]] float gain_at(audio::frame_count frame, audio::frame_count stop) const noexcept
        {
            constexpr float base_gain = 0.2f;

            if (fade_frames == 0)
                return frame >= stop ? 0.0f : base_gain;

            float gain = base_gain;

            if (frame < fade_frames)
                gain *= static_cast<float>(frame) / static_cast<float>(fade_frames);

            if (frame >= stop)
            {
                const double t = static_cast<double>(frame - stop) / static_cast<double>(fade_frames);
                gain *= static_cast<float>(t >= 1.0 ? 0.0 : 1.0 - t);
            }

            return gain;
        }
    };

} // namespace

int main()
{
    catalyst::catalyst_version_anchor();

    // One console sink, and every line below reaches the terminal, coloured when the terminal
    // understands colour. Sending the same log to a file is one more add_sink, and no change here.
    logging::default_logger().add_sink(logging::console_sink{});

    for (const auto backend : audio::available_backends())
        logging::info<example_log>("Available backend: {}", backend);

    if (const auto devices = audio::devices(); devices)
    {
        for (const auto &device : *devices)
        {
            logging::info<example_log>(
                " - Device: {}{}, id: {}",
                device.name,
                device.is_default ? " (default)" : "",
                device.id);
        }
    }

    tone state;

    // The renderer is named rather than passed inline, because `audio::renderer` refers to it
    // rather than owning it - a temporary lambda would be gone before the first block. Binding one
    // is a compile error for that reason.
    auto render = [&state](audio::render_block &block) noexcept {
        if (block.output.empty() || block.sample_rate == 0)
            return;

        // Phase is computed from the absolute frame index in double precision. A float accumulator
        // would drift, and a float frame counter would stop counting exactly after 2^24 frames.
        const double phase_step = (two_pi * state.frequency_hz) / static_cast<double>(block.sample_rate);
        const auto stop = state.stop_begin.load(std::memory_order_relaxed);

        for (std::uint32_t f = 0; f < block.frames; ++f)
        {
            const audio::frame_count index = block.position + f;

            const double phase = std::fmod(static_cast<double>(index) * phase_step, two_pi);
            const auto value = static_cast<audio::sample>(
                state.gain_at(index, stop) * static_cast<float>(std::sin(phase)));

            // The frame view removes the interleaving arithmetic, which is where the bugs are.
            for (audio::sample &channel : block.output_frame(f))
                channel = value;
        }
    };

    // Device and stream events reach listeners from pump(), on this thread - never from the
    // driver's notification thread. So a listener may do whatever an ordinary function may do.
    events::bus bus;

    bool device_lost = false;

    auto lost_token = bus.add_listener<audio::device_lost_event>(
        [&device_lost](const audio::device_lost_event &event) {
            logging::error<example_log>("Device lost: {}", event.device_id);
            device_lost = true;
        });

    auto xrun_token = bus.add_listener<audio::xrun_event>(
        [](const audio::xrun_event &event) {
            logging::warn<example_log>("{} xrun(s), {} total", event.count, event.total);
        });

    auto default_token = bus.add_listener<audio::default_device_changed_event>(
        [](const audio::default_device_changed_event &event) {
            // Note what this does *not* do: a running stream is not moved to the new default.
            // Whether following it is right depends on the program, so the module leaves it here.
            logging::info<example_log>("Default {} device is now {}", event.direction, event.device_id);
        });

    audio::stream_config config;
    config.sample_rate = 48000;
    config.output_channels = 2;
    config.block_frames = 512;
    config.bus = &bus;

    auto stream = audio::stream::open(config, render);
    if (!stream)
    {
        // The error formats as a whole sentence: which backend, what failed, and what the device
        // would have accepted instead.
        logging::critical<example_log>("Failed to open audio stream: {}", stream.error());
        return 1;
    }

    // Never assume the request was honoured: the device may have imposed a different rate, channel
    // count or block size, and with fallback enabled it does so silently and successfully.
    const audio::stream_info &info = stream->info();
    state.fade_frames = info.sample_rate / 50; // 20 ms

    logging::info<example_log>("Backend: {}", stream->backend());
    logging::info<example_log>("Device: {}", info.device_name);
    logging::info<example_log>(
        "Format: {} Hz, {} ch ({}), {} frames/block, {:.2f} ms latency{}",
        info.sample_rate,
        info.output_channels,
        info.output_layout,
        info.block_frames,
        std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(info.output_latency).count(),
        info.exclusive ? " (exclusive)" : "");

    if (const auto started = stream->start(); !started)
    {
        logging::critical<example_log>("Failed to start audio stream: {}", started.error());
        return 1;
    }

    logging::info<example_log>("Playing {:.0f} Hz test tone for 2 seconds...", state.frequency_hz);

    // A real program pumps once a frame. This one has no frames, so it pumps on a timer - the point
    // being that events arrive here, in the loop, and not on whichever thread noticed them.
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (std::chrono::steady_clock::now() < deadline && !device_lost)
    {
        stream->pump();
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    if (!device_lost)
    {
        // Ask for a fade-out before stopping, so the tone ends rather than being cut off.
        state.stop_begin.store(stream->stats().frames_rendered, std::memory_order_relaxed);
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
    }

    stream->stop();
    stream->pump();

    const audio::stream_stats stats = stream->stats();
    logging::info<example_log>(
        "Rendered {} frames in {} blocks; {} xruns, peak load {:.1f}%",
        stats.frames_rendered,
        stats.blocks,
        stats.xruns,
        stats.peak_load * 100.0);

    lost_token.remove();
    xrun_token.remove();
    default_token.remove();

    return 0;
}
