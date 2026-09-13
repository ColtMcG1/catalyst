/**
 * @file calibration.cpp
 * @brief Implementation of the dead-zone calibrator.
 * License: MIT (see LICENSE).
 */

#include <catalyst/input/calibration.hpp>

#include <catalyst/input/context.hpp>

#include <algorithm>
#include <cmath>

namespace catalyst::input
{
    namespace
    {
        [[nodiscard]] double clamp_threshold(double t) noexcept
        {
            return std::clamp(t, 0.0, 0.999);
        }

        /** @brief NaN and infinity out of a misbehaving driver would poison every peak from here on. */
        [[nodiscard]] double finite_or_zero(double v) noexcept
        {
            return std::isfinite(v) ? v : 0.0;
        }

        [[nodiscard]] double stick_magnitude(const gamepad_state &s, gamepad_axis x, gamepad_axis y) noexcept
        {
            const double vx = finite_or_zero(s.axis(x));
            const double vy = finite_or_zero(s.axis(y));
            return std::sqrt(vx * vx + vy * vy);
        }

        [[nodiscard]] double trigger_magnitude(const gamepad_state &s, gamepad_axis a) noexcept
        {
            return std::fabs(finite_or_zero(s.axis(a)));
        }
    } // namespace

    gamepad_deadzone deadzone_for_noise(double peak_stick, double peak_trigger,
                                        const gamepad_deadzone_calibration_options &opts) noexcept
    {
        const double headroom = std::max(finite_or_zero(opts.headroom), 0.0);
        const double margin = std::max(finite_or_zero(opts.margin), 0.0);

        const auto threshold = [&](double peak) noexcept
        {
            const double p = std::max(finite_or_zero(peak), 0.0);
            return clamp_threshold(p * (1.0 + headroom) + margin);
        };

        gamepad_deadzone dz;
        dz.stick = threshold(peak_stick);
        dz.trigger = threshold(peak_trigger);
        return dz;
    }

    gamepad_deadzone_calibrator::gamepad_deadzone_calibrator(std::uint32_t slot,
                                                             const gamepad_deadzone_calibration_options &opts) noexcept
        : m_slot(slot), m_options(opts)
    {
    }

    void gamepad_deadzone_calibrator::start() noexcept
    {
        start(clock::now());
    }

    void gamepad_deadzone_calibrator::start(clock::time_point now) noexcept
    {
        m_status = gamepad_calibration_status::sampling;
        m_restarts = 0;
        restart_window(now);
    }

    void gamepad_deadzone_calibrator::cancel() noexcept
    {
        m_status = gamepad_calibration_status::idle;
    }

    void gamepad_deadzone_calibrator::restart_window(clock::time_point now) noexcept
    {
        m_window_start = now;
        m_last_sample = now;
        m_peak_stick = 0.0;
        m_peak_trigger = 0.0;
    }

    bool gamepad_deadzone_calibrator::update(const context &sys) noexcept
    {
        return sample(sys.raw_gamepad(m_slot), clock::now());
    }

    bool gamepad_deadzone_calibrator::sample(const gamepad_state &raw, clock::time_point now) noexcept
    {
        if (!is_sampling())
            return is_complete();

        if (!raw.connected)
        {
            m_status = gamepad_calibration_status::disconnected;
            return false;
        }

        m_last_sample = now;

        const double stick = std::max(stick_magnitude(raw, gamepad_axis::left_x, gamepad_axis::left_y),
                                      stick_magnitude(raw, gamepad_axis::right_x, gamepad_axis::right_y));
        const double trigger = std::max(trigger_magnitude(raw, gamepad_axis::left_trigger),
                                        trigger_magnitude(raw, gamepad_axis::right_trigger));

        // Anything that looks deliberate means the controller is not at rest: throw the window away and wait for the
        // user to let go, rather than baking their thumb into the noise floor.
        const bool axis_check = m_options.disturbance_threshold < 1.0;
        const bool disturbed = raw.buttons != gamepad_buttons::none ||
                               (axis_check && std::max(stick, trigger) > m_options.disturbance_threshold);
        if (disturbed)
        {
            ++m_restarts;
            restart_window(now);
            return false;
        }

        m_peak_stick = std::max(m_peak_stick, stick);
        m_peak_trigger = std::max(m_peak_trigger, trigger);

        if (now - m_window_start >= m_options.duration)
        {
            m_status = gamepad_calibration_status::complete;
            return true;
        }
        return false;
    }

    double gamepad_deadzone_calibrator::progress() const noexcept
    {
        switch (m_status)
        {
        case gamepad_calibration_status::idle:
            return 0.0;
        case gamepad_calibration_status::complete:
            return 1.0;
        case gamepad_calibration_status::sampling:
        case gamepad_calibration_status::disconnected:
            break;
        }

        using seconds = std::chrono::duration<double>;
        const double total = std::chrono::duration_cast<seconds>(m_options.duration).count();
        if (total <= 0.0)
            return 1.0;
        const double elapsed = std::chrono::duration_cast<seconds>(m_last_sample - m_window_start).count();
        return std::clamp(elapsed / total, 0.0, 1.0);
    }

    gamepad_deadzone gamepad_deadzone_calibrator::result() const noexcept
    {
        return deadzone_for_noise(m_peak_stick, m_peak_trigger, m_options);
    }

    bool gamepad_deadzone_calibrator::apply(context &sys) const noexcept
    {
        if (!is_complete())
            return false;
        sys.set_deadzone(result());
        return true;
    }

} // namespace catalyst::input
