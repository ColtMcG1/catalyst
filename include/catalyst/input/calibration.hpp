/**
 * @file calibration.hpp
 * @brief Learning a gamepad's dead zone from a controller that is left alone.
 * @details Split out of gamepad.hpp because it is a tool built *on* the gamepad API rather than part of it - the
 * vocabulary header should not carry a state machine, and the old one also carried a blocking version that slept in a
 * loop, which a library core has no business doing.
 *
 * The idea is simple: watch a resting controller, take the largest stick magnitude and trigger value it reports as its
 * noise floor, and put the threshold just above them. That gives every player a dead zone matched to their own
 * hardware instead of one tuned for the worst pad the developer happened to own.
 *
 * The calibrator is fed one frame at a time, so a game runs its normal loop and draws a progress bar while it works:
 *
 *     gamepad_deadzone_calibrator cal(0);
 *     cal.start();                              // the user picked "calibrate"
 *     ...
 *     in.poll();                                // as usual
 *     if (cal.update(in))                       // true once it has rested long enough
 *         in.set_deadzone(cal.result());
 *     draw_progress(cal.progress());
 *
 * Touching anything restarts the window, and `restarts()` is the cue to tell the user to let go.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/input/gamepad.hpp>

#include <chrono>
#include <cstdint>

namespace catalyst::input
{
    class context;

    /**
     * @struct gamepad_deadzone_calibration_options
     * @brief Tuning for a calibration run. The threshold ends up at `peak * (1 + headroom) + margin`.
     */
    struct gamepad_deadzone_calibration_options
    {
        /** @brief How long the controller must rest undisturbed before the calibration completes. */
        std::chrono::milliseconds duration{1000};
        /** @brief Relative headroom above the noise peak. 0.25 puts the threshold 25% above the largest value seen. */
        double headroom{0.25};
        /** @brief Absolute margin on top of the headroom, and the floor for a controller with no measurable noise. */
        double margin{0.02};
        /**
         * @brief A raw magnitude above this, or any button being held, means the user is touching the controller: the
         * peaks are thrown away and the rest window starts over. A value of 1 or more turns the axis check off, though
         * buttons still restart the window.
         */
        double disturbance_threshold{0.5};
    };

    /**
     * @enum gamepad_calibration_status
     * @brief Where a calibrator is in its life.
     */
    enum class gamepad_calibration_status : std::uint8_t
    {
        /** @brief Not started, or cancelled. */
        idle,
        /** @brief Watching the controller; keep feeding it. */
        sampling,
        /** @brief The rest window elapsed. result() is meaningful. */
        complete,
        /** @brief The slot emptied while sampling. start() again once it comes back. */
        disconnected
    };

    /**
     * @fn deadzone_for_noise
     * @brief The policy a calibration applies to a pair of noise peaks, exposed so it can be used on peaks measured
     * some other way: `peak * (1 + headroom) + margin`, clamped to a usable range.
     */
    [[nodiscard]] gamepad_deadzone deadzone_for_noise(double peak_stick, double peak_trigger,
                                                      const gamepad_deadzone_calibration_options &opts = {}) noexcept;

    /**
     * @class gamepad_deadzone_calibrator
     * @brief Learns a dead zone one frame at a time.
     * @note `sample()` takes a state from anywhere, which is how the whole thing is testable without a controller;
     * `update()` is `sample()` fed from a context's last poll.
     */
    class gamepad_deadzone_calibrator
    {
    public:
        using clock = input_clock;

        gamepad_deadzone_calibrator() noexcept = default;
        explicit gamepad_deadzone_calibrator(std::uint32_t slot,
                                             const gamepad_deadzone_calibration_options &opts = {}) noexcept;

        /** @brief Begins, or begins again, at clock::now(). Any previous result is discarded. */
        void start() noexcept;
        /** @brief Begins with an explicit start time, for callers supplying their own timestamps to sample(). */
        void start(clock::time_point now) noexcept;
        /** @brief Stops and returns to gamepad_calibration_status::idle. */
        void cancel() noexcept;

        /**
         * @brief Feeds this calibrator's slot from @p sys, using the pre-dead-zone axes.
         * @return True once the calibration is complete, and on every call after that until start() or cancel().
         */
        bool update(const context &sys) noexcept;

        /**
         * @brief Feeds one raw, pre-dead-zone state observed at @p now. Does nothing unless sampling.
         * @return True once the calibration is complete.
         */
        bool sample(const gamepad_state &raw, clock::time_point now) noexcept;

        [[nodiscard]] gamepad_calibration_status status() const noexcept { return m_status; }
        [[nodiscard]] bool is_sampling() const noexcept { return m_status == gamepad_calibration_status::sampling; }
        [[nodiscard]] bool is_complete() const noexcept { return m_status == gamepad_calibration_status::complete; }

        /** @brief How much of the current rest window has elapsed, over [0, 1]. 1 once complete, 0 when idle. */
        [[nodiscard]] double progress() const noexcept;

        /**
         * @brief The dead zone the peaks so far imply. Meaningful once complete; before that it reflects a partial
         * window and may still grow.
         */
        [[nodiscard]] gamepad_deadzone result() const noexcept;

        /** @brief Largest radial magnitude seen on either stick in the current window. */
        [[nodiscard]] double peak_stick_noise() const noexcept { return m_peak_stick; }
        /** @brief Largest value seen on either trigger in the current window. */
        [[nodiscard]] double peak_trigger_noise() const noexcept { return m_peak_trigger; }
        /** @brief How many times the window restarted because the controller was disturbed. Ask the user to let go. */
        [[nodiscard]] unsigned restarts() const noexcept { return m_restarts; }

        [[nodiscard]] std::uint32_t slot() const noexcept { return m_slot; }
        [[nodiscard]] const gamepad_deadzone_calibration_options &options() const noexcept { return m_options; }

        /**
         * @brief Installs result() on @p sys, if the calibration is complete.
         * @return False, changing nothing, if it is not.
         */
        bool apply(context &sys) const noexcept;

    private:
        void restart_window(clock::time_point now) noexcept;

        std::uint32_t m_slot{0};
        gamepad_deadzone_calibration_options m_options{};
        gamepad_calibration_status m_status{gamepad_calibration_status::idle};
        clock::time_point m_window_start{};
        clock::time_point m_last_sample{};
        double m_peak_stick{0.0};
        double m_peak_trigger{0.0};
        unsigned m_restarts{0};
    };

} // namespace catalyst::input
