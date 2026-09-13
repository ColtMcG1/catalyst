/**
 * @file test_mixer.cpp
 * @brief Exercises `sound_buffer`, `voice` handles and `mixer` by rendering them through an
 * `offline_stream` and asserting on the samples that come out.
 * @details The mixer is the first thing in the module with two halves that disagree about time -
 * the game thread decides, the render thread applies - so almost everything worth checking is a
 * question about *when*. Does a handle answered now survive until the block that uses it? Does a
 * sound released now stay alive until the render thread has stopped reading it? Is a stale handle
 * rejected or does it turn down whatever took its slot? Rendering offline is what makes those
 * questions decidable: one thread, one block at a time, exact sample values.
 *
 * The last case does start a second thread, because the arrangement the type is built for - a game
 * thread posting while a render thread renders - is not exercised by anything above it.
 * License: MIT (see LICENSE).
 */

#include "../test_common.hpp"

#include <catalyst/audio/mixer.hpp>
#include <catalyst/audio/offline.hpp>
#include <catalyst/audio/sound.hpp>

#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <numbers>
#include <thread>
#include <vector>

using namespace catalyst;
using namespace catalyst::audio;

namespace
{

    constexpr sample tolerance = 1.0e-5f;

    [[nodiscard]] bool close_enough(sample a, sample b) noexcept
    {
        return std::fabs(a - b) <= tolerance;
    }

    /// Every sample is @p value, so any gain or pan applied to it is readable directly.
    [[nodiscard]] sound_buffer constant_sound(
        sample value,
        frame_count frames,
        channel_count channels,
        sample_rate_t rate = 48000)
    {
        std::vector<sample> samples(static_cast<std::size_t>(frames) * channels, value);
        return sound_buffer(std::move(samples), rate, channels);
    }

    /// Frame f holds the value f in every channel, so a read position is visible in the output.
    [[nodiscard]] sound_buffer ramp_sound(
        frame_count frames,
        channel_count channels,
        sample_rate_t rate = 48000)
    {
        std::vector<sample> samples(static_cast<std::size_t>(frames) * channels, sample{0});

        for (frame_count frame = 0; frame < frames; ++frame)
            for (channel_count channel = 0; channel < channels; ++channel)
                samples[static_cast<std::size_t>(frame) * channels + channel] =
                    static_cast<sample>(frame);

        return sound_buffer(std::move(samples), rate, channels);
    }

    [[nodiscard]] offline_config stereo_config(std::uint32_t block_frames = 16)
    {
        offline_config config;
        config.sample_rate = 48000;
        config.output_channels = 2;
        config.block_frames = block_frames;
        return config;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // sound_buffer
    // ------------------------------------------------------------------------------------------------------------------

    void test_sound_buffer_reports_its_shape()
    {
        const sound_buffer sound = ramp_sound(10, 2, 44100);

        CT_REQUIRE(sound.frames() == 10);
        CT_REQUIRE(sound.channels() == 2);
        CT_REQUIRE(sound.sample_rate() == 44100);
        CT_REQUIRE(sound.samples().size() == 20);
        CT_REQUIRE(!sound.empty());
        CT_REQUIRE(close_enough(sound.frame(3)[0], 3.0f));
        CT_REQUIRE(sound.frame(10).empty());
    }

    /// A partial trailing frame would make `frames()` and `samples().size()` disagree.
    void test_sound_buffer_drops_a_partial_frame()
    {
        const sound_buffer sound(std::vector<sample>{1.0f, 2.0f, 3.0f}, 48000, 2);

        CT_REQUIRE(sound.frames() == 1);
        CT_REQUIRE(sound.samples().size() == 2);
    }

    void test_sound_buffer_never_has_zero_channels()
    {
        const sound_buffer sound(std::vector<sample>{1.0f, 2.0f}, 48000, 0);

        CT_REQUIRE(sound.channels() == 1);
        CT_REQUIRE(sound.frames() == 2);
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Rendering
    // ------------------------------------------------------------------------------------------------------------------

    void test_a_mixer_with_nothing_playing_renders_silence()
    {
        mixer mix;

        auto opened = offline_stream::open(stereo_config(), mix);
        CT_REQUIRE(opened.has_value());
        opened->render(32);

        for (const sample value : opened->captured())
            CT_REQUIRE(value == 0.0f);

        CT_REQUIRE(mix.stats().blocks == 2);
        CT_REQUIRE(mix.active_voices() == 0);
    }

    /// A stereo source panned centre uses the balance law, so it comes out untouched.
    void test_a_stereo_voice_at_unity_is_the_source_exactly()
    {
        mixer mix;

        const sound_id sound = mix.add_sound(ramp_sound(64, 2));
        CT_REQUIRE(sound.valid());
        CT_REQUIRE(mix.has_sound(sound));

        const voice_id voice = mix.play(sound);
        CT_REQUIRE(voice.valid());
        CT_REQUIRE(mix.is_playing(voice));

        auto opened = offline_stream::open(stereo_config(), mix);
        CT_REQUIRE(opened.has_value());
        opened->render(32);

        const auto out = opened->captured();
        CT_REQUIRE(out.size() == 64);

        for (std::size_t frame = 0; frame < 32; ++frame)
        {
            CT_REQUIRE(close_enough(out[frame * 2], static_cast<sample>(frame)));
            CT_REQUIRE(close_enough(out[frame * 2 + 1], static_cast<sample>(frame)));
        }

        CT_REQUIRE(mix.active_voices() == 1);
        CT_REQUIRE(mix.stats().voices_started == 1);
    }

    /// A mono source is *placed*, so centring it costs 3 dB rather than doubling its power.
    void test_a_mono_voice_is_panned_at_constant_power()
    {
        const sample centre = std::numbers::sqrt2_v<float> * 0.5f;

        struct expectation
        {
            float pan;
            sample left;
            sample right;
        };

        const expectation cases[] = {
            {0.0f, centre, centre},
            {-1.0f, 1.0f, 0.0f},
            {1.0f, 0.0f, 1.0f},
        };

        for (const expectation &expected : cases)
        {
            mixer mix;

            const sound_id sound = mix.add_sound(constant_sound(1.0f, 32, 1));
            CT_REQUIRE(mix.play(sound, {.pan = expected.pan}).valid());

            auto opened = offline_stream::open(stereo_config(), mix);
            CT_REQUIRE(opened.has_value());
            opened->render(16);

            const auto out = opened->captured();
            CT_REQUIRE(close_enough(out[0], expected.left));
            CT_REQUIRE(close_enough(out[1], expected.right));
        }
    }

    void test_voices_sum()
    {
        mixer mix;

        const sound_id sound = mix.add_sound(constant_sound(0.25f, 32, 2));
        CT_REQUIRE(mix.play(sound).valid());
        CT_REQUIRE(mix.play(sound).valid());

        auto opened = offline_stream::open(stereo_config(), mix);
        CT_REQUIRE(opened.has_value());
        opened->render(16);

        CT_REQUIRE(close_enough(opened->captured()[0], 0.5f));
        CT_REQUIRE(mix.active_voices() == 2);
    }

    /// The gain given to `play` applies immediately; a gain *changed* later ramps. Both matter: a
    /// sound that faded in over its first block would be a click's opposite but just as wrong.
    void test_gain_applies_at_once_and_changes_by_a_ramp()
    {
        mixer mix;

        const sound_id sound = mix.add_sound(constant_sound(0.8f, 4096, 2));
        const voice_id voice = mix.play(sound, {.gain = 1.0f});
        CT_REQUIRE(voice.valid());

        auto opened = offline_stream::open(stereo_config(16), mix);
        CT_REQUIRE(opened.has_value());

        opened->render(16);
        CT_REQUIRE(close_enough(opened->captured()[0], 0.8f)); // no fade-in

        mix.set_gain(voice, 0.5f);
        opened->clear_captured();
        opened->render(16);

        const auto ramped = opened->captured();
        CT_REQUIRE(close_enough(ramped[0], 0.8f));       // starts where it was
        CT_REQUIRE(ramped[30] < ramped[0]);              // and moves
        CT_REQUIRE(ramped[30] > 0.4f);                   // without arriving early

        opened->clear_captured();
        opened->render(16);

        for (const sample value : opened->captured())
            CT_REQUIRE(close_enough(value, 0.4f));       // settled exactly on the target
    }

    void test_master_gain_scales_everything()
    {
        mixer mix;

        const sound_id sound = mix.add_sound(constant_sound(1.0f, 4096, 2));
        CT_REQUIRE(mix.play(sound).valid());

        auto opened = offline_stream::open(stereo_config(16), mix);
        CT_REQUIRE(opened.has_value());

        mix.set_master_gain(0.25f);
        opened->render(16);   // ramps from 1.0 to 0.25 across this block
        opened->clear_captured();
        opened->render(16);

        for (const sample value : opened->captured())
            CT_REQUIRE(close_enough(value, 0.25f));
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Read position
    // ------------------------------------------------------------------------------------------------------------------

    /// Half speed reads every other frame, interpolating the ones in between.
    void test_speed_moves_the_read_head_and_interpolates()
    {
        mixer mix;

        const sound_id sound = mix.add_sound(ramp_sound(64, 1));
        CT_REQUIRE(mix.play(sound, {.pan = -1.0f, .speed = 0.5f}).valid());

        auto opened = offline_stream::open(stereo_config(), mix);
        CT_REQUIRE(opened.has_value());
        opened->render(16);

        const auto out = opened->captured();
        for (std::size_t frame = 0; frame < 16; ++frame)
            CT_REQUIRE(close_enough(out[frame * 2], static_cast<sample>(frame) * 0.5f));
    }

    /// A sound decoded at half the device's rate must play at pitch, not at double speed.
    void test_a_slower_sound_is_rate_converted()
    {
        mixer mix;

        const sound_id sound = mix.add_sound(ramp_sound(64, 1, 24000));
        CT_REQUIRE(mix.play(sound, {.pan = -1.0f}).valid());

        auto opened = offline_stream::open(stereo_config(), mix);
        CT_REQUIRE(opened.has_value());
        opened->render(16);

        const auto out = opened->captured();
        for (std::size_t frame = 0; frame < 16; ++frame)
            CT_REQUIRE(close_enough(out[frame * 2], static_cast<sample>(frame) * 0.5f));
    }

    void test_a_looping_voice_repeats_and_never_ends()
    {
        mixer mix;

        const sound_id sound = mix.add_sound(ramp_sound(4, 2));
        const voice_id voice = mix.play(sound, {.looping = true});
        CT_REQUIRE(voice.valid());

        auto opened = offline_stream::open(stereo_config(), mix);
        CT_REQUIRE(opened.has_value());
        opened->render(12);

        const auto out = opened->captured();
        for (std::size_t frame = 0; frame < 12; ++frame)
            CT_REQUIRE(close_enough(out[frame * 2], static_cast<sample>(frame % 4)));

        CT_REQUIRE(mix.is_playing(voice));
        CT_REQUIRE(mix.stats().voices_ended == 0);
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Handles and lifetime
    // ------------------------------------------------------------------------------------------------------------------

    /// A one-shot ends by itself, and the slot comes back to the game thread through `collect`.
    void test_a_finished_voice_releases_its_slot()
    {
        mixer mix;

        const sound_id sound = mix.add_sound(constant_sound(1.0f, 8, 2));
        const voice_id voice = mix.play(sound);
        CT_REQUIRE(mix.is_playing(voice));

        auto opened = offline_stream::open(stereo_config(), mix);
        CT_REQUIRE(opened.has_value());
        opened->render(16);

        // Eight frames of source, sixteen rendered: silence after the eighth.
        const auto out = opened->captured();
        CT_REQUIRE(close_enough(out[7 * 2], 1.0f));
        CT_REQUIRE(close_enough(out[8 * 2], 0.0f));

        CT_REQUIRE(!mix.is_playing(voice));
        CT_REQUIRE(mix.active_voices() == 0);
        CT_REQUIRE(mix.stats().voices_ended == 1);

        mix.collect();

        // The slot is reused, and the handle to its previous occupant is not confused with the new
        // one - which is the whole reason a handle carries a generation.
        const voice_id next = mix.play(sound);
        CT_REQUIRE(next.valid());
        CT_REQUIRE(next.index == voice.index);
        CT_REQUIRE(next.generation != voice.generation);
        CT_REQUIRE(!mix.is_playing(voice));
        CT_REQUIRE(mix.is_playing(next));
    }

    void test_stop_ends_a_voice_at_the_next_block()
    {
        mixer mix;

        const sound_id sound = mix.add_sound(constant_sound(1.0f, 4096, 2));
        const voice_id voice = mix.play(sound);

        auto opened = offline_stream::open(stereo_config(), mix);
        CT_REQUIRE(opened.has_value());
        opened->render(16);
        CT_REQUIRE(mix.is_playing(voice));

        mix.stop(voice);
        opened->clear_captured();
        opened->render(16);

        CT_REQUIRE(!mix.is_playing(voice));
        for (const sample value : opened->captured())
            CT_REQUIRE(value == 0.0f);
    }

    /// Commands naming a voice that has gone are dropped, not applied to whoever replaced it.
    void test_a_stale_voice_handle_is_ignored()
    {
        mixer mix;

        const sound_id sound = mix.add_sound(constant_sound(1.0f, 4096, 2));
        const voice_id first = mix.play(sound);

        auto opened = offline_stream::open(stereo_config(), mix);
        CT_REQUIRE(opened.has_value());
        opened->render(16);

        mix.stop(first);
        opened->render(16);
        mix.collect();

        const voice_id second = mix.play(sound);
        CT_REQUIRE(second.index == first.index);

        // Aimed at a voice that ended, on a slot now occupied by another - the generation is what
        // stops it from silencing the wrong sound.
        mix.set_gain(first, 0.0f);
        mix.stop(first);

        opened->clear_captured();
        opened->render(16);

        CT_REQUIRE(mix.is_playing(second));
        CT_REQUIRE(close_enough(opened->captured()[0], 1.0f));
    }

    /// The two-step release: the game thread stops using it, the render thread lets go, and only
    /// then is the memory freed - on the game thread, by `collect`.
    void test_a_released_sound_is_freed_only_after_the_render_thread_lets_go()
    {
        mixer mix;

        const sound_id sound = mix.add_sound(constant_sound(1.0f, 4096, 2));
        const voice_id voice = mix.play(sound);

        auto opened = offline_stream::open(stereo_config(), mix);
        CT_REQUIRE(opened.has_value());
        opened->render(16);
        CT_REQUIRE(mix.stats().live_sounds == 1);

        mix.release_sound(sound);

        // Gone from the game thread's point of view at once...
        CT_REQUIRE(!mix.has_sound(sound));
        CT_REQUIRE(!mix.play(sound).valid());

        // ...but not freed, because the render thread has not been told yet.
        CT_REQUIRE(mix.collect() == 0);
        CT_REQUIRE(mix.stats().live_sounds == 1);

        opened->clear_captured();
        opened->render(16);

        // The release also stopped the voice that was reading it.
        CT_REQUIRE(!mix.is_playing(voice));
        for (const sample value : opened->captured())
            CT_REQUIRE(value == 0.0f);

        CT_REQUIRE(mix.collect() == 1);
        CT_REQUIRE(mix.stats().live_sounds == 0);
    }

    void test_a_stale_sound_handle_is_rejected()
    {
        mixer mix;

        const sound_id first = mix.add_sound(constant_sound(1.0f, 16, 2));
        mix.release_sound(first);

        auto opened = offline_stream::open(stereo_config(), mix);
        CT_REQUIRE(opened.has_value());
        opened->render(16);
        CT_REQUIRE(mix.collect() == 1);

        const sound_id second = mix.add_sound(constant_sound(0.5f, 16, 2));
        CT_REQUIRE(second.index == first.index);
        CT_REQUIRE(second.generation != first.generation);

        CT_REQUIRE(!mix.has_sound(first));
        CT_REQUIRE(!mix.play(first).valid());
        CT_REQUIRE(mix.has_sound(second));
    }

    void test_an_empty_sound_ends_immediately()
    {
        mixer mix;

        const sound_id sound = mix.add_sound(sound_buffer{});
        const voice_id voice = mix.play(sound);
        CT_REQUIRE(voice.valid());

        auto opened = offline_stream::open(stereo_config(), mix);
        CT_REQUIRE(opened.has_value());
        opened->render(16);

        CT_REQUIRE(!mix.is_playing(voice));
        CT_REQUIRE(mix.active_voices() == 0);

        // And the slot still comes back, or a program playing empty sounds would run out.
        mix.collect();
        CT_REQUIRE(mix.play(sound).valid());
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Limits
    // ------------------------------------------------------------------------------------------------------------------

    void test_running_out_of_voices_is_reported()
    {
        mixer mix(mixer_config{.max_voices = 2});

        const sound_id sound = mix.add_sound(constant_sound(1.0f, 4096, 2));
        CT_REQUIRE(mix.play(sound).valid());
        CT_REQUIRE(mix.play(sound).valid());

        const voice_id refused = mix.play(sound);
        CT_REQUIRE(!refused.valid());
        CT_REQUIRE(refused == no_voice);
        CT_REQUIRE(mix.stats().voices_refused == 1);
    }

    void test_running_out_of_sounds_is_reported()
    {
        mixer mix(mixer_config{.max_sounds = 1});

        CT_REQUIRE(mix.add_sound(constant_sound(1.0f, 16, 2)).valid());
        CT_REQUIRE(!mix.add_sound(constant_sound(1.0f, 16, 2)).valid());
    }

    /// The smallest mixer that can exist - one voice, one sound, a ring one command deep - still
    /// plays, which is the only useful meaning of "the limits are raised to one".
    void test_a_zero_sized_config_is_raised_to_one()
    {
        mixer mix(mixer_config{.max_voices = 0, .max_sounds = 0, .command_capacity = 0});

        const sound_id sound = mix.add_sound(constant_sound(1.0f, 4096, 2));
        CT_REQUIRE(sound.valid());

        auto opened = offline_stream::open(stereo_config(), mix);
        CT_REQUIRE(opened.has_value());
        opened->render(16); // Drains the one command the ring can hold.

        const voice_id voice = mix.play(sound);
        CT_REQUIRE(voice.valid());

        opened->clear_captured();
        opened->render(16);
        CT_REQUIRE(close_enough(opened->captured()[0], 1.0f));
    }

    /// A ring too small for a burst loses parameter changes, but never loses a stop: `collect`
    /// posts the refused ones again, so a voice cannot be left playing by a full ring.
    void test_a_refused_stop_is_retried_by_collect()
    {
        mixer mix(mixer_config{.max_voices = 4, .command_capacity = 1});

        const sound_id sound = mix.add_sound(constant_sound(1.0f, 4096, 2));
        CT_REQUIRE(sound.valid());

        auto opened = offline_stream::open(stereo_config(), mix);
        CT_REQUIRE(opened.has_value());
        opened->render(16);

        const voice_id voice = mix.play(sound);
        CT_REQUIRE(voice.valid());
        opened->render(16);
        CT_REQUIRE(mix.is_playing(voice));

        // Fill the one-deep ring, then ask for a stop that cannot possibly fit.
        mix.set_gain(voice, 0.5f);
        mix.stop(voice);
        CT_REQUIRE(mix.stats().commands_refused > 0);

        opened->render(16);
        CT_REQUIRE(mix.is_playing(voice)); // The stop has not arrived yet, and was not lost.

        mix.collect();
        opened->clear_captured();
        opened->render(16);

        CT_REQUIRE(!mix.is_playing(voice));
        for (const sample value : opened->captured())
            CT_REQUIRE(value == 0.0f);
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Two threads
    // ------------------------------------------------------------------------------------------------------------------

    /// The arrangement the type exists for: one thread rendering blocks while another posts.
    void test_a_mixer_survives_a_game_thread_and_a_render_thread()
    {
        constexpr int rounds = 2000;

        mixer mix(mixer_config{.max_voices = 8, .command_capacity = 64});

        const sound_id sound = mix.add_sound(constant_sound(0.1f, 480, 2));
        CT_REQUIRE(sound.valid());

        auto config = stereo_config(64);
        config.capture = false; // A long run should not be a memory test.

        auto opened = offline_stream::open(config, mix);
        CT_REQUIRE(opened.has_value());

        std::atomic<bool> rendering{true};
        std::thread render_thread([&opened, &rendering]
                                  {
            while (rendering.load(std::memory_order_relaxed))
                opened->render(64); });

        // Started, not merely spawned: an optimised build can finish the whole loop below in less
        // time than it takes Windows to schedule the thread, and a test that overlaps nothing is
        // not the test that was wanted.
        while (mix.stats().blocks == 0)
            std::this_thread::yield();

        for (int i = 0; i < rounds; ++i)
        {
            const voice_id voice = mix.play(sound, {.gain = 0.5f, .pan = 0.25f});
            if (voice.valid())
            {
                mix.set_gain(voice, 0.25f);
                if ((i & 1) != 0)
                    mix.stop(voice);
            }

            mix.collect();
            std::this_thread::yield();
        }

        rendering.store(false, std::memory_order_relaxed);
        render_thread.join();

        mix.collect();

        const mixer_stats stats = mix.stats();
        CT_REQUIRE(stats.blocks > 0);
        CT_REQUIRE(stats.voices_started > 0);
        CT_REQUIRE(stats.voices_started >= stats.voices_ended);
        CT_REQUIRE(stats.active_voices <= 8);

        // Everything that was started and has ended must have given its slot back, so the mixer is
        // still able to play after two thousand rounds of churn.
        mix.stop_all();
        opened->render(64);
        mix.collect();
        CT_REQUIRE(mix.play(sound).valid());
    }

} // namespace

int main()
{
    test_sound_buffer_reports_its_shape();
    test_sound_buffer_drops_a_partial_frame();
    test_sound_buffer_never_has_zero_channels();

    test_a_mixer_with_nothing_playing_renders_silence();
    test_a_stereo_voice_at_unity_is_the_source_exactly();
    test_a_mono_voice_is_panned_at_constant_power();
    test_voices_sum();
    test_gain_applies_at_once_and_changes_by_a_ramp();
    test_master_gain_scales_everything();

    test_speed_moves_the_read_head_and_interpolates();
    test_a_slower_sound_is_rate_converted();
    test_a_looping_voice_repeats_and_never_ends();

    test_a_finished_voice_releases_its_slot();
    test_stop_ends_a_voice_at_the_next_block();
    test_a_stale_voice_handle_is_ignored();
    test_a_released_sound_is_freed_only_after_the_render_thread_lets_go();
    test_a_stale_sound_handle_is_rejected();
    test_an_empty_sound_ends_immediately();

    test_running_out_of_voices_is_reported();
    test_running_out_of_sounds_is_reported();
    test_a_zero_sized_config_is_raised_to_one();
    test_a_refused_stop_is_retried_by_collect();

    test_a_mixer_survives_a_game_thread_and_a_render_thread();

    return 0;
}
