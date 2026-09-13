/**
 * @file color.hpp
 * @brief Defines the color type used by UI styles and the draw list.
 * @details Colors are stored as four floats in the range [0, 1] because that is the form blending,
 * opacity and interpolation need. Packing to the eight-bit-per-channel form a vertex buffer wants is
 * an explicit step, so the lossy conversion only happens where it is intended.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <cstdint>

namespace catalyst::ui
{

    /**
     * @struct color
     * @brief An RGBA color with one float per channel, nominally in the range [0, 1].
     * @details Values outside [0, 1] are permitted so that intermediate arithmetic (for example an
     * animation overshooting) does not have to clamp at every step; `saturate` clamps when a final
     * value is needed. Alpha is straight (not premultiplied); `premultiplied` produces the other form
     * for renderers that want it.
     */
    struct color
    {
        /**
         * @brief The red channel.
         */
        float r = 0.0f;
        /**
         * @brief The green channel.
         */
        float g = 0.0f;
        /**
         * @brief The blue channel.
         */
        float b = 0.0f;
        /**
         * @brief The alpha channel, where 0 is fully transparent and 1 is fully opaque.
         */
        float a = 1.0f;

        /**
         * @fn rgb
         * @brief Builds an opaque color from three channel values.
         * @param r The red channel, in [0, 1].
         * @param g The green channel, in [0, 1].
         * @param b The blue channel, in [0, 1].
         * @return The color, with alpha set to 1.
         */
        [[nodiscard]] static constexpr color rgb(float r, float g, float b) noexcept { return color{r, g, b, 1.0f}; }

        /**
         * @fn rgba
         * @brief Builds a color from four channel values.
         * @param r The red channel, in [0, 1].
         * @param g The green channel, in [0, 1].
         * @param b The blue channel, in [0, 1].
         * @param a The alpha channel, in [0, 1].
         * @return The color.
         */
        [[nodiscard]] static constexpr color rgba(float r, float g, float b, float a) noexcept { return color{r, g, b, a}; }

        /**
         * @fn from_rgba8
         * @brief Builds a color from eight-bit-per-channel values.
         * @param r The red channel, in [0, 255].
         * @param g The green channel, in [0, 255].
         * @param b The blue channel, in [0, 255].
         * @param a The alpha channel, in [0, 255]. Defaults to fully opaque.
         * @return The color, with each channel divided by 255.
         */
        [[nodiscard]] static constexpr color from_rgba8(std::uint8_t r, std::uint8_t g, std::uint8_t b,
                                                        std::uint8_t a = 255) noexcept
        {
            return color{static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f, static_cast<float>(b) / 255.0f,
                         static_cast<float>(a) / 255.0f};
        }

        /**
         * @fn from_argb32
         * @brief Builds a color from a packed 0xAARRGGBB value, the form color literals are usually written in.
         * @param packed The packed color, with alpha in the most significant byte.
         * @return The unpacked color.
         */
        [[nodiscard]] static constexpr color from_argb32(std::uint32_t packed) noexcept
        {
            return from_rgba8(static_cast<std::uint8_t>((packed >> 16) & 0xFFu), static_cast<std::uint8_t>((packed >> 8) & 0xFFu),
                              static_cast<std::uint8_t>(packed & 0xFFu), static_cast<std::uint8_t>((packed >> 24) & 0xFFu));
        }

        /**
         * @fn to_rgba8
         * @brief Packs this color into a 0xAABBGGRR value, the channel order a vertex color attribute usually expects.
         * @details Channels are saturated to [0, 1] and rounded to nearest before packing.
         * @return The packed color, with red in the least significant byte.
         */
        [[nodiscard]] constexpr std::uint32_t to_rgba8() const noexcept
        {
            const auto quantize = [](float v) constexpr noexcept -> std::uint32_t {
                const float clamped = (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v);
                return static_cast<std::uint32_t>(clamped * 255.0f + 0.5f);
            };

            return quantize(r) | (quantize(g) << 8) | (quantize(b) << 16) | (quantize(a) << 24);
        }

        /**
         * @fn with_alpha
         * @brief Returns a copy of this color with a different alpha channel.
         * @param alpha The alpha channel for the copy, in [0, 1].
         * @return The color with the given alpha.
         */
        [[nodiscard]] constexpr color with_alpha(float alpha) const noexcept { return color{r, g, b, alpha}; }

        /**
         * @fn premultiplied
         * @brief Returns this color with its RGB channels scaled by its alpha.
         * @return The premultiplied-alpha form of this color.
         */
        [[nodiscard]] constexpr color premultiplied() const noexcept { return color{r * a, g * a, b * a, a}; }

        /**
         * @fn saturate
         * @brief Returns this color with every channel clamped to [0, 1].
         * @return The clamped color.
         */
        [[nodiscard]] constexpr color saturate() const noexcept
        {
            const auto clamp01 = [](float v) constexpr noexcept { return (v < 0.0f) ? 0.0f : ((v > 1.0f) ? 1.0f : v); };
            return color{clamp01(r), clamp01(g), clamp01(b), clamp01(a)};
        }

        /**
         * @fn is_transparent
         * @brief Reports whether this color is fully transparent and therefore contributes nothing when drawn.
         * @return True when alpha is at or below zero.
         */
        [[nodiscard]] constexpr bool is_transparent() const noexcept { return a <= 0.0f; }

        /**
         * @brief Compares two colors for exact channel-by-channel equality.
         */
        [[nodiscard]] constexpr bool operator==(const color &other) const noexcept = default;
    };

    /**
     * @fn lerp
     * @brief Linearly interpolates between two colors, channel by channel.
     * @param from The color returned when `t` is 0.
     * @param to The color returned when `t` is 1.
     * @param t The interpolation factor. Values outside [0, 1] extrapolate.
     * @return The interpolated color.
     */
    [[nodiscard]] constexpr color lerp(const color &from, const color &to, float t) noexcept
    {
        return color{from.r + (to.r - from.r) * t, from.g + (to.g - from.g) * t, from.b + (to.b - from.b) * t,
                     from.a + (to.a - from.a) * t};
    }

    /**
     * @namespace catalyst::ui::colors
     * @brief A small set of named colors, enough for defaults and tests without pulling in a palette.
     */
    namespace colors
    {
        /**
         * @brief Fully transparent black, the default background of a node.
         */
        inline constexpr color transparent = color{0.0f, 0.0f, 0.0f, 0.0f};
        /**
         * @brief Opaque black.
         */
        inline constexpr color black = color::rgb(0.0f, 0.0f, 0.0f);
        /**
         * @brief Opaque white.
         */
        inline constexpr color white = color::rgb(1.0f, 1.0f, 1.0f);
        /**
         * @brief Opaque red.
         */
        inline constexpr color red = color::rgb(1.0f, 0.0f, 0.0f);
        /**
         * @brief Opaque green.
         */
        inline constexpr color green = color::rgb(0.0f, 1.0f, 0.0f);
        /**
         * @brief Opaque blue.
         */
        inline constexpr color blue = color::rgb(0.0f, 0.0f, 1.0f);
    } // namespace colors

} // namespace catalyst::ui
