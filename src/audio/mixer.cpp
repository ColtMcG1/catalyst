/**
 * @file mixer.cpp
 * @brief The two halves of `mixer`: the game-thread half that allocates handles and posts commands,
 * and the render-thread half that applies them and sums voices into a block.
 * @details The file is laid out in that order, and the division is the design. Every member of
 * `impl` below is commented with which thread owns it, because the correctness of the whole type is
 * that no field is written on one thread and read on the other except through a `command_ring` or
 * an atomic - and that is a property you can only check by reading the declarations.
 *
 * Two of those atomics do more work than they look like. `playing[i]` holds the generation
 * occupying voice slot i, written by the game thread when it allocates the slot and cleared by the
 * render thread when the voice ends; `held[i]` does the same for a sound. They exist so that
 * reclaiming a slot needs no message: a message can be refused by a full ring, and a refused
 * "this voice ended" is a slot that never comes back.
 * License: MIT (see LICENSE).
 */

#include <catalyst/audio/mixer.hpp>

#include <catalyst/audio/command.hpp>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <vector>

namespace catalyst::audio
{

    namespace
    {

        /** @brief A gain is a multiplier, so the only value it cannot take is a negative one. */
        [[nodiscard]] constexpr float sane_gain(float gain) noexcept
        {
            return gain > 0.0f ? gain : 0.0f;
        }

        [[nodiscard]] constexpr float sane_pan(float pan) noexcept
        {
            return std::clamp(pan, -1.0f, 1.0f);
        }

        /**
         * @brief Keeps a voice moving forward.
         * @details Zero would be a voice that reads the same frame for ever and never reaches its
         * end, which is a leaked slot rather than an audible mistake. A tiny positive step is
         * audible, which is what a caller who passed zero needs.
         */
        [[nodiscard]] constexpr float sane_speed(float speed) noexcept
        {
            return speed > 1.0e-4f ? speed : 1.0e-4f;
        }

        /** @brief Zero limits are raised to one: a mixer that can hold nothing is not a mixer. */
        [[nodiscard]] constexpr mixer_config sane(const mixer_config &config) noexcept
        {
            return mixer_config{
                .max_voices = config.max_voices == 0 ? 1u : config.max_voices,
                .max_sounds = config.max_sounds == 0 ? 1u : config.max_sounds,
                .command_capacity = config.command_capacity == 0 ? std::size_t{1} : config.command_capacity,
                .command_budget = config.command_budget,
            };
        }

        [[nodiscard]] constexpr voice_params sane(const voice_params &params) noexcept
        {
            return voice_params{
                .gain = sane_gain(params.gain),
                .pan = sane_pan(params.pan),
                .speed = sane_speed(params.speed),
                .looping = params.looping,
            };
        }

        /**
         * @struct pan_gains
         * @brief What a pan position means for the left and right output channels.
         * @details Two laws, because a mono source and a stereo source are asking different
         * questions. A mono source is being *placed*, so it uses the constant-power law - equal
         * power at every position, which is -3 dB in each channel at the centre rather than the
         * +3 dB bump a linear law would give. A source that already has a stereo image is being
         * *balanced*, so it uses the linear law and sits at unity in both channels when centred,
         * because turning a centred stereo mix down by 3 dB is not what anyone means by "pan 0".
         */
        struct pan_gains
        {
            float left = 1.0f;
            float right = 1.0f;
        };

        [[nodiscard]] pan_gains gains_for(float pan, bool mono_source) noexcept
        {
            if (mono_source)
            {
                const float theta = (pan + 1.0f) * 0.25f * std::numbers::pi_v<float>;
                return {std::cos(theta), std::sin(theta)};
            }

            return {pan <= 0.0f ? 1.0f : 1.0f - pan, pan >= 0.0f ? 1.0f : 1.0f + pan};
        }

    } // namespace

    // ------------------------------------------------------------------------------------------------------------------
    // State
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @struct mixer::impl
     * @brief Everything a mixer owns, grouped by the thread that may touch it.
     */
    struct mixer::impl
    {
        /** @brief Takes an already-sanitised config, because `commands` is sized from it here. */
        explicit impl(const mixer_config &settings)
            : config(settings), commands(settings.command_capacity)
        {
        }

        // ---- Built once, read by both ----------------------------------------------------------

        const mixer_config config;
        command_ring commands;

        // ---- Game thread only ------------------------------------------------------------------

        /** @brief One sound slot as the game thread sees it: the owner of the memory. */
        struct control_sound
        {
            std::unique_ptr<sound_buffer> buffer; ///< Null when the slot is free.
            bool releasing = false;               ///< Waiting for the render thread to let go.
            bool release_posted = false;          ///< The release command was accepted.
        };

        std::vector<control_sound> sounds;
        std::vector<std::uint32_t> sound_generation; ///< Current occupant of each sound slot.
        std::vector<std::uint32_t> free_sounds;

        std::vector<std::uint32_t> voice_generation; ///< Current occupant of each voice slot.
        std::vector<bool> voice_allocated;
        std::vector<std::uint32_t> free_voices;

        /**
         * @brief Stops the ring had no room for, to be posted again by `collect()`.
         * @details A lost "quieter" is inaudible and a lost "stop" is a sound that keeps playing,
         * so the two refusals are not worth the same. Parameter changes are allowed to be dropped -
         * the next one supersedes them - and anything with a lasting consequence is remembered.
         */
        std::vector<voice_id> pending_stops;
        bool pending_stop_all = false;

        // ---- Published across threads ----------------------------------------------------------

        /**
         * @brief The generation occupying voice slot i, or 0 when the slot is idle.
         * @details Written by the game thread on `play` and by the render thread when the voice
         * ends. It is what makes `collect()` able to reclaim a slot without being told, and what
         * `is_playing` reads.
         */
        std::unique_ptr<std::atomic<std::uint32_t>[]> playing;

        /**
         * @brief The generation the render thread still holds in sound slot i, or 0.
         * @details Cleared by the render thread when it drops the pointer, which is the signal that
         * `collect()` may free the buffer.
         */
        std::unique_ptr<std::atomic<std::uint32_t>[]> held;

        /** @brief Sounds held. Written by the game thread; atomic because `stats()` is not its. */
        std::atomic<std::uint32_t> live_sounds{0};

        std::atomic<std::uint32_t> active_voices{0};
        std::atomic<std::uint64_t> voices_started{0};
        std::atomic<std::uint64_t> voices_ended{0};
        std::atomic<std::uint64_t> voices_refused{0};
        std::atomic<std::uint64_t> blocks{0};

        // ---- Render thread only ----------------------------------------------------------------

        /** @brief One voice as the render thread plays it. */
        struct audio_voice
        {
            bool active = false;
            std::uint32_t generation = 0;
            const sound_buffer *buffer = nullptr;
            std::uint32_t sound_index = 0;

            double position = 0.0; ///< Read head, in source frames. Fractional: see `speed`.
            float speed = 1.0f;
            bool looping = false;

            float gain = 1.0f; ///< Where the ramp is now.
            float gain_target = 1.0f;
            float pan = 0.0f;
            float pan_target = 0.0f;
        };

        std::vector<audio_voice> voices;
        std::vector<const sound_buffer *> audio_sounds;

        float master_gain = 1.0f;
        float master_target = 1.0f;

        // ---- Render-thread operations, called from commands -------------------------------------

        /** @brief Render thread. Puts a voice in a slot the game thread already reserved. */
        void start_voice(
            std::uint32_t index,
            std::uint32_t generation,
            std::uint32_t sound_index,
            const sound_buffer *buffer,
            voice_params params) noexcept
        {
            if (index >= voices.size())
                return;

            audio_voice &voice = voices[index];

            // The slot was reserved for this generation, so anything already in it has ended and
            // simply has not been collected yet.
            voice.active = buffer != nullptr && buffer->frames() != 0;
            voice.generation = generation;
            voice.buffer = buffer;
            voice.sound_index = sound_index;
            voice.position = 0.0;
            voice.speed = params.speed;
            voice.looping = params.looping;

            // No ramp on the first block: a voice starting at zero and ramping up over one block is
            // a fade-in nobody asked for.
            voice.gain = params.gain;
            voice.gain_target = params.gain;
            voice.pan = params.pan;
            voice.pan_target = params.pan;

            if (voice.active)
            {
                voices_started.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                // An empty sound is a voice that ends before it is heard, and the slot has to come
                // back the same way any other finished voice's does.
                voice.buffer = nullptr;
                playing[index].store(0, std::memory_order_release);
            }
        }

        /** @brief Render thread. Ends a voice and releases the slot back to `collect()`. */
        void end_voice(std::uint32_t index) noexcept
        {
            audio_voice &voice = voices[index];
            if (!voice.active)
                return;

            voice.active = false;
            voice.buffer = nullptr;
            voices_ended.fetch_add(1, std::memory_order_relaxed);
            playing[index].store(0, std::memory_order_release);
        }

        /** @brief Render thread. The voice in @p index if it is still the one @p generation named. */
        [[nodiscard]] audio_voice *voice_at(std::uint32_t index, std::uint32_t generation) noexcept
        {
            if (index >= voices.size())
                return nullptr;

            audio_voice &voice = voices[index];
            if (!voice.active || voice.generation != generation)
                return nullptr;

            return &voice;
        }

        /** @brief Render thread. Sums voice @p index into @p block, ending it if it runs out. */
        void mix_voice(std::uint32_t index, render_block &block) noexcept;

        // ---- Game-thread operations that may have to be tried twice ----------------------------

        /** @brief Game thread. Queues the end of one voice. False when the ring is full. */
        [[nodiscard]] bool post_stop(voice_id voice)
        {
            const std::uint32_t index = voice.index;
            const std::uint32_t generation = voice.generation;

            return commands.post([self = this, index, generation]() noexcept
                                 {
                if (self->voice_at(index, generation) != nullptr)
                    self->end_voice(index); });
        }

        /** @brief Game thread. Queues the end of every voice. False when the ring is full. */
        [[nodiscard]] bool post_stop_all()
        {
            return commands.post([self = this]() noexcept
                                 {
                for (std::uint32_t v = 0; v < self->voices.size(); ++v)
                    self->end_voice(v); });
        }

        /** @brief Game thread. Queues a sound's retirement. False when the ring is full. */
        [[nodiscard]] bool post_release(std::uint32_t index)
        {
            return commands.post([self = this, index]() noexcept
                                 {
                self->audio_sounds[index] = nullptr;

                for (std::uint32_t v = 0; v < self->voices.size(); ++v)
                    if (self->voices[v].active && self->voices[v].sound_index == index)
                        self->end_voice(v);

                self->held[index].store(0, std::memory_order_release); });
        }

        /**
         * @brief Game thread. Keeps a refused stop for `collect()` to post again.
         * @details Deduplicated, and capped at one entry per voice slot, so a caller stopping the
         * same voice every frame while the ring stays full cannot grow this without bound.
         */
        void remember_stop(voice_id voice)
        {
            for (const voice_id pending : pending_stops)
                if (pending == voice)
                    return;

            if (pending_stops.size() >= voices.size())
                return;

            pending_stops.push_back(voice);
        }
    };

    // ------------------------------------------------------------------------------------------------------------------
    // Construction
    // ------------------------------------------------------------------------------------------------------------------

    mixer::mixer(const mixer_config &config)
        : impl_(std::make_unique<impl>(sane(config)))
    {
        impl &m = *impl_;

        const auto voices = static_cast<std::size_t>(m.config.max_voices);
        const auto sounds = static_cast<std::size_t>(m.config.max_sounds);

        m.sounds.resize(sounds);
        m.sound_generation.assign(sounds, 0);
        m.voice_generation.assign(voices, 0);
        m.voice_allocated.assign(voices, false);

        // Handed out from the back, so the first sounds and voices of a run get slot 0 upward and a
        // log line reads in the order things happened.
        m.free_sounds.resize(sounds);
        for (std::size_t i = 0; i < sounds; ++i)
            m.free_sounds[i] = static_cast<std::uint32_t>(sounds - 1 - i);

        m.free_voices.resize(voices);
        for (std::size_t i = 0; i < voices; ++i)
            m.free_voices[i] = static_cast<std::uint32_t>(voices - 1 - i);

        m.playing = std::make_unique<std::atomic<std::uint32_t>[]>(voices);
        m.held = std::make_unique<std::atomic<std::uint32_t>[]>(sounds);

        m.voices.resize(voices);
        m.audio_sounds.assign(sounds, nullptr);
    }

    mixer::~mixer() = default;

    // ------------------------------------------------------------------------------------------------------------------
    // Sounds - game thread
    // ------------------------------------------------------------------------------------------------------------------

    sound_id mixer::add_sound(sound_buffer buffer)
    {
        impl &m = *impl_;

        if (m.free_sounds.empty())
            return no_sound;

        const std::uint32_t index = m.free_sounds.back();
        m.free_sounds.pop_back();

        std::uint32_t generation = m.sound_generation[index] + 1;
        if (generation == 0)
            generation = 1; // 0 is the null handle, so a wrapped generation skips it.

        m.sound_generation[index] = generation;
        m.sounds[index].buffer = std::make_unique<sound_buffer>(std::move(buffer));
        m.sounds[index].releasing = false;
        m.sounds[index].release_posted = false;
        m.held[index].store(generation, std::memory_order_release);

        const sound_buffer *pointer = m.sounds[index].buffer.get();

        if (!m.commands.post([self = &m, index, pointer]() noexcept
                             { self->audio_sounds[index] = pointer; }))
        {
            // The render thread will never learn about it, so nothing here happened.
            m.sounds[index].buffer.reset();
            m.held[index].store(0, std::memory_order_release);
            m.free_sounds.push_back(index);
            return no_sound;
        }

        m.live_sounds.fetch_add(1, std::memory_order_relaxed);
        return sound_id{index, generation};
    }

    void mixer::release_sound(sound_id sound)
    {
        impl &m = *impl_;

        if (!has_sound(sound))
            return;

        const std::uint32_t index = sound.index;
        m.sounds[index].releasing = true;

        // A refused command is not a lost release: `collect()` posts it again. Losing it would mean
        // a buffer the render thread never lets go of, which is a leak rather than a dropped event.
        m.sounds[index].release_posted = m.post_release(index);
    }

    bool mixer::has_sound(sound_id sound) const noexcept
    {
        const impl &m = *impl_;

        if (!sound.valid() || sound.index >= m.sounds.size())
            return false;

        return m.sound_generation[sound.index] == sound.generation &&
               m.sounds[sound.index].buffer != nullptr &&
               !m.sounds[sound.index].releasing;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Voices - game thread
    // ------------------------------------------------------------------------------------------------------------------

    voice_id mixer::play(sound_id sound, const voice_params &params)
    {
        impl &m = *impl_;

        if (!has_sound(sound))
            return no_voice;

        if (m.free_voices.empty())
        {
            m.voices_refused.fetch_add(1, std::memory_order_relaxed);
            return no_voice;
        }

        const std::uint32_t index = m.free_voices.back();
        m.free_voices.pop_back();

        std::uint32_t generation = m.voice_generation[index] + 1;
        if (generation == 0)
            generation = 1;

        m.voice_generation[index] = generation;
        m.voice_allocated[index] = true;

        // Claimed before the command is posted, so a `collect()` that runs in between cannot see an
        // idle slot and hand it out twice.
        m.playing[index].store(generation, std::memory_order_release);

        const sound_buffer *pointer = m.sounds[sound.index].buffer.get();
        const std::uint32_t sound_index = sound.index;
        const voice_params settings = sane(params);

        if (!m.commands.post([self = &m, index, generation, sound_index, pointer, settings]() noexcept
                             { self->start_voice(index, generation, sound_index, pointer, settings); }))
        {
            m.playing[index].store(0, std::memory_order_release);
            m.voice_allocated[index] = false;
            m.free_voices.push_back(index);
            m.voices_refused.fetch_add(1, std::memory_order_relaxed);
            return no_voice;
        }

        return voice_id{index, generation};
    }

    void mixer::stop(voice_id voice)
    {
        impl &m = *impl_;

        if (!voice.valid() || voice.index >= m.voices.size())
            return;

        if (!m.post_stop(voice))
            m.remember_stop(voice);
    }

    void mixer::stop_all()
    {
        impl &m = *impl_;

        if (!m.post_stop_all())
            m.pending_stop_all = true;
    }

    void mixer::set_gain(voice_id voice, float gain)
    {
        impl &m = *impl_;
        const std::uint32_t index = voice.index;
        const std::uint32_t generation = voice.generation;
        const float target = sane_gain(gain);

        m.commands.post([self = &m, index, generation, target]() noexcept
                        {
            if (impl::audio_voice *v = self->voice_at(index, generation))
                v->gain_target = target; });
    }

    void mixer::set_pan(voice_id voice, float pan)
    {
        impl &m = *impl_;
        const std::uint32_t index = voice.index;
        const std::uint32_t generation = voice.generation;
        const float target = sane_pan(pan);

        m.commands.post([self = &m, index, generation, target]() noexcept
                        {
            if (impl::audio_voice *v = self->voice_at(index, generation))
                v->pan_target = target; });
    }

    void mixer::set_speed(voice_id voice, float speed)
    {
        impl &m = *impl_;
        const std::uint32_t index = voice.index;
        const std::uint32_t generation = voice.generation;
        const float target = sane_speed(speed);

        m.commands.post([self = &m, index, generation, target]() noexcept
                        {
            if (impl::audio_voice *v = self->voice_at(index, generation))
                v->speed = target; });
    }

    void mixer::set_master_gain(float gain)
    {
        impl &m = *impl_;
        const float target = sane_gain(gain);

        m.commands.post([self = &m, target]() noexcept
                        { self->master_target = target; });
    }

    bool mixer::is_playing(voice_id voice) const noexcept
    {
        const impl &m = *impl_;

        if (!voice.valid() || voice.index >= m.voices.size())
            return false;

        return m.playing[voice.index].load(std::memory_order_acquire) == voice.generation;
    }

    std::uint32_t mixer::active_voices() const noexcept
    {
        return impl_->active_voices.load(std::memory_order_relaxed);
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Housekeeping - game thread
    // ------------------------------------------------------------------------------------------------------------------

    std::size_t mixer::collect()
    {
        impl &m = *impl_;

        // Anything refused by a full ring is tried again here, before slots are reclaimed - a stop
        // that lands now is a voice that can be collected on this pass rather than the next.
        if (m.pending_stop_all)
            m.pending_stop_all = !m.post_stop_all();

        for (std::size_t i = m.pending_stops.size(); i-- > 0;)
        {
            if (!m.post_stop(m.pending_stops[i]))
                break; // The ring is still full; the rest can wait too.

            m.pending_stops.erase(m.pending_stops.begin() + static_cast<std::ptrdiff_t>(i));
        }

        for (std::uint32_t index = 0; index < m.voice_allocated.size(); ++index)
        {
            if (!m.voice_allocated[index])
                continue;

            if (m.playing[index].load(std::memory_order_acquire) != 0)
                continue;

            m.voice_allocated[index] = false;
            m.free_voices.push_back(index);
        }

        std::size_t freed = 0;

        for (std::uint32_t index = 0; index < m.sounds.size(); ++index)
        {
            impl::control_sound &slot = m.sounds[index];
            if (!slot.releasing)
                continue;

            // A release whose command was refused has not reached the render thread at all, so try
            // again now that a block has probably drained the ring.
            if (!slot.release_posted)
            {
                slot.release_posted = m.post_release(index);
                continue;
            }

            if (m.held[index].load(std::memory_order_acquire) != 0)
                continue;

            // The render thread has let go, so this is the thread that frees it.
            slot.buffer.reset();
            slot.releasing = false;
            slot.release_posted = false;
            m.free_sounds.push_back(index);
            m.live_sounds.fetch_sub(1, std::memory_order_relaxed);
            ++freed;
        }

        return freed;
    }

    mixer_stats mixer::stats() const noexcept
    {
        const impl &m = *impl_;

        mixer_stats out;
        out.active_voices = m.active_voices.load(std::memory_order_relaxed);
        out.live_sounds = m.live_sounds.load(std::memory_order_relaxed);
        out.voices_started = m.voices_started.load(std::memory_order_relaxed);
        out.voices_ended = m.voices_ended.load(std::memory_order_relaxed);
        out.voices_refused = m.voices_refused.load(std::memory_order_relaxed);
        out.commands_refused = m.commands.refused();
        out.blocks = m.blocks.load(std::memory_order_relaxed);
        return out;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Rendering - render thread
    // ------------------------------------------------------------------------------------------------------------------

    void mixer::impl::mix_voice(std::uint32_t index, render_block &block) noexcept
    {
        audio_voice &voice = voices[index];
        const sound_buffer *buffer = voice.buffer;
        const auto source = buffer->samples();
        const frame_count source_frames = buffer->frames();
        const channel_count source_channels = buffer->channels();

        const std::uint32_t frames = block.frames;
        const channel_count out_channels = block.output_channels;
        const float span = static_cast<float>(frames);

        // How far the read head moves per output frame: the rate ratio, times the caller's pitch.
        double step = static_cast<double>(voice.speed);
        if (block.sample_rate != 0 && buffer->sample_rate() != 0)
            step *= static_cast<double>(buffer->sample_rate()) /
                    static_cast<double>(block.sample_rate);

        // Pan is evaluated twice per block rather than twice per frame, and the channel gains are
        // interpolated between - the trigonometry costs the same as a few frames of mixing.
        const bool mono_source = source_channels == 1;
        const pan_gains from = gains_for(voice.pan, mono_source);
        const pan_gains to = gains_for(voice.pan_target, mono_source);

        float left = from.left;
        float right = from.right;
        const float left_step = (to.left - from.left) / span;
        const float right_step = (to.right - from.right) / span;

        float gain = voice.gain;
        const float gain_step = (voice.gain_target - voice.gain) / span;

        bool finished = false;

        for (std::uint32_t frame = 0; frame < frames; ++frame)
        {
            if (voice.position >= static_cast<double>(source_frames))
            {
                if (!voice.looping)
                {
                    finished = true;
                    break;
                }

                voice.position = std::fmod(voice.position, static_cast<double>(source_frames));
            }

            const auto lower = static_cast<frame_count>(voice.position);
            const float fraction = static_cast<float>(voice.position - static_cast<double>(lower));

            // The frame after the last one is the first frame again for a loop, and itself for a
            // one-shot - which holds the final sample rather than interpolating toward silence.
            frame_count upper = lower + 1;
            if (upper >= source_frames)
                upper = voice.looping ? 0 : lower;

            const std::size_t lower_base = static_cast<std::size_t>(lower) * source_channels;
            const std::size_t upper_base = static_cast<std::size_t>(upper) * source_channels;

            const auto read = [&](channel_count channel) noexcept -> sample
            {
                const sample a = source[lower_base + channel];
                const sample b = source[upper_base + channel];
                return a + (b - a) * fraction;
            };

            const sample first = read(0);
            const sample second = source_channels >= 2 ? read(1) : first;
            const auto out = block.output_frame(frame);

            if (out_channels == 1)
            {
                out[0] += (first + second) * 0.5f * gain;
            }
            else if (out_channels == 2)
            {
                out[0] += first * left * gain;
                out[1] += second * right * gain;
            }
            else
            {
                // Beyond stereo there is nothing to pan with until the Tier 3 panner: channels map
                // straight across, and a mono source goes to the front pair at equal power.
                if (mono_source)
                {
                    const float centre = std::numbers::sqrt2_v<float> * 0.5f;
                    out[0] += first * centre * gain;
                    out[1] += first * centre * gain;
                }
                else
                {
                    const channel_count shared = std::min(source_channels, out_channels);
                    for (channel_count channel = 0; channel < shared; ++channel)
                        out[channel] += read(channel) * gain;
                }
            }

            voice.position += step;
            left += left_step;
            right += right_step;
            gain += gain_step;
        }

        if (finished)
        {
            end_voice(index);
            return;
        }

        // Landed exactly on the target rather than wherever the increments accumulated to, so a
        // ramp that is meant to reach silence reaches it.
        voice.gain = voice.gain_target;
        voice.pan = voice.pan_target;
    }

    void mixer::render(render_block &block) noexcept
    {
        impl &m = *impl_;

        const std::size_t budget = m.config.command_budget == 0
                                       ? std::numeric_limits<std::size_t>::max()
                                       : m.config.command_budget;
        m.commands.execute(budget);

        m.blocks.fetch_add(1, std::memory_order_relaxed);

        if (block.frames == 0 || block.output_channels == 0 || block.output.empty())
            return;

        block.silence();

        std::uint32_t active = 0;
        for (std::uint32_t index = 0; index < m.voices.size(); ++index)
        {
            if (!m.voices[index].active)
                continue;

            m.mix_voice(index, block);

            if (m.voices[index].active)
                ++active;
        }

        m.active_voices.store(active, std::memory_order_relaxed);

        // Master gain is one pass over the block rather than a multiply inside every voice, and it
        // is skipped entirely at unity - which is where it sits in most programs.
        if (m.master_gain != 1.0f || m.master_target != 1.0f)
        {
            const float span = static_cast<float>(block.frames);
            const float step = (m.master_target - m.master_gain) / span;

            float gain = m.master_gain;
            for (std::uint32_t frame = 0; frame < block.frames; ++frame)
            {
                for (sample &value : block.output_frame(frame))
                    value *= gain;

                gain += step;
            }

            m.master_gain = m.master_target;
        }
    }

} // namespace catalyst::audio
