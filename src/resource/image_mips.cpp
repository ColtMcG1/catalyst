/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file image_mips.cpp
 * @brief @ref catalyst::resource::generate_mips -- a box filter down to 1x1, with the two
 * corrections that separate a usable chain from a wrong one.
 * @details The filter itself is the boring half. Every level is the average of the up-to-2x2x2
 * texels above it, each level filtered from the one before rather than from level 0, which is what
 * every tool and every `glGenerateMipmap` does and is O(n) over the whole chain.
 *
 * The two corrections are the half worth reading:
 *
 *   - **sRGB texels are not proportional to light.** Averaging the stored bytes of 0 and 255 gives
 *     128, which represents about 22% of the light that the correct answer, 188, does. Do that down
 *     a nine-level chain and a surface visibly darkens as it recedes -- and it darkens differently
 *     from the surface next to it that happened to keep level 0, which is what makes the artefact
 *     look like a lighting bug rather than a texture bug. So an sRGB format is decoded to linear,
 *     averaged, and re-encoded. Alpha is exempt: it is linear even in an sRGB format.
 *   - **Transparent texels still have colour.** In a cutout texture the fully transparent texels
 *     usually hold something arbitrary, often black, and an unweighted average pulls it into the
 *     visible neighbours a little more at every level. Weighting colour by alpha fixes it, and is
 *     off by default because it is wrong for an image whose alpha is data rather than coverage.
 *
 * Neither is a matter of taste, and neither is something a caller can correct afterwards, which is
 * why they are here rather than left to one.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/resource/image.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <optional>
#include <string>
#include <vector>

namespace catalyst::resource
{
    namespace
    {
        /** @brief What the filter needs to know about a format, once it has agreed to filter it. */
        struct filter_desc
        {
            std::uint32_t channels = 0;
            bool floating = false;
            bool srgb = false;
        };

        /**
         * @brief The formats this filter handles, or `nullopt`.
         * @details 8-bit unorm and 32-bit float, and nothing else. `rgba16_float` is the notable
         * absence: filtering it means a half-to-float conversion in both directions, and this module
         * has no half-float code because nothing else in it needs any. A 16-bit float image that
         * wants a chain is a good argument for cooking it into a KTX2.
         *
         * Depth formats are absent because a filtered depth buffer is meaningless, and the integer
         * formats because averaging an index or an id produces a value that indexes nothing.
         */
        std::optional<filter_desc> describe(format f) noexcept
        {
            switch (f)
            {
            case format::r8_unorm:         return filter_desc{1, false, false};
            case format::rg8_unorm:        return filter_desc{2, false, false};
            case format::rgba8_unorm:      return filter_desc{4, false, false};
            case format::rgba8_unorm_srgb: return filter_desc{4, false, true};
            case format::bgra8_unorm:      return filter_desc{4, false, false};
            case format::bgra8_unorm_srgb: return filter_desc{4, false, true};
            case format::r32_float:        return filter_desc{1, true, false};
            case format::rg32_float:       return filter_desc{2, true, false};
            case format::rgb32_float:      return filter_desc{3, true, false};
            case format::rgba32_float:     return filter_desc{4, true, false};
            default:                       return std::nullopt;
            }
        }

        /**
         * @brief sRGB byte to linear float, tabulated.
         * @details 256 entries is the whole domain, so this is exact rather than an approximation,
         * and it replaces a `pow` per channel read with a load. The encode direction has no such
         * shortcut -- its input is a continuous float -- and stays a `pow` per channel written,
         * which is a quarter of the work because only the destination texels are encoded.
         */
        const std::array<float, 256> &srgb_to_linear_table() noexcept
        {
            static const std::array<float, 256> table = [] {
                std::array<float, 256> values{};
                for (std::size_t i = 0; i < values.size(); ++i)
                {
                    const float encoded = static_cast<float>(i) / 255.0f;
                    values[i] = (encoded <= 0.04045f) ? (encoded / 12.92f)
                                                      : std::pow((encoded + 0.055f) / 1.055f, 2.4f);
                }
                return values;
            }();

            return table;
        }

        /** @brief Linear float to sRGB byte: the IEC 61966-2-1 transfer function, rounded. */
        std::uint8_t linear_to_srgb_byte(float linear) noexcept
        {
            const float encoded =
                (linear <= 0.0031308f) ? (linear * 12.92f) : (1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f);

            return static_cast<std::uint8_t>(std::clamp(encoded, 0.0f, 1.0f) * 255.0f + 0.5f);
        }

        /** @brief Bytes one texel occupies, for a format `describe` accepted. */
        std::size_t texel_stride(const filter_desc &desc) noexcept
        {
            return desc.channels * (desc.floating ? sizeof(float) : 1u);
        }

        /**
         * @brief Halves one mip level into the next.
         * @tparam Floating Whether the components are `float` rather than `std::uint8_t`. A template
         *         parameter rather than a branch because it decides the two innermost operations in
         *         a loop that runs once per component per sample.
         */
        template <bool Floating>
        void filter_level(std::span<const std::byte> source, std::span<std::byte> destination, extent3d source_extent,
                          extent3d destination_extent, const filter_desc &desc, const mip_options &options)
        {
            const std::size_t stride = texel_stride(desc);
            const std::array<float, 256> &to_linear = srgb_to_linear_table();

            const auto read = [&](const std::byte *texel, std::uint32_t channel) noexcept -> float {
                if constexpr (Floating)
                {
                    float value = 0.0f;
                    std::memcpy(&value, texel + channel * sizeof(float), sizeof(float));
                    return value;
                }
                else
                {
                    const auto raw = static_cast<std::uint8_t>(texel[channel]);

                    // Channel 3 is alpha and is linear even in an sRGB format, so it never goes
                    // through the table. Channels 0-2 are the colour, whichever of R and B comes
                    // first -- a BGRA swap does not move which components are gamma-encoded.
                    const bool encoded = desc.srgb && options.srgb_aware && channel < 3;
                    return encoded ? to_linear[raw] : static_cast<float>(raw) * (1.0f / 255.0f);
                }
            };

            const auto write = [&](std::byte *texel, std::uint32_t channel, float value) noexcept {
                if constexpr (Floating)
                {
                    std::memcpy(texel + channel * sizeof(float), &value, sizeof(float));
                }
                else
                {
                    const bool encoded = desc.srgb && options.srgb_aware && channel < 3;
                    const std::uint8_t out =
                        encoded ? linear_to_srgb_byte(value)
                                : static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
                    texel[channel] = static_cast<std::byte>(out);
                }
            };

            // An axis that is already 1 contributes one sample, not two of the same texel. Where an
            // axis is larger, `2x + 1` is always in range: the destination extent is the floor of
            // half the source, so an odd source drops its last row rather than sampling past it.
            const std::uint32_t taps_x = source_extent.width > 1 ? 2u : 1u;
            const std::uint32_t taps_y = source_extent.height > 1 ? 2u : 1u;
            const std::uint32_t taps_z = source_extent.depth > 1 ? 2u : 1u;
            const auto tap_count = static_cast<float>(taps_x * taps_y * taps_z);

            const bool weighted = options.alpha_weighted && desc.channels == 4;

            for (std::uint32_t z = 0; z < destination_extent.depth; ++z)
            {
                for (std::uint32_t y = 0; y < destination_extent.height; ++y)
                {
                    for (std::uint32_t x = 0; x < destination_extent.width; ++x)
                    {
                        std::array<float, 4> plain{};
                        std::array<float, 4> by_alpha{};
                        float weight = 0.0f;

                        for (std::uint32_t tz = 0; tz < taps_z; ++tz)
                        {
                            for (std::uint32_t ty = 0; ty < taps_y; ++ty)
                            {
                                for (std::uint32_t tx = 0; tx < taps_x; ++tx)
                                {
                                    const std::uint64_t sx = 2ull * x + tx;
                                    const std::uint64_t sy = 2ull * y + ty;
                                    const std::uint64_t sz = 2ull * z + tz;

                                    const std::byte *texel =
                                        source.data() +
                                        ((sz * source_extent.height + sy) * source_extent.width + sx) * stride;

                                    const float alpha = weighted ? read(texel, 3) : 1.0f;

                                    for (std::uint32_t c = 0; c < desc.channels; ++c)
                                    {
                                        const float value = read(texel, c);
                                        plain[c] += value;
                                        if (c < 3)
                                            by_alpha[c] += value * alpha;
                                    }

                                    weight += alpha;
                                }
                            }
                        }

                        std::byte *out =
                            destination.data() +
                            ((static_cast<std::uint64_t>(z) * destination_extent.height + y) * destination_extent.width +
                             x) *
                                stride;

                        for (std::uint32_t c = 0; c < desc.channels; ++c)
                        {
                            // Colour is alpha-weighted when asked for; alpha itself never is. A
                            // neighbourhood that is entirely transparent has no colour worth
                            // preserving and falls back to the plain average, which is also the
                            // only thing to do when the divisor would be zero.
                            const bool use_weighted = weighted && c < 3 && weight > 0.0f;
                            write(out, c, use_weighted ? by_alpha[c] / weight : plain[c] / tap_count);
                        }
                    }
                }
            }
        }

    } // namespace

    std::expected<image, error> generate_mips(const image &source, mip_options options)
    {
        if (source.empty() || !source.consistent())
            return std::unexpected(make_error(error_code::decode_failed, {},
                                              "cannot generate mips: the source image's pixels do not match its own "
                                              "format, extent and level description"));

        const format fmt = source.pixel_format();

        if (is_block_compressed(fmt))
            return std::unexpected(make_error(error_code::unsupported_format, {},
                                              "cannot generate mips for a block-compressed image: filtering the texels "
                                              "means decompressing and re-encoding them, which belongs in a cooker. A "
                                              "cooked KTX2 or DDS already carries its chain"));

        const std::optional<filter_desc> desc = describe(fmt);
        if (!desc)
            return std::unexpected(make_error(error_code::unsupported_format, {},
                                              "no mip filter for this format; see generate_mips in image.hpp for the "
                                              "set that is filterable"));

        const extent3d extent = source.extent();
        const std::uint32_t layers = source.array_layers();

        // Clamp rather than reject. A caller asking for "a chain" and a caller asking for "eight
        // levels" of a 4x4 image both want as much of one as exists.
        const std::uint32_t available = max_mip_levels(extent);
        const std::uint32_t levels = (options.levels == 0) ? available : std::min(options.levels, available);

        const std::uint64_t chain_bytes = packed_size_bytes(fmt, extent, levels, 1);
        std::vector<std::byte> pixels(static_cast<std::size_t>(chain_bytes * layers));

        for (std::uint32_t layer = 0; layer < layers; ++layer)
        {
            const std::size_t base = static_cast<std::size_t>(chain_bytes * layer);

            // Level 0 is copied verbatim from the source's own level 0. Any levels the source
            // already had below it are ignored: this rebuilds a chain rather than extending one, so
            // calling it on its own output gives the same answer again.
            const std::span<const std::byte> level_zero = source.level(0, layer);
            std::memcpy(pixels.data() + base, level_zero.data(), level_zero.size());

            std::size_t previous = base;
            for (std::uint32_t level = 1; level < levels; ++level)
            {
                const extent3d source_extent = mip_extent(extent, level - 1);
                const extent3d destination_extent = mip_extent(extent, level);

                const std::size_t offset = base + static_cast<std::size_t>(packed_size_bytes(fmt, extent, level, 1));
                const auto source_bytes = static_cast<std::size_t>(format_image_size_bytes(fmt, source_extent));
                const auto destination_bytes =
                    static_cast<std::size_t>(format_image_size_bytes(fmt, destination_extent));

                const std::span<const std::byte> in{pixels.data() + previous, source_bytes};
                const std::span<std::byte> out{pixels.data() + offset, destination_bytes};

                if (desc->floating)
                    filter_level<true>(in, out, source_extent, destination_extent, *desc, options);
                else
                    filter_level<false>(in, out, source_extent, destination_extent, *desc, options);

                previous = offset;
            }
        }

        return image{std::move(pixels), fmt, extent, levels, layers};
    }

} // namespace catalyst::resource
