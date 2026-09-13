/**
 * @file types.hpp
 * @brief The vocabulary every other audio header is written in: the sample type, the clock, frame
 * and channel counts, and the conversions between frames, seconds and decibels.
 * @details Three choices are made here once, so nothing downstream has to restate them.
 *
 * **Samples are `float`.** Every buffer the module hands out or takes in is 32-bit float,
 * interleaved, nominally in [-1, 1]; backends clamp on conversion to fixed-point device formats.
 * This is deliberate rather than a default. Every supported device format is float32 or narrower,
 * so a `double` pipeline would be truncated at the driver boundary while costing twice the memory
 * bandwidth and half the SIMD width. Use `double` inside a callback for phase accumulators, filter
 * state and resampler positions - the places where error compounds - and `frame_count` for absolute
 * time, never a float counter: float32 stops representing consecutive integers at 2^24 frames,
 * which is under six minutes at 48 kHz.
 *
 * **Time is frames, and durations are `std::chrono`.** A frame index is exact and a wall-clock
 * duration is not, so positions and lengths are `frame_count` and only latencies and measured
 * costs - things that were never exact - are `seconds`. The old surface reported latency as a bare
 * `double ..._seconds`, which read the same at a call site whether the caller meant seconds or
 * milliseconds; `seconds` does not.
 *
 * **Channels are a layout, not just a number.** A count says how much memory a block needs; a
 * layout says which speaker each channel is. Tier 1 only ever needs the count, but the count alone
 * cannot survive into a mixer, so the layout is defined here rather than retrofitted later.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <format>
#include <string_view>

namespace catalyst::audio
{

    // ------------------------------------------------------------------------------------------------------------------
    // Samples and counts
    // ------------------------------------------------------------------------------------------------------------------

    /** @brief The sample type of every buffer in the module. See the file comment for why. */
    using sample = float;

    static_assert(sizeof(sample) == 4, "catalyst::audio::sample must be 32-bit IEEE 754 float");

    /**
     * @brief A count of, or an index into, audio frames.
     * @details One frame is one sample per channel. 64-bit because this is the module's clock: at
     * 48 kHz it overflows after about twelve million years, and at 32 bits it would overflow after
     * a day.
     */
    using frame_count = std::uint64_t;

    /** @brief A number of channels. 32-bit because no device has more than a few dozen. */
    using channel_count = std::uint32_t;

    /**
     * @brief Frames per second.
     * @details Spelled with the suffix, unlike its neighbours, so that a struct can carry a member
     * called `sample_rate` - which is what every call site wants to write - without the member name
     * shadowing the type inside its own class.
     */
    using sample_rate_t = std::uint32_t;

    /** @brief The clock audio events are stamped with. Steady, because audio timing is intervals. */
    using audio_clock = std::chrono::steady_clock;

    /** @brief A point on @ref audio_clock. */
    using audio_time = audio_clock::time_point;

    /**
     * @brief A duration in seconds, held as a `double`.
     * @details Used for latencies and measured callback costs - quantities that are approximate by
     * nature. Converts implicitly to and from any other `std::chrono` duration, so
     * `info.output_latency` prints as milliseconds with a `duration_cast` and no arithmetic.
     */
    using seconds = std::chrono::duration<double>;

    // ------------------------------------------------------------------------------------------------------------------
    // Direction
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @enum stream_direction
     * @brief Which half, or both halves, of a device a stream opens.
     */
    enum class stream_direction : std::uint8_t
    {
        /** @brief Playback only. The stream writes; `render_block::input` is empty. */
        output = 0,
        /** @brief Capture only. The stream reads; `render_block::output` is empty. */
        input,
        /** @brief Both, in one callback, sharing one clock. */
        duplex,
    };

    /** @brief The display name of a direction: "output", "input" or "duplex". */
    [[nodiscard]] constexpr std::string_view name(stream_direction direction) noexcept
    {
        switch (direction)
        {
        case stream_direction::output:
            return "output";
        case stream_direction::input:
            return "input";
        case stream_direction::duplex:
            return "duplex";
        }
        return "unknown";
    }

    /** @brief True for the directions that produce audio. */
    [[nodiscard]] constexpr bool has_output(stream_direction direction) noexcept
    {
        return direction == stream_direction::output || direction == stream_direction::duplex;
    }

    /** @brief True for the directions that consume audio. */
    [[nodiscard]] constexpr bool has_input(stream_direction direction) noexcept
    {
        return direction == stream_direction::input || direction == stream_direction::duplex;
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Channel layout
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @enum channel_layout
     * @brief Which speaker each channel of an interleaved buffer drives.
     * @details Interleaving order is the WAVE_FORMAT_EXTENSIBLE order, which is what WASAPI, ASIO
     * and every WAV file already use, so no layout in this enum needs a permutation on the way to a
     * device. `unspecified` is the honest answer for a device that reports a channel count and
     * nothing else, and for any count this enum does not name - a 32-channel interface, say. Code
     * that only needs to size a buffer should use @ref channel_count rather than matching on this.
     */
    enum class channel_layout : std::uint8_t
    {
        /** @brief The device told us how many channels, not what they are. */
        unspecified = 0,
        /** @brief One channel. */
        mono,
        /** @brief L, R. */
        stereo,
        /** @brief FL, FR, BL, BR. */
        quad,
        /** @brief FL, FR, FC, LFE, BL, BR. */
        surround_5_1,
        /** @brief FL, FR, FC, LFE, BL, BR, SL, SR. */
        surround_7_1,
    };

    /** @brief How many channels a layout has. Zero for @ref channel_layout::unspecified. */
    [[nodiscard]] constexpr channel_count channels_in(channel_layout layout) noexcept
    {
        switch (layout)
        {
        case channel_layout::unspecified:
            return 0;
        case channel_layout::mono:
            return 1;
        case channel_layout::stereo:
            return 2;
        case channel_layout::quad:
            return 4;
        case channel_layout::surround_5_1:
            return 6;
        case channel_layout::surround_7_1:
            return 8;
        }
        return 0;
    }

    /**
     * @brief The layout conventionally meant by a channel count.
     * @param count The number of channels.
     * @return The matching layout, or @ref channel_layout::unspecified for a count this enum does
     * not name. Note that 6 and 8 are guesses - a device with eight channels need not be 7.1 - so
     * this is for display and defaults, not for routing.
     */
    [[nodiscard]] constexpr channel_layout layout_for(channel_count count) noexcept
    {
        switch (count)
        {
        case 1:
            return channel_layout::mono;
        case 2:
            return channel_layout::stereo;
        case 4:
            return channel_layout::quad;
        case 6:
            return channel_layout::surround_5_1;
        case 8:
            return channel_layout::surround_7_1;
        default:
            return channel_layout::unspecified;
        }
    }

    /** @brief The display name of a layout: "stereo", "5.1", ... */
    [[nodiscard]] constexpr std::string_view name(channel_layout layout) noexcept
    {
        switch (layout)
        {
        case channel_layout::unspecified:
            return "unspecified";
        case channel_layout::mono:
            return "mono";
        case channel_layout::stereo:
            return "stereo";
        case channel_layout::quad:
            return "quad";
        case channel_layout::surround_5_1:
            return "5.1";
        case channel_layout::surround_7_1:
            return "7.1";
        }
        return "unknown";
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Conversions
    // ------------------------------------------------------------------------------------------------------------------

    /**
     * @brief How long @p frames last at @p rate.
     * @return Zero when @p rate is zero, rather than a division by it.
     */
    [[nodiscard]] constexpr seconds frames_to_time(frame_count frames, sample_rate_t rate) noexcept
    {
        if (rate == 0)
            return seconds{0.0};
        return seconds{static_cast<double>(frames) / static_cast<double>(rate)};
    }

    /**
     * @brief How many whole frames fit in @p time at @p rate.
     * @details Truncates. A caller sizing a buffer wants to round up itself; a caller converting a
     * position wants the truncation.
     */
    [[nodiscard]] constexpr frame_count time_to_frames(seconds time, sample_rate_t rate) noexcept
    {
        if (rate == 0 || time.count() <= 0.0)
            return 0;
        return static_cast<frame_count>(time.count() * static_cast<double>(rate));
    }

    /**
     * @brief The linear gain a level in decibels means, so `db_to_gain(-6.0f)` is about 0.5.
     * @param db Decibels relative to full scale. Negative attenuates.
     * @return The multiplier to apply to a sample. Exactly zero at or below -120 dB, so a fade to
     * silence reaches silence instead of approaching it.
     */
    [[nodiscard]] inline sample db_to_gain(float db) noexcept
    {
        if (db <= -120.0f)
            return 0.0f;
        return static_cast<sample>(std::pow(10.0f, db * 0.05f));
    }

    /**
     * @brief The level in decibels a linear gain means. The inverse of @ref db_to_gain.
     * @return -120 for a gain of zero or less, rather than negative infinity.
     */
    [[nodiscard]] inline float gain_to_db(sample gain) noexcept
    {
        if (gain <= 0.0f)
            return -120.0f;
        return 20.0f * std::log10(static_cast<float>(gain));
    }

} // namespace catalyst::audio

/** @brief Formats a direction as its name, so `log::info("{} stream", direction)` works. */
template <>
struct std::formatter<catalyst::audio::stream_direction> : std::formatter<std::string_view>
{
    template <typename Context>
    auto format(catalyst::audio::stream_direction direction, Context &ctx) const
    {
        return std::formatter<std::string_view>::format(catalyst::audio::name(direction), ctx);
    }
};

/** @brief Formats a channel layout as its name, so `log::info("{}", layout)` works. */
template <>
struct std::formatter<catalyst::audio::channel_layout> : std::formatter<std::string_view>
{
    template <typename Context>
    auto format(catalyst::audio::channel_layout layout, Context &ctx) const
    {
        return std::formatter<std::string_view>::format(catalyst::audio::name(layout), ctx);
    }
};
