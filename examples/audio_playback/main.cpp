/*
 * @file main.cpp
 * @brief Example of using the Catalyst Audio Engine to play a test tone.
 * @details This example demonstrates how to initialize the Catalyst Audio Engine, inspect the
 * format the device actually negotiated, set up a callback to render a 440Hz sine wave, and
 * cleanly shut down after playback. The tone is derived from `render_context::stream_time_frames`
 * and the negotiated sample rate rather than from a private counter, which keeps pitch correct
 * even when the device refuses the requested rate. Fades at both ends minimise clicks, and the
 * stream's health counters are printed at the end.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/catalyst.hpp>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <limits>
#include <thread>

namespace {

constexpr double two_pi = 6.2831853071795864769;

struct tone_state
{
  double frequency_hz = 440.0;
  uint64_t fade_frames = 0;

  // Written by main, read by the render thread.
  std::atomic<uint64_t> stop_begin{std::numeric_limits<uint64_t>::max()};
};

void render_test_tone(catalyst::audio::render_context& context) noexcept
{
  auto* state = static_cast<tone_state*>(context.user);
  if (!state || !context.output || context.sample_rate == 0)
    return;

  constexpr float base_gain = 0.2f;

  // Phase is computed from the absolute frame index in double precision. A float accumulator
  // would drift, and a float frame counter would stop counting exactly after 2^24 frames.
  const double phase_step = (two_pi * state->frequency_hz) / static_cast<double>(context.sample_rate);
  const uint64_t stop_begin = state->stop_begin.load(std::memory_order_relaxed);

  for (uint32_t f = 0; f < context.frames; ++f)
  {
    const uint64_t frame_index = context.stream_time_frames + f;

    float gain = base_gain;

    // Fade in.
    if (state->fade_frames != 0 && frame_index < state->fade_frames)
    {
      gain *= static_cast<float>(
          static_cast<double>(frame_index) / static_cast<double>(state->fade_frames));
    }

    // Fade out once requested.
    if (stop_begin != std::numeric_limits<uint64_t>::max() && frame_index >= stop_begin)
    {
      if (state->fade_frames == 0)
      {
        gain = 0.0f;
      }
      else
      {
        const double t =
            static_cast<double>(frame_index - stop_begin) / static_cast<double>(state->fade_frames);
        gain *= static_cast<float>(t >= 1.0 ? 0.0 : 1.0 - t);
      }
    }

    const double phase = std::fmod(static_cast<double>(frame_index) * phase_step, two_pi);
    const float sample = gain * static_cast<float>(std::sin(phase));

    const uint32_t base = f * context.output_channels;
    for (uint32_t c = 0; c < context.output_channels; ++c)
      context.output[base + c] = sample;
  }
}

} // namespace

int main()
{
  using namespace catalyst::audio;

  catalyst::catalyst_version_anchor();

  engine audio_engine;
  tone_state tone;

  engine_config cfg;
  cfg.sample_rate = 48000;
  cfg.output_channels = 2;
  cfg.frames_per_buffer = 512;
  cfg.callback = &render_test_tone;
  cfg.user = &tone;

  for (const auto backend : engine::available_backends())
    std::printf("Available backend: %s\n", to_string(backend).data());

  if (const auto devices = engine::devices(engine_backend::automatic); devices)
  {
    for (const auto& device : *devices)
    {
      std::printf(
          " - Device: %s%s\n    id: %s\n",
          device.name.c_str(),
          device.is_default ? " (default)" : "",
          device.id.c_str());
    }
  }

  if (const auto result = audio_engine.initialize(cfg); !result)
  {
    std::fprintf(
        stderr,
        "Failed to initialize audio engine: %s\n",
        to_string(result.error()).data());
    return 1;
  }

  // Never assume the request was honoured: the device may have imposed a different rate,
  // channel count or buffer size.
  const auto info = audio_engine.info();
  tone.fade_frames = info.sample_rate / 50; // 20ms

  std::printf(
      "Backend: %s\nDevice: %s\nFormat: %u Hz, %u ch, %u frames/buffer, %.2f ms latency%s\n",
      audio_engine.backend_name().data(),
      info.device_name.c_str(),
      info.sample_rate,
      info.output_channels,
      info.buffer_frames,
      info.output_latency_seconds * 1000.0,
      info.exclusive ? " (exclusive)" : "");

  if (const auto result = audio_engine.start(); !result)
  {
    std::fprintf(
        stderr,
        "Failed to start audio engine: %s\n",
        to_string(result.error()).data());
    return 1;
  }

  std::printf("Playing %.0fHz test tone for 2 seconds...\n", tone.frequency_hz);
  std::this_thread::sleep_for(std::chrono::seconds(2));

  // Request a short fade-out before stopping to reduce clicks.
  tone.stop_begin.store(
      audio_engine.stats().frames_rendered, std::memory_order_relaxed);
  std::this_thread::sleep_for(std::chrono::milliseconds(30));

  audio_engine.stop();

  const auto stats = audio_engine.stats();
  std::printf(
      "Rendered %llu frames in %llu callbacks; %llu xruns, peak load %.1f%%\n",
      static_cast<unsigned long long>(stats.frames_rendered),
      static_cast<unsigned long long>(stats.callback_count),
      static_cast<unsigned long long>(stats.xruns),
      stats.peak_load * 100.0);

  audio_engine.shutdown();

  return 0;
}
