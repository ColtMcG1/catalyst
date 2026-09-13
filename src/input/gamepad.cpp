/**
 * @file gamepad.cpp
 * @brief The dead-zone primitives. Everything else that used to live here is now in context.cpp (polling) or
 * calibration.cpp (learning a threshold).
 * License: MIT (see LICENSE).
 */

#include <catalyst/input/gamepad.hpp>

#include <algorithm>
#include <cmath>

namespace catalyst::input
{
    namespace
    {
        /** @brief A threshold of 1 would divide by zero on the rescale, so the usable range stops just short. */
        [[nodiscard]] double clamp_threshold(double t) noexcept
        {
            return std::clamp(t, 0.0, 0.999);
        }
    } // namespace

    double apply_deadzone(double value, double threshold) noexcept
    {
        const double t = clamp_threshold(threshold);
        const double magnitude = std::fabs(value);
        if (magnitude <= t)
            return 0.0;

        // Rescale the remainder over the full range, so the output leaves the dead zone at 0 and still reaches 1 -
        // rather than jumping straight to the threshold value the moment the stick moves.
        const double scaled = std::min((magnitude - t) / (1.0 - t), 1.0);
        return std::copysign(scaled, value);
    }

    void apply_radial_deadzone(double &x, double &y, double threshold) noexcept
    {
        const double t = clamp_threshold(threshold);
        const double magnitude = std::sqrt(x * x + y * y);
        if (magnitude <= t)
        {
            x = 0.0;
            y = 0.0;
            return;
        }

        const double scaled = std::min((magnitude - t) / (1.0 - t), 1.0);
        const double k = scaled / magnitude;
        x *= k;
        y *= k;
    }

} // namespace catalyst::input
