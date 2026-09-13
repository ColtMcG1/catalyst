/**
 * @file sound.hpp
 * @brief Decoded audio held in memory, and the handle a mixer knows it by.
 * @details A @ref sound_buffer is the simplest thing in the module: samples, a rate, a channel
 * count, and nothing else. It is deliberately not a resource, not reference-counted and not aware
 * of the mixer. Everything that makes a sound *play* - where it is up to, how loud, how it is
 * panned - belongs to a voice, because two voices playing the same sound must not share any of it.
 *
 * The buffer is interleaved 32-bit float at whatever rate it was decoded at, which need not be the
 * rate the device is running. A voice reconciles the two when it plays; see mixer.hpp.
 *
 * Decoders arrive in a later part of Tier 2 and will produce exactly this type, so nothing that
 * consumes a `sound_buffer` today has to change when a WAV file becomes a way to get one.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/audio/types.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

namespace catalyst::audio
{

    /**
     * @struct sound_id
     * @brief A handle to a sound a mixer holds: a slot index plus the generation that occupied it.
     * @details A generation of 0 is the null handle. The generation moves every time a slot is
     * reused, so a handle kept across `mixer::release_sound` is *detected* rather than resolving to
     * whatever was loaded into the slot afterwards - the same rule as `input::device_id` and
     * `ui::node`.
     */
    struct sound_id
    {
        std::uint32_t index{0};
        std::uint32_t generation{0};

        [[nodiscard]] constexpr bool valid() const noexcept { return generation != 0; }
        [[nodiscard]] constexpr explicit operator bool() const noexcept { return valid(); }

        [[nodiscard]] friend constexpr bool operator==(sound_id, sound_id) noexcept = default;
    };

    /** @brief The handle that refers to no sound. Every lookup rejects it. */
    inline constexpr sound_id no_sound{};

    /**
     * @class sound_buffer
     * @brief Interleaved float samples with the rate and channel count needed to interpret them.
     * @details Immutable once built. A mixer reads it from the render thread while the game thread
     * may still hold a handle to it, and immutability is what makes that safe without a lock: there
     * is no state to observe half-changed.
     *
     * Samples are truncated to a whole number of frames on construction, so `frames()` and
     * `samples().size()` can never disagree - a partial trailing frame is the sort of thing that
     * turns into a one-sample click at the end of every playback.
     */
    class sound_buffer
    {
    public:
        /** @brief An empty buffer. Playing it ends immediately rather than failing. */
        sound_buffer() noexcept = default;

        /**
         * @brief Takes ownership of @p samples and interprets them at @p rate.
         * @param samples Interleaved audio, nominally in [-1, 1]. Any partial trailing frame is
         * dropped.
         * @param rate The rate the samples were produced at, which need not be the device's.
         * @param channels Channels per frame. Zero is treated as one, so the buffer is never a
         * shape that cannot be indexed.
         */
        sound_buffer(std::vector<sample> samples, sample_rate_t rate, channel_count channels)
            : samples_(std::move(samples)),
              sample_rate_(rate),
              channels_(channels == 0 ? 1 : channels)
        {
            samples_.resize((samples_.size() / channels_) * channels_);
        }

        /** @brief The interleaved samples. */
        [[nodiscard]] std::span<const sample> samples() const noexcept { return samples_; }

        /** @brief Frames held, which is `samples().size() / channels()` exactly. */
        [[nodiscard]] frame_count frames() const noexcept
        {
            return static_cast<frame_count>(samples_.size() / channels_);
        }

        /** @brief Channels per frame. Never zero. */
        [[nodiscard]] channel_count channels() const noexcept { return channels_; }

        /** @brief The rate the samples were produced at. */
        [[nodiscard]] sample_rate_t sample_rate() const noexcept { return sample_rate_; }

        /** @brief How long the buffer lasts at its own rate. */
        [[nodiscard]] seconds duration() const noexcept
        {
            return frames_to_time(frames(), sample_rate_);
        }

        /** @brief True when there is nothing to play. */
        [[nodiscard]] bool empty() const noexcept { return samples_.empty(); }

        /**
         * @brief One frame's samples, one per channel.
         * @param frame Frame index. Out of range returns an empty span rather than reading past
         * the end.
         */
        [[nodiscard]] std::span<const sample> frame(frame_count frame) const noexcept
        {
            if (frame >= frames())
                return {};

            const std::size_t offset = static_cast<std::size_t>(frame) * channels_;
            return std::span<const sample>(samples_).subspan(offset, channels_);
        }

    private:
        std::vector<sample> samples_;
        sample_rate_t sample_rate_ = 48000;
        channel_count channels_ = 1;
    };

} // namespace catalyst::audio
