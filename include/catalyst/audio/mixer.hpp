/**
 * @file mixer.hpp
 * @brief Voices, and the thing that sums them: the first consumer of the command ring, and what a
 * game actually talks to when it wants a sound played.
 * @details A mixer is one object with two halves and a queue between them. The game thread asks -
 * play this, quieter, stop that - and every ask becomes a @ref command. The render thread applies
 * the queue at the top of a block and then sums whatever is playing into the block's output. No
 * lock, no allocation, and no field written on one thread while the other is reading it.
 *
 *     audio::mixer mix;
 *     const auto footstep = mix.add_sound(std::move(buffer));
 *
 *     auto stream = audio::stream::open(cfg, mix);        // a mixer *is* a renderable
 *     stream->start();
 *
 *     mix.play(footstep, {.gain = 0.8f, .pan = -0.3f});   // game thread, any time
 *     mix.collect();                                      // once a frame, beside pump()
 *
 * ### The four decisions
 *
 * **The mixer takes its format from the block, not from a config.** There is no `sample_rate` or
 * `output_channels` to set, because `render_block` already carries both and a device is free to
 * negotiate something other than what was asked for. A mixer configured separately could disagree
 * with the stream feeding it, and the disagreement would be inaudible until it was a wrong pitch.
 *
 * **A handle is answered immediately; the work happens later.** @ref play returns a
 * @ref voice_id before the render thread has seen the command, because a caller that has to wait a
 * block to learn what it just started cannot do anything useful with the answer. The slot is
 * allocated on the game thread, the command carries the slot with it, and the two agree because the
 * generation is decided at the same moment as the slot.
 *
 * **Nothing is freed on the render thread, and nothing needs a return ring to say so.**
 * command.hpp describes handing an object back through a second ring, which is right when the
 * render thread holds the only reference. A mixer is arranged so that it never does: the game
 * thread owns every `sound_buffer` for as long as the mixer knows about it, and the render thread
 * only ever holds a pointer. Retiring one is therefore a pointer being dropped and an atomic word
 * being cleared - which, unlike a message, cannot be refused by a full ring. @ref collect is what
 * notices and frees, on the game thread.
 *
 * **Gain and pan ramp across a block; they do not jump.** A gain applied as a step is a
 * discontinuity, and a discontinuity is a click - which is why a mixer is not just a loop that
 * adds. Each voice interpolates from where it was to where it was asked to be over exactly one
 * block, so a fade written as a command per frame comes out smooth.
 *
 * ### What is not here yet
 *
 * Buses. Every voice sums straight into the block, and @ref mixer::set_master_gain is the only
 * thing above them. A `mixer_bus` tree - groups, per-group effects, sends - is the next part of
 * Tier 2 and slots in between the voice loop and the block without changing this surface.
 *
 * Resampling worth the name. A voice whose sound was decoded at a different rate than the device
 * runs at is played back with a linearly interpolated read position, which is correct in pitch and
 * cheap, and audibly imperfect on large ratios. The Tier 4 resampler replaces the read, not the
 * rest of the voice.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/audio/block.hpp>
#include <catalyst/audio/sound.hpp>
#include <catalyst/audio/types.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace catalyst::audio
{

    /**
     * @struct voice_id
     * @brief A handle to one playing sound: a slot index plus the generation that occupied it.
     * @details A generation of 0 is the null handle. A voice that finishes releases its slot, and
     * the next occupant gets a new generation, so a handle kept after the sound ended is rejected
     * rather than quietly turning down whatever is playing there now.
     */
    struct voice_id
    {
        std::uint32_t index{0};
        std::uint32_t generation{0};

        [[nodiscard]] constexpr bool valid() const noexcept { return generation != 0; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

        [[nodiscard]] friend constexpr bool operator==(voice_id, voice_id) noexcept = default;
    };

    /** @brief The handle that refers to no voice. What @ref mixer::play returns on failure. */
    inline constexpr voice_id no_voice{};

    /**
     * @struct voice_params
     * @brief How a voice should sound. Designated initialisers are the intended spelling.
     */
    struct voice_params
    {
        /** @brief Linear gain, not decibels - `db_to_gain()` converts. Negative is clamped to 0. */
        float gain = 1.0f;

        /**
         * @brief Stereo position: -1 hard left, 0 centre, +1 hard right. Clamped.
         * @details Constant-power, so a source panned centre is not louder than the same source
         * panned hard over. Ignored when the output is not stereo.
         */
        float pan = 0.0f;

        /**
         * @brief Playback rate multiplier, on top of any rate conversion. 2.0 is an octave up.
         * @details Zero or less is clamped to a very small positive value rather than freezing the
         * voice, so a mistake is audible instead of a voice that never finishes.
         */
        float speed = 1.0f;

        /** @brief Restart from the beginning at the end instead of finishing. */
        bool looping = false;
    };

    /**
     * @struct mixer_config
     * @brief The sizes a mixer preallocates. Everything else it learns from the block.
     * @details These are limits, not reservations of anything expensive: a voice slot is a few
     * dozen bytes. Set them high enough that @ref mixer_stats::voices_refused stays at zero.
     */
    struct mixer_config
    {
        /** @brief How many sounds may play at once. */
        std::uint32_t max_voices = 64;

        /** @brief How many distinct sounds the mixer may hold. */
        std::uint32_t max_sounds = 256;

        /** @brief How many commands may be in flight between the two threads. */
        std::size_t command_capacity = 256;

        /**
         * @brief Commands applied per block, or 0 for all of them.
         * @details A cap keeps a burst on the game thread from making one block expensive. The
         * default applies everything, which is right until a profile says otherwise.
         */
        std::size_t command_budget = 0;
    };

    /**
     * @struct mixer_stats
     * @brief What the mixer has been doing, for a debug overlay or a log line.
     */
    struct mixer_stats
    {
        /** @brief Voices producing audio as of the last block. */
        std::uint32_t active_voices = 0;

        /** @brief Sounds currently held, including any waiting for @ref mixer::collect. */
        std::uint32_t live_sounds = 0;

        /** @brief Voices that have started since the mixer was built. */
        std::uint64_t voices_started = 0;

        /** @brief Voices that have ended, by finishing or by being stopped. */
        std::uint64_t voices_ended = 0;

        /** @brief Calls to @ref mixer::play that found no free slot. Should stay zero. */
        std::uint64_t voices_refused = 0;

        /**
         * @brief Commands the ring had no room for.
         * @details Not all of them were lost - @ref mixer::collect posts refused stops and releases
         * again - but a number that keeps climbing means `command_capacity` is too small for the
         * bursts this program produces, and the parameter changes among them *were* lost.
         */
        std::uint64_t commands_refused = 0;

        /** @brief Blocks rendered. */
        std::uint64_t blocks = 0;
    };

    /**
     * @class mixer
     * @brief Holds sounds, plays voices, and sums them into a block.
     * @details Two threads, and which one may call what is the whole contract:
     *
     * | Game thread | Render thread |
     * | --- | --- |
     * | @ref add_sound, @ref release_sound | @ref render |
     * | @ref play, @ref stop, @ref stop_all | |
     * | @ref set_gain, @ref set_pan, @ref set_speed, @ref set_master_gain | |
     * | @ref collect, @ref stats, @ref is_playing | |
     *
     * "Game thread" means one thread, the same one each time - the game-thread half is a
     * `command_ring` producer, and that is a single-producer queue. Calling @ref play from a worker
     * pool is the one way to misuse this type that will not announce itself.
     *
     * Neither copyable nor movable, because the renderer holds a pointer to it for the life of the
     * stream. Build it where it will live.
     */
    class mixer
    {
    public:
        /**
         * @brief Builds a mixer and allocates everything it will ever need.
         * @param config Sizes; see @ref mixer_config. Zero limits are raised to one.
         * @details This constructor is the only place a mixer allocates. Nothing on the render
         * path, and nothing @ref play does, touches the allocator.
         */
        explicit mixer(const mixer_config &config = {});

        ~mixer();

        mixer(const mixer &) = delete;
        mixer &operator=(const mixer &) = delete;
        mixer(mixer &&) = delete;
        mixer &operator=(mixer &&) = delete;

        // ------------------------------------------------------------------------------------------------------------
        // Sounds - game thread
        // ------------------------------------------------------------------------------------------------------------

        /**
         * @brief Gives the mixer a sound to play and returns the handle it will be known by.
         * @param buffer The audio, moved in. The mixer holds it until @ref release_sound and
         * @ref collect have both happened.
         * @return A handle, or @ref no_sound if the mixer already holds `max_sounds`.
         * @details The render thread can see the sound from the next block, but it need not have:
         * a @ref play posted immediately after this one is applied after it, because a ring keeps
         * its order.
         */
        [[nodiscard]] sound_id add_sound(sound_buffer buffer);

        /**
         * @brief Stops every voice playing @p sound and begins releasing it.
         * @param sound The handle to release. A stale or null handle is ignored.
         * @details The memory is not freed here. The render thread drops its pointer first, and
         * the next @ref collect frees the buffer and reuses the slot - so a sound is never
         * destroyed underneath a voice reading it.
         */
        void release_sound(sound_id sound);

        /** @brief True while the mixer still holds @p sound. */
        [[nodiscard]] bool has_sound(sound_id sound) const noexcept;

        // ------------------------------------------------------------------------------------------------------------
        // Voices - game thread
        // ------------------------------------------------------------------------------------------------------------

        /**
         * @brief Starts @p sound playing.
         * @param sound What to play. A stale or null handle starts nothing.
         * @param params How it should sound; see @ref voice_params.
         * @return A handle to the voice, or @ref no_voice if every slot is in use or @p sound is
         * not held.
         */
        [[nodiscard]] voice_id play(sound_id sound, const voice_params &params = {});

        /**
         * @brief Ends @p voice at the next block. A stale or null handle is ignored.
         * @details There is no fade: a voice stopped mid-waveform is a discontinuity. Ramp the gain
         * to zero first if that matters.
         *
         * A stop the command ring had no room for is remembered and posted again by @ref collect,
         * because a lost stop is a sound that keeps playing. The `set_*` calls below are not
         * retried - losing one is inaudible, and the next value supersedes it anyway.
         */
        void stop(voice_id voice);

        /** @brief Ends every voice at the next block. Retried by @ref collect, like @ref stop. */
        void stop_all();

        /** @brief Sets a voice's linear gain, reached by a ramp over the next block. */
        void set_gain(voice_id voice, float gain);

        /** @brief Sets a voice's stereo position, reached by a ramp over the next block. */
        void set_pan(voice_id voice, float pan);

        /** @brief Sets a voice's playback rate multiplier, from the next block. */
        void set_speed(voice_id voice, float speed);

        /** @brief Scales everything the mixer produces. Ramped over the next block, like a voice. */
        void set_master_gain(float gain);

        /**
         * @brief Whether @p voice is still playing.
         * @details Answered from an atomic the render thread publishes, so it is current as of the
         * last block rather than as of the last command. A voice that was started but has not been
         * rendered yet reads as playing, which is what a caller means by the question.
         */
        [[nodiscard]] bool is_playing(voice_id voice) const noexcept;

        /** @brief How many voices are producing audio, as of the last block. */
        [[nodiscard]] std::uint32_t active_voices() const noexcept;

        // ------------------------------------------------------------------------------------------------------------
        // Housekeeping - game thread
        // ------------------------------------------------------------------------------------------------------------

        /**
         * @brief Reclaims finished voices, frees released sounds, and re-posts what a full ring
         * refused. Call once a frame.
         * @return How many sounds were freed.
         * @details The counterpart to `stream::pump()`, and for the same reason: the work has to
         * happen on a thread that is allowed to call the allocator. A mixer that is never collected
         * keeps playing correctly, but its voice slots and released buffers are not reused, and a
         * @ref stop that did not fit in the ring never arrives.
         */
        std::size_t collect();

        /** @brief A snapshot of the counters. Safe from any thread. */
        [[nodiscard]] mixer_stats stats() const noexcept;

        // ------------------------------------------------------------------------------------------------------------
        // Rendering - render thread
        // ------------------------------------------------------------------------------------------------------------

        /**
         * @brief Applies pending commands, then sums every voice into @p block. Render thread.
         * @param block The block to fill. Its output is overwritten, not added to, so the mixer
         * does not need it silenced first.
         * @details Allocates nothing, locks nothing and cannot throw, so it may be called directly
         * from a driver callback.
         */
        void render(render_block &block) noexcept;

        /**
         * @brief The same as @ref render, so that a mixer *is* a @ref renderable.
         * @details Lets a named mixer be handed to `stream::open` directly, without a lambda that
         * does nothing but forward.
         */
        void operator()(render_block &block) noexcept { render(block); }

    private:
        struct impl;
        std::unique_ptr<impl> impl_;
    };

    static_assert(renderable<mixer>, "a mixer must be usable as a stream's renderer");

} // namespace catalyst::audio
