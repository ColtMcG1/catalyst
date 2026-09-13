/**
 * @file measurement.hpp
 * @brief Defines types and functions for representing and resolving CSS-like measurements.
 * @details This includes the `calc_measure` template for representing linear combinations of units, and the `length` type for common CSS length units. The `resolve_or` function converts these measurements to pixels using a provided context.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <type_traits>

/**
 * @namespace catalyst::ui
 * @brief The catalyst::ui namespace contains types and functions related to user interface measurements and layout. This includes representations of CSS-like measurements (e.g. lengths with various units), as well as functions for resolving these measurements to pixel values based on a given context (e.g. DPI scale, parent dimensions). By organizing UI-related measurement functionality within this namespace, we can provide a clear structure for users of the library to access and work with UI measurements in a consistent way.
 * @details The measurement types and functions in this namespace are designed to be flexible and efficient, allowing developers to express complex layout requirements using familiar CSS-like units and calculations. The `calc_measure` template provides a way to represent linear combinations of units, while the `length` type defines common CSS length units. The `resolve_or` function can be used to convert these measurements to pixel values based on a provided context, making it easier to implement responsive and adaptive UI layouts.
 */
namespace catalyst::ui
{
    /**
     * @enum axis
     * @brief Represents the horizontal (x) and vertical (y) axes for measurement resolution.
     * @details This enum is used to specify which axis a measurement should be resolved against when converting to pixels. For example, when resolving a percentage unit, the axis determines whether the percentage is relative to the parent width (x axis) or parent height (y axis).
     */
    enum class axis : std::uint8_t
    {
        x,
        y,
    };

    /**
     * @struct resolve_context
     * @brief Contains contextual information needed to resolve measurements to pixel values.
     * @details This struct includes fields for DPI scaling, font sizes (for em/rem units), parent dimensions (for percentage units), and viewport dimensions (for vw/vh units). When resolving a measurement to pixels, this context provides the necessary information to perform the calculations based on the various units involved.
     */
    struct resolve_context
    {
        /**
         * @brief The DPI scaling factor, used to convert dp units to pixels.
         */
        float dpi_scale = 1.0f;

        /**
         * @brief The effective DPI (pixels-per-inch) on the x axis.
         * @details Physical units (in/cm/mm) resolve using this value.
         * If left at the default (96), resolution falls back to `dpi_scale * 96` for backward compatibility.
         */
        float dpi_x = 96.0f;
        /**
         * @brief The effective DPI (pixels-per-inch) on the y axis.
         * @details Physical units (in/cm/mm) resolve using this value.
         * If left at the default (96), resolution falls back to `dpi_scale * 96` for backward compatibility.
         */
        float dpi_y = 96.0f;

        /**
         * @brief Returns the effective DPI (pixels-per-inch) for the given axis.
         * @details Prefer setting `dpi_x`/`dpi_y` from the platform. If they are left at 96,
         * this function falls back to `dpi_scale * 96` so older code that only sets `dpi_scale`
         * continues to behave as before.
         */
        [[nodiscard]] constexpr float effective_dpi(axis a) const noexcept
        {
            const float d = (a == axis::x) ? dpi_x : dpi_y;
            if (d != 96.0f)
                return d;
            return dpi_scale * 96.0f;
        }

        /**
         * @brief Returns the effective dp scale (relative to 96 DPI) for the given axis.
         */
        [[nodiscard]] constexpr float effective_dpi_scale(axis a) const noexcept
        {
            return effective_dpi(a) / 96.0f;
        }
        /**
         * @brief The font size in pixels, used to convert em units to pixels.
         */
        float font_px = 16.0f;
        /**
         * @brief The root font size in pixels, used to convert rem units to pixels.
         */
        float root_font_px = 16.0f;

        /**
         * @brief The parent element's width in pixels, used to convert percentage units on the x axis to pixels.
         */
        float parent_width_px = 0.0f;
        /**
         * @brief The parent element's height in pixels, used to convert percentage units on the y axis to pixels.
         */
        float parent_height_px = 0.0f;

        /**
         * @brief The viewport width in pixels, used to convert vw units to pixels.
         */
        float viewport_width_px = 0.0f;
        /**
         * @brief The viewport height in pixels, used to convert vh units to pixels.
         */
        float viewport_height_px = 0.0f;
    };

    /**
     * @struct calc_measure
     * @brief Represents a linear combination of units for measurements (e.g. lengths). This allows for expressing measurements that combine multiple units (e.g. "10px + 5%") in a structured way.
     * @tparam UnitEnum An enum type representing the different units (e.g. length_unit).
     * @tparam UnitCount The number of different units represented by UnitEnum.
     * @tparam Rep The scalar type used for the coefficients of each unit (e.g. float).
     */
    template <typename UnitEnum, std::size_t UnitCount, typename Rep = float>
        requires std::is_enum_v<UnitEnum>
    struct calc_measure
    {
        /**
         * @brief The enum type representing the different units (e.g. length_unit).
         */
        using unit_type = UnitEnum;
        /**
         * @brief The scalar type used for the coefficients of each unit (e.g. float).
         */
        using rep = Rep;

        /**
         * @brief Coefficients for each unit, indexed by the underlying integer value of the UnitEnum. For example, if UnitEnum is length_unit, then coeff[0] would correspond to the coefficient for px, coeff[1] for dp, etc.
         */
        std::array<rep, UnitCount> coeff{};
        /**
         * @brief Flag indicating whether this measurement is "auto". If true, the measurement should be treated as an automatic value (e.g. "auto" in CSS) rather than a combination of units.
         */
        bool is_auto = false;
        /**
         * @fn calc_measure
         * @brief Default constructor initializes all coefficients to zero and is_auto to false.
         */
        constexpr calc_measure() noexcept = default;
        /**
         * @fn auto_value
         * @brief Constructs a calc_measure representing the "auto" value. This is a special value that indicates the measurement should be automatically determined by the layout system, rather than being a specific combination of units.
         * @return A calc_measure instance with is_auto set to true and all coefficients set to zero.
         */
        [[nodiscard]] static constexpr calc_measure auto_value() noexcept
        {
            calc_measure out{};
            out.is_auto = true;
            return out;
        }
        /**
         * @fn unit
         * @brief Constructs a calc_measure representing a single unit with a given coefficient. For example, this can be used to create a measurement of "10px" by calling unit(length_unit::px, 10.0f).
         * @param u The unit enum value representing the type of unit (e.g. length_unit::px).
         * @param v The coefficient for the unit (e.g. 10.0f for "10px").
         * @return A calc_measure instance with the specified unit coefficient set and is_auto set to false.
         */
        [[nodiscard]] static constexpr calc_measure unit(UnitEnum u, rep v) noexcept
        {
            calc_measure out{};
            out.set(u, v);
            return out;
        }
        /**
         * @fn empty
         * @brief Checks if this calc_measure has no units (i.e. all coefficients are zero) and is not auto.
         * @return true if this calc_measure has no units and is not auto, false otherwise.
         */
        [[nodiscard]] constexpr bool empty() const noexcept
        {
            if (is_auto)
                return false;
            for (std::size_t i = 0; i < UnitCount; ++i)
            {
                if (coeff[i] != rep{})
                    return false;
            }
            return true;
        }
        /**
         * @fn get
         * @brief Retrieves the coefficient for a specific unit.
         * @param u The unit enum value representing the type of unit (e.g. length_unit::px).
         * @return The coefficient for the specified unit. If the unit is not set, this will return zero.
         */
        [[nodiscard]] constexpr rep get(UnitEnum u) const noexcept
        {
            return coeff[static_cast<std::size_t>(u)];
        }
        /**
         * @fn set
         * @brief Sets the coefficient for a specific unit.
         * @param u The unit enum value representing the type of unit (e.g. length_unit::px).
         * @param v The coefficient to set for the specified unit.
         */
        constexpr void set(UnitEnum u, rep v) noexcept
        {
            coeff[static_cast<std::size_t>(u)] = v;
        }
        /**
         * @fn add
         * @brief Adds a coefficient to a specific unit. This is useful for building up a calc_measure that combines multiple units (e.g. "10px + 5%").
         * @param u The unit enum value representing the type of unit (e.g. length_unit::px).
         * @param v The coefficient to add for the specified unit.
         */
        constexpr void add(UnitEnum u, rep v) noexcept
        {
            coeff[static_cast<std::size_t>(u)] += v;
        }
        /**
         * @fn operator+
         * @brief Adds two calc_measure instances together, combining their coefficients. If either instance is auto, the result will be auto.
         * @param a The first calc_measure to add.
         * @param b The second calc_measure to add.
         * @return A new calc_measure that is the sum of a and b.
         */
        [[nodiscard]] friend constexpr calc_measure operator+(calc_measure a, const calc_measure &b) noexcept
        {
            if (a.is_auto || b.is_auto)
                return calc_measure::auto_value();
            for (std::size_t i = 0; i < UnitCount; ++i)
                a.coeff[i] += b.coeff[i];
            return a;
        }
        /**
         * @fn operator-
         * @brief Subtracts one calc_measure from another, combining their coefficients. If either instance is auto, the result will be auto.
         * @param a The calc_measure to subtract from.
         * @param b The calc_measure to subtract.
         * @return A new calc_measure that is the difference of a and b.
         */
        [[nodiscard]] friend constexpr calc_measure operator-(calc_measure a, const calc_measure &b) noexcept
        {
            if (a.is_auto || b.is_auto)
                return calc_measure::auto_value();
            for (std::size_t i = 0; i < UnitCount; ++i)
                a.coeff[i] -= b.coeff[i];
            return a;
        }
        /**
         * @fn operator-
         * @brief Negates a calc_measure, negating all of its coefficients. If the instance is auto, the result will be auto.
         * @param a The calc_measure to negate.
         * @return A new calc_measure that is the negation of a.
         */
        [[nodiscard]] friend constexpr calc_measure operator-(calc_measure a) noexcept
        {
            if (a.is_auto)
                return a;
            for (std::size_t i = 0; i < UnitCount; ++i)
                a.coeff[i] = -a.coeff[i];
            return a;
        }
        /**
         * @fn operator*
         * @brief Multiplies a calc_measure by a scalar value, scaling all of its coefficients. If the instance is auto, the result will be auto.
         * @param a The calc_measure to multiply.
         * @param s The scalar value to multiply by.
         * @return A new calc_measure that is the product of a and s.
         */
        [[nodiscard]] friend constexpr calc_measure operator*(calc_measure a, rep s) noexcept
        {
            if (a.is_auto)
                return a;
            for (std::size_t i = 0; i < UnitCount; ++i)
                a.coeff[i] *= s;
            return a;
        }
        /**
         * @fn operator*
         * @brief Multiplies a scalar value by a calc_measure, scaling all of its coefficients. If the instance is auto, the result will be auto.
         * @param s The scalar value to multiply.
         * @param a The calc_measure to multiply.
         * @return A new calc_measure that is the product of s and a.
         */
        [[nodiscard]] friend constexpr calc_measure operator*(rep s, calc_measure a) noexcept { return a * s; }
        /**
         * @fn operator/
         * @brief Divides a calc_measure by a scalar value, scaling all of its coefficients. If the instance is auto, the result will be auto.
         * @param a The calc_measure to divide.
         * @param s The scalar value to divide by.
         * @return A new calc_measure that is the quotient of a and s.
         */
        [[nodiscard]] friend constexpr calc_measure operator/(calc_measure a, rep s) noexcept
        {
            if (a.is_auto)
                return a;
            for (std::size_t i = 0; i < UnitCount; ++i)
                a.coeff[i] /= s;
            return a;
        }
        /**
         * @fn operator+=
         * @brief Adds another calc_measure to this one in-place, combining their coefficients. If either instance is auto, the result will be auto.
         * @param b The calc_measure to add to this one.
         * @return Reference to this calc_measure after the addition.
         */
        constexpr calc_measure &operator+=(const calc_measure &b) noexcept { return *this = (*this + b); }
        /**
         * @fn operator-=
         * @brief Subtracts another calc_measure from this one in-place, combining their coefficients. If either instance is auto, the result will be auto.
         * @param b The calc_measure to subtract from this one.
         * @return Reference to this calc_measure after the subtraction.
         */
        constexpr calc_measure &operator-=(const calc_measure &b) noexcept { return *this = (*this - b); }
        /**
         * @fn operator*=
         * @brief Multiplies this calc_measure by a scalar value in-place, scaling all of its coefficients. If the instance is auto, the result will be auto.
         * @param s The scalar value to multiply by.
         * @return Reference to this calc_measure after the multiplication.
         */
        constexpr calc_measure &operator*=(rep s) noexcept { return *this = (*this * s); }
        /**
         * @fn operator/=
         * @brief Divides this calc_measure by a scalar value in-place, scaling all of its coefficients. If the instance is auto, the result will be auto.
         * @param s The scalar value to divide by.
         * @return Reference to this calc_measure after the division.
         */
        constexpr calc_measure &operator/=(rep s) noexcept { return *this = (*this / s); }
    };

    /**
     * @enum length_unit
     * @brief Represents the different units that can be used for length measurements in the UI (e.g. px, dp, em, rem, percent, vw, vh).
     */
    enum class length_unit : std::uint8_t
    {
        /**
         * @brief Pixels (px) unit. This is an absolute unit representing a fixed number of pixels on the screen.
         */
        px = 0,
        /**
         * @brief Density-independent pixels (dp) unit. This is a relative unit that scales based on the DPI scaling factor in the resolve context, allowing for consistent physical sizes across different screen densities.
         */
        dp,
        /**
         * @brief Em units. This is a relative unit that scales based on the font size in the resolve context, allowing for sizes that are relative to the current font size.
         */
        em,
        /**
         * @brief Rem units. This is a relative unit that scales based on the root font size in the resolve context, allowing for sizes that are relative to the overall document's base font size.
         */
        rem,
        /**
         * @brief Percentage units. This is a relative unit that scales based on the parent dimensions in the resolve context, allowing for sizes that are relative to the parent container.
         */
        percent,
        /**
         * @brief Viewport width (vw) units. This is a relative unit that scales based on the width of the viewport in the resolve context, allowing for sizes that are relative to the viewport's width.
         */
        vw,
        /**
         * @brief Viewport height (vh) units. This is a relative unit that scales based on the height of the viewport in the resolve context, allowing for sizes that are relative to the viewport's height.
         */
        vh,
        /**
         * @brief Inches (in) unit. This is an absolute unit representing a fixed physical size of one inch on the screen. The actual pixel value will depend on the DPI scaling factor in the resolve context.
         */
        in,
        /**
         * @brief Centimeters (cm) unit. This is an absolute unit representing a fixed physical size of one centimeter on the screen. The actual pixel value will depend on the DPI scaling factor in the resolve context.
         */
        cm,
        /**
         * @brief Millimeters (mm) unit. This is an absolute unit representing a fixed physical size of one millimeter on the screen. The actual pixel value will depend on the DPI scaling factor in the resolve context.
         */
        mm,
        /**
         * @brief The count of length units. This is used to determine the size of the coefficients array in calc_measure when representing length measurements.
         */
        _count,
    };

    /**
     * @typedef length
     * @brief A type representing a length measurement that can be a combination of various units (e.g. "10px + 5%"). This is defined as a calc_measure with length_unit as the unit enum and float as the coefficient type.
     */
    using length = calc_measure<length_unit, static_cast<std::size_t>(length_unit::_count), float>;

    /**
     * @fn auto_
     * @brief Returns a length value representing an automatic size. This is a special value that indicates the measurement should be automatically determined by the layout system, rather than being a specific combination of units.
     * @return A length instance with is_auto set to true and all coefficients set to zero.
     */
    [[nodiscard]] constexpr length auto_() noexcept { return length::auto_value(); }

    /**
     * @fn px
     * @brief Constructs a length measurement in pixels.
     * @param v The length value in pixels.
     * @return A length instance representing the given value in pixels.
     */
    [[nodiscard]] constexpr length px(float v) noexcept { return length::unit(length_unit::px, v); }
    /**
     * @fn dp
     * @brief Constructs a length measurement in density-independent pixels (dp). The actual pixel value will be determined by the DPI scaling factor in the resolve context.
     * @param v The length value in dp.
     * @return A length instance representing the given value in dp.
     */
    [[nodiscard]] constexpr length dp(float v) noexcept { return length::unit(length_unit::dp, v); }
    /**
     * @fn em
     * @brief Constructs a length measurement in em units. The actual pixel value will be determined by the font size in the resolve context.
     * @param v The length value in em.
     * @return A length instance representing the given value in em.
     */
    [[nodiscard]] constexpr length em(float v) noexcept { return length::unit(length_unit::em, v); }
    /**
     * @fn rem
     * @brief Constructs a length measurement in rem units. The actual pixel value will be determined by the root font size in the resolve context.
     * @param v The length value in rem.
     * @return A length instance representing the given value in rem.
     */
    [[nodiscard]] constexpr length rem(float v) noexcept { return length::unit(length_unit::rem, v); }

    /**
     * @fn percent
     * @brief Constructs a length measurement in percentage units. The actual pixel value will be determined by the parent dimensions in the resolve context, and the axis specified when resolving.
     * @param v The length value in percent (e.g. 50.0f for "50%").
     * @return A length instance representing the given value in percent.
     */
    [[nodiscard]] constexpr length percent(float v) noexcept { return length::unit(length_unit::percent, v); }

    /**
     * @fn vw
     * @brief Constructs a length measurement in viewport width units (vw). The actual pixel value will be determined by the viewport width in the resolve context.
     * @param v The length value in vw.
     * @return A length instance representing the given value in vw.
     */
    [[nodiscard]] constexpr length vw(float v) noexcept { return length::unit(length_unit::vw, v); }

    /**
     * @fn vh
     * @brief Constructs a length measurement in viewport height units (vh). The actual pixel value will be determined by the viewport height in the resolve context.
     * @param v The length value in vh.
     * @return A length instance representing the given value in vh.
     */
    [[nodiscard]] constexpr length vh(float v) noexcept { return length::unit(length_unit::vh, v); }

    /**
     * @fn in
     * @brief Constructs a length measurement in inches. The actual pixel value will be determined by the DPI information in the resolve context.
     * @param v The length value in inches.
     * @return A length instance representing the given value in inches.
     */
    [[nodiscard]] constexpr length in(float v) noexcept { return length::unit(length_unit::in, v); }

    /**
     * @fn cm
     * @brief Constructs a length measurement in centimeters. The actual pixel value will be determined by the DPI information in the resolve context.
     * @param v The length value in centimeters.
     * @return A length instance representing the given value in centimeters.
     */
    [[nodiscard]] constexpr length cm(float v) noexcept { return length::unit(length_unit::cm, v); }

    /**
     * @fn mm
     * @brief Constructs a length measurement in millimeters. The actual pixel value will be determined by the DPI information in the resolve context.
     * @param v The length value in millimeters.
     * @return A length instance representing the given value in millimeters.
     */
    [[nodiscard]] constexpr length mm(float v) noexcept { return length::unit(length_unit::mm, v); }

    /**
     * @fn resolve_or
     * @brief Resolves a length measurement to a pixel value based on the provided context. If the length is auto, this function will return the specified auto_px value instead.
     * @param v The length measurement to resolve.
     * @param a The axis (x or y) to resolve against, which determines how percentage units are calculated.
     * @param ctx The context containing information needed to resolve the measurement (e.g. DPI scale, font sizes, parent dimensions, viewport dimensions).
     * @param auto_px The pixel value to return if the length is auto. This allows the caller to specify what "auto" should resolve to in this context.
     * @return The resolved pixel value for the given length measurement and context.
     */
    [[nodiscard]] inline float resolve_or(const length &v, axis a, const resolve_context &ctx, float auto_px = 0.0f) noexcept
    {
        if (v.is_auto)
            return auto_px;

        const float parent_axis_px = (a == axis::x) ? ctx.parent_width_px : ctx.parent_height_px;

        const float dpi = ctx.effective_dpi(a);
        const float dp_scale = ctx.effective_dpi_scale(a);

        float out = 0.0f;
        out += v.get(length_unit::px);
        out += v.get(length_unit::dp) * dp_scale;
        out += v.get(length_unit::em) * ctx.font_px;
        out += v.get(length_unit::rem) * ctx.root_font_px;
        out += v.get(length_unit::percent) * (parent_axis_px / 100.0f);
        out += v.get(length_unit::vw) * (ctx.viewport_width_px / 100.0f);
        out += v.get(length_unit::vh) * (ctx.viewport_height_px / 100.0f);
        out += v.get(length_unit::in) * dpi;
        out += v.get(length_unit::cm) * (dpi / 2.54f);
        out += v.get(length_unit::mm) * (dpi / 25.4f);
        return out;
    }

    namespace literals
    {
        /**
         * @fn operator"" _px
         * @brief User-defined literal for creating a length measurement in pixels. For example, 10.0f_px would create a length representing "10px".
         * @param v The length value in pixels.
         * @return A length instance representing the given value in pixels.
         */
        [[nodiscard]] constexpr length operator""_px(long double v) noexcept { return px(static_cast<float>(v)); }
        /**
         * @fn operator"" _dp
         * @brief User-defined literal for creating a length measurement in density-independent pixels (dp). For example, 10.0f_dp would create a length representing "10dp".
         * @param v The length value in dp.
         * @return A length instance representing the given value in dp.
         */
        [[nodiscard]] constexpr length operator""_dp(long double v) noexcept { return dp(static_cast<float>(v)); }
        /**
         * @fn operator"" _em
         * @brief User-defined literal for creating a length measurement in em units. For example, 1.5f_em would create a length representing "1.5em".
         * @param v The length value in em.
         * @return A length instance representing the given value in em.
         */
        [[nodiscard]] constexpr length operator""_em(long double v) noexcept { return em(static_cast<float>(v)); }
        /**
         * @fn operator"" _rem
         * @brief User-defined literal for creating a length measurement in rem units. For example, 2.0f_rem would create a length representing "2rem".
         * @param v The length value in rem.
         * @return A length instance representing the given value in rem.
         */
        [[nodiscard]] constexpr length operator""_rem(long double v) noexcept { return rem(static_cast<float>(v)); }
        /**
         * @fn operator"" _percent
         * @brief User-defined literal for creating a length measurement in percentage units. For example, 50.0f_percent would create a length representing "50%".
         * @param v The length value in percent.
         * @return A length instance representing the given value in percent.
         */
        [[nodiscard]] constexpr length operator""_percent(long double v) noexcept { return percent(static_cast<float>(v)); }
        /**
         * @fn operator"" _vw
         * @brief User-defined literal for creating a length measurement in viewport width units (vw). For example, 10.0f_vw would create a length representing "10vw".
         * @param v The length value in vw.
         * @return A length instance representing the given value in vw.
         */
        [[nodiscard]] constexpr length operator""_vw(long double v) noexcept { return vw(static_cast<float>(v)); }
        /**
         * @fn operator"" _vh
         * @brief User-defined literal for creating a length measurement in viewport height units (vh). For example, 10.0f_vh would create a length representing "10vh".
         * @param v The length value in vh.
         * @return A length instance representing the given value in vh.
         */
        [[nodiscard]] constexpr length operator""_vh(long double v) noexcept { return vh(static_cast<float>(v)); }
        /**
         * @fn operator"" _in
         * @brief User-defined literal for creating a length measurement in inches. For example, 2.0f_in would create a length representing "2 inches".
         * @param v The length value in inches.
         * @return A length instance representing the given value in inches.
         */
        [[nodiscard]] constexpr length operator""_in(long double v) noexcept { return in(static_cast<float>(v)); }
        /**
         * @fn operator"" _cm
         * @brief User-defined literal for creating a length measurement in centimeters. For example, 5.0f_cm would create a length representing "5 centimeters".
         * @param v The length value in centimeters.
         * @return A length instance representing the given value in centimeters.
         */
        [[nodiscard]] constexpr length operator""_cm(long double v) noexcept { return cm(static_cast<float>(v)); }
        /**
         * @fn operator"" _mm
         * @brief User-defined literal for creating a length measurement in millimeters. For example, 10.0f_mm would create a length representing "10 millimeters".
         * @param v The length value in millimeters.
         * @return A length instance representing the given value in millimeters.
         */
        [[nodiscard]] constexpr length operator""_mm(long double v) noexcept { return mm(static_cast<float>(v)); }
    } // namespace literals
} // namespace catalyst::ui