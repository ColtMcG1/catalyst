/**
 * @file events.hpp
 * @brief What the audio module tells the rest of the program about, published on a
 * `catalyst::events::bus`.
 * @details The old surface reported device topology through a `device_change_callback` and a second
 * `void *user`, invoked on a platform-owned notification thread, with a comment telling the caller
 * to post it to a queue of their own and act on it somewhere safe. That comment is now the
 * implementation: the stream holds the queue, and @ref stream::pump drains it onto the bus from
 * whichever thread the caller drives the stream from.
 *
 * That is what makes these events ordinary. A listener added with `bus.add_listener<...>` runs on
 * the caller's thread, inside `pump()`, and may do anything a normal function may do - open a
 * device, format a log line, touch a widget. Nothing here is delivered from the real-time thread,
 * and nothing here needs the caller to know which thread a driver happened to use.
 *
 * The cost of that choice is that events are as timely as `pump()` is frequent. A program that
 * calls it once a frame learns about an unplugged headset within a frame, which is soon enough for
 * every use these events have; a program that never calls it learns nothing, and its queue is
 * bounded and drops the oldest rather than growing.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/audio/backend.hpp>
#include <catalyst/audio/error.hpp>
#include <catalyst/audio/types.hpp>
#include <catalyst/events/tag.hpp>

#include <string>

namespace catalyst::audio
{

    /**
     * @namespace catalyst::audio::tags
     * @brief Static event tags for this module, so the bus dispatches on a constant rather than on
     * `typeid`.
     * @details The audio module owns the block 0x0002'0000 - 0x0002'FFFF, next after the input
     * module's. Values are never reused or reordered: a recorded session or a serialised filter may
     * name one.
     */
    namespace tags
    {
        /** @brief First tag in the audio module's block. */
        inline constexpr events::event_type_t audio_base = 0x0002'0000u;

        inline constexpr events::event_type_t device_added = audio_base + 0x00;
        inline constexpr events::event_type_t device_removed = audio_base + 0x01;
        inline constexpr events::event_type_t default_device_changed = audio_base + 0x02;
        inline constexpr events::event_type_t device_lost = audio_base + 0x03;

        inline constexpr events::event_type_t stream_started = audio_base + 0x10;
        inline constexpr events::event_type_t stream_stopped = audio_base + 0x11;
        inline constexpr events::event_type_t stream_failed = audio_base + 0x12;
        inline constexpr events::event_type_t xrun = audio_base + 0x13;
    } // namespace tags

    /**
     * @struct audio_event
     * @brief The two fields every audio event carries, inherited rather than repeated eight times.
     * @tparam Tag The event's static tag; see @ref catalyst::audio::tags.
     */
    template <events::event_type_t Tag>
    struct audio_event : events::tagged<Tag>
    {
        /** @brief The backend that observed it. Resolved, never @ref backend_kind::automatic. */
        backend_kind backend = backend_kind::automatic;

        /** @brief When it happened, stamped when the backend observed it - not when `pump()` got
         * around to publishing it. */
        audio_time time{};
    };

    /**
     * @struct device_added_event
     * @brief An endpoint appeared. Re-enumerate to see it: this carries the id, not the whole
     * @ref device_info, because building one costs a platform call that most listeners do not want.
     */
    struct device_added_event : audio_event<tags::device_added>
    {
        /** @brief The new endpoint's @ref device_info::id. */
        std::string device_id;
    };

    /**
     * @struct device_removed_event
     * @brief An endpoint went away. Not necessarily the one this stream is on - see
     * @ref device_lost_event for that.
     */
    struct device_removed_event : audio_event<tags::device_removed>
    {
        /** @brief The departed endpoint's @ref device_info::id. */
        std::string device_id;
    };

    /**
     * @struct default_device_changed_event
     * @brief The system default endpoint for a direction changed - the user selected a different
     * one, or plugged in something that takes precedence.
     * @details A stream opened with @ref device_selector::system_default does **not** follow this
     * on its own: moving a running stream to another device is a policy decision, not a mechanism,
     * and a game and a DAW want opposite answers. Reopen the stream here if following is what you
     * want.
     */
    struct default_device_changed_event : audio_event<tags::default_device_changed>
    {
        /** @brief The new default endpoint's @ref device_info::id. Empty if there is none. */
        std::string device_id;

        /** @brief Which direction's default changed. */
        stream_direction direction = stream_direction::output;
    };

    /**
     * @struct device_lost_event
     * @brief The device backing this stream went away. The stream is dead and will render nothing
     * further; open a new one.
     */
    struct device_lost_event : audio_event<tags::device_lost>
    {
        /** @brief The lost endpoint's @ref device_info::id. */
        std::string device_id;
    };

    /** @struct stream_started_event
     *  @brief A stream began delivering blocks. Published on the first @ref stream::pump after
     *  @ref stream::start succeeds. */
    struct stream_started_event : audio_event<tags::stream_started>
    {
        /** @brief The negotiated rate. */
        sample_rate_t sample_rate = 0;
        /** @brief Channels being rendered. */
        channel_count output_channels = 0;
        /** @brief Channels being captured. */
        channel_count input_channels = 0;
    };

    /** @struct stream_stopped_event
     *  @brief A stream stopped delivering blocks, whether by @ref stream::stop or by failing. */
    struct stream_stopped_event : audio_event<tags::stream_stopped>
    {
        /** @brief Frames delivered over the whole run. */
        frame_count frames_rendered = 0;
    };

    /**
     * @struct stream_failed_event
     * @brief The stream stopped by itself because something went wrong on the driver thread.
     * @details The failure is reported here rather than returned, because there is no call in
     * progress to return it from - the render thread found it. A @ref stream_stopped_event follows.
     */
    struct stream_failed_event : audio_event<tags::stream_failed>
    {
        /** @brief What went wrong. */
        audio::error error{};
    };

    /**
     * @struct xrun_event
     * @brief Audio was dropped or not produced in time. Any of these is audible.
     * @details Coalesced: one event per `pump()` carrying however many occurred, rather than one
     * per glitch, because a stream that is glitching is glitching continuously and a listener that
     * logs each one makes it worse.
     */
    struct xrun_event : audio_event<tags::xrun>
    {
        /** @brief How many occurred since the last one of these. */
        std::uint64_t count = 0;

        /** @brief The running total, matching @ref stream_stats::xruns. */
        std::uint64_t total = 0;
    };

} // namespace catalyst::audio
