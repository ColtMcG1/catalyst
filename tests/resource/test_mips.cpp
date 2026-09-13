/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file test_mips.cpp
 * @brief catalyst::resource::generate_mips -- the chain's shape, and the two corrections that make
 * the values right rather than merely present.
 * @details The images here are built in the test rather than decoded, because what is under test is
 * arithmetic and a decoder in the middle would only make a failure harder to read. Each case names
 * the number it expects and why: 188 rather than 128 for the sRGB average is the whole argument for
 * `mip_options::srgb_aware` written as an assertion.
 *
 * No decoder is needed for any of this, so the suite runs in a `CATALYST_RESOURCE_STB=OFF` build.
 * License: MIT (see LICENSE).
 */

#include "../test_common.hpp"

#include <catalyst/resource/image.hpp>

#include <cstdint>
#include <cstring>
#include <vector>

using namespace catalyst::resource;

namespace
{
    /** @brief An image over 8-bit texels given in memory order. */
    image make_u8(format fmt, extent3d extent, std::vector<std::uint8_t> texels, std::uint32_t layers = 1)
    {
        std::vector<std::byte> bytes(texels.size());
        for (std::size_t i = 0; i < texels.size(); ++i)
            bytes[i] = static_cast<std::byte>(texels[i]);

        return image{std::move(bytes), fmt, extent, 1, layers};
    }

    /** @brief An image over 32-bit float texels. */
    image make_f32(format fmt, extent3d extent, std::vector<float> texels)
    {
        std::vector<std::byte> bytes(texels.size() * sizeof(float));
        std::memcpy(bytes.data(), texels.data(), bytes.size());

        return image{std::move(bytes), fmt, extent, 1, 1};
    }

    std::uint8_t u8_at(const image &img, std::uint32_t level, std::size_t index, std::uint32_t layer = 0)
    {
        const std::span<const std::byte> bytes = img.level(level, layer);
        CT_REQUIRE(index < bytes.size());
        return static_cast<std::uint8_t>(bytes[index]);
    }

    float f32_at(const image &img, std::uint32_t level, std::size_t index)
    {
        const std::span<const std::byte> bytes = img.level(level);
        float value = 0.0f;
        CT_REQUIRE((index + 1) * sizeof(float) <= bytes.size());
        std::memcpy(&value, bytes.data() + index * sizeof(float), sizeof(float));
        return value;
    }

    // -----------------------------------------------------------------------------
    // Shape
    // -----------------------------------------------------------------------------

    void test_chain_shape()
    {
        const image source = make_u8(format::rgba8_unorm, {4, 4, 1}, std::vector<std::uint8_t>(4 * 4 * 4, 128));

        auto full = generate_mips(source);
        CT_REQUIRE(full.has_value());
        CT_REQUIRE(full->mip_levels() == 3); // 4x4, 2x2, 1x1
        CT_REQUIRE(full->extent() == (extent3d{4, 4, 1}));
        CT_REQUIRE(full->pixel_format() == format::rgba8_unorm);
        CT_REQUIRE(full->array_layers() == 1);
        CT_REQUIRE(full->consistent());
        CT_REQUIRE(full->pixels().size() == 64 + 16 + 4);

        // A count that the extent cannot supply is clamped, not refused: "a chain" and "eight
        // levels of it" should not need the caller to compute the same number twice.
        auto clamped = generate_mips(source, {.levels = 8});
        CT_REQUIRE(clamped.has_value());
        CT_REQUIRE(clamped->mip_levels() == 3);

        // ...and a smaller count is honoured exactly, for the caller who wants to stop at 2x2.
        auto partial = generate_mips(source, {.levels = 2});
        CT_REQUIRE(partial.has_value());
        CT_REQUIRE(partial->mip_levels() == 2);
        CT_REQUIRE(partial->pixels().size() == 64 + 16);

        // One level is a copy, and still a valid answer.
        auto single = generate_mips(source, {.levels = 1});
        CT_REQUIRE(single.has_value());
        CT_REQUIRE(single->mip_levels() == 1);
        CT_REQUIRE(single->pixels().size() == 64);
    }

    void test_non_power_of_two_floors()
    {
        // 5x5 halves to 2x2, not 3x3: the last row and column are dropped rather than blended,
        // which is the convention mip_extent and every graphics API share.
        const image source = make_u8(format::r8_unorm, {5, 5, 1}, std::vector<std::uint8_t>(25, 7));

        auto chain = generate_mips(source);
        CT_REQUIRE(chain.has_value());
        CT_REQUIRE(chain->mip_levels() == 3);
        CT_REQUIRE(chain->extent_at(1) == (extent3d{2, 2, 1}));
        CT_REQUIRE(chain->level(1).size() == 4);
        CT_REQUIRE(chain->level(2).size() == 1);
        CT_REQUIRE(chain->consistent());

        // Every source texel is 7, so every average is 7 whichever texels were dropped.
        CT_REQUIRE(u8_at(*chain, 1, 0) == 7);
        CT_REQUIRE(u8_at(*chain, 2, 0) == 7);
    }

    void test_axis_that_reaches_one_keeps_going()
    {
        // 8x1 has four levels, not one. An axis at 1 contributes a single sample rather than the
        // same texel twice, which is what stops the average from being silently weighted.
        const image source = make_u8(format::r8_unorm, {8, 1, 1}, {0, 8, 16, 24, 32, 40, 48, 56});

        auto chain = generate_mips(source);
        CT_REQUIRE(chain.has_value());
        CT_REQUIRE(chain->mip_levels() == 4);
        CT_REQUIRE(chain->extent_at(1) == (extent3d{4, 1, 1}));

        CT_REQUIRE(u8_at(*chain, 1, 0) == 4);  // (0 + 8) / 2
        CT_REQUIRE(u8_at(*chain, 1, 3) == 52); // (48 + 56) / 2
        CT_REQUIRE(u8_at(*chain, 3, 0) == 28); // the mean of the whole row
    }

    // -----------------------------------------------------------------------------
    // Values
    // -----------------------------------------------------------------------------

    void test_box_filter_averages()
    {
        // A 2x2 whose red channel is 0, 100, 200, 255. The mean is 138.75, and the round-half-up
        // the writer does makes that 139 rather than 138.
        const image source = make_u8(format::rgba8_unorm, {2, 2, 1},
                                     {0, 0, 0, 255, 100, 0, 0, 255, 200, 0, 0, 255, 255, 0, 0, 255});

        auto chain = generate_mips(source);
        CT_REQUIRE(chain.has_value());
        CT_REQUIRE(chain->mip_levels() == 2);

        CT_REQUIRE(u8_at(*chain, 1, 0) == 139); // red
        CT_REQUIRE(u8_at(*chain, 1, 1) == 0);   // green
        CT_REQUIRE(u8_at(*chain, 1, 3) == 255); // alpha, uniformly opaque

        // Level 0 is the source, byte for byte.
        CT_REQUIRE(u8_at(*chain, 0, 4) == 100);
    }

    void test_srgb_is_averaged_in_linear_light()
    {
        // Two black texels and two white ones. In linear light the average is half the light, which
        // sRGB spells 188. Averaging the stored bytes gives 128 -- about 22% of the light -- and
        // that gap, compounded down a chain, is the artefact srgb_aware exists to prevent.
        const image source = make_u8(format::rgba8_unorm_srgb, {2, 2, 1},
                                     {0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255});

        auto correct = generate_mips(source);
        CT_REQUIRE(correct.has_value());
        CT_REQUIRE(u8_at(*correct, 1, 0) == 188);
        CT_REQUIRE(u8_at(*correct, 1, 1) == 188);
        CT_REQUIRE(u8_at(*correct, 1, 2) == 188);

        // Alpha is linear even in an sRGB format, so it must NOT go through the curve. All four
        // texels are opaque, so anything but 255 here means alpha was gamma-encoded.
        CT_REQUIRE(u8_at(*correct, 1, 3) == 255);

        auto naive = generate_mips(source, {.srgb_aware = false});
        CT_REQUIRE(naive.has_value());
        CT_REQUIRE(u8_at(*naive, 1, 0) == 128);

        // A linear format is unaffected by the flag either way -- averaging its bytes is already
        // the right thing.
        const image linear = make_u8(format::rgba8_unorm, {2, 2, 1},
                                     {0, 0, 0, 255, 0, 0, 0, 255, 255, 255, 255, 255, 255, 255, 255, 255});
        auto linear_chain = generate_mips(linear);
        CT_REQUIRE(linear_chain.has_value());
        CT_REQUIRE(u8_at(*linear_chain, 1, 0) == 128);
    }

    void test_alpha_weighting()
    {
        // One opaque red texel and three fully transparent black ones -- a cutout texture's edge.
        const image source =
            make_u8(format::rgba8_unorm, {2, 2, 1}, {255, 0, 0, 255, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0});

        // Unweighted, the transparent black drags the red down to a quarter of itself. Every level
        // down does it again, which is the dark fringe on minified foliage.
        auto plain = generate_mips(source);
        CT_REQUIRE(plain.has_value());
        CT_REQUIRE(u8_at(*plain, 1, 0) == 64); // 255 / 4, rounded

        // Weighted, the only texel with any coverage decides the colour.
        auto weighted = generate_mips(source, {.alpha_weighted = true});
        CT_REQUIRE(weighted.has_value());
        CT_REQUIRE(u8_at(*weighted, 1, 0) == 255);

        // Alpha itself is averaged unweighted either way; weighting it would make the cutout
        // dissolve or spread as it minifies.
        CT_REQUIRE(u8_at(*plain, 1, 3) == 64);
        CT_REQUIRE(u8_at(*weighted, 1, 3) == 64);

        // A neighbourhood that is entirely transparent has no colour to preserve and no non-zero
        // divisor, so it falls back to the plain average rather than dividing by zero.
        const image invisible =
            make_u8(format::rgba8_unorm, {2, 2, 1}, {200, 0, 0, 0, 200, 0, 0, 0, 200, 0, 0, 0, 200, 0, 0, 0});
        auto fallback = generate_mips(invisible, {.alpha_weighted = true});
        CT_REQUIRE(fallback.has_value());
        CT_REQUIRE(u8_at(*fallback, 1, 0) == 200);
        CT_REQUIRE(u8_at(*fallback, 1, 3) == 0);
    }

    void test_three_dimensions_average_eight_texels()
    {
        // A 2x2x2 volume collapses to one texel, and that texel is the mean of all eight -- not of
        // the four in one slice.
        const image source = make_u8(format::r8_unorm, {2, 2, 2}, {0, 10, 20, 30, 40, 50, 60, 70});

        auto chain = generate_mips(source);
        CT_REQUIRE(chain.has_value());
        CT_REQUIRE(chain->mip_levels() == 2);
        CT_REQUIRE(chain->extent_at(1) == (extent3d{1, 1, 1}));
        CT_REQUIRE(u8_at(*chain, 1, 0) == 35); // 280 / 8
    }

    void test_float_formats()
    {
        const image source = make_f32(format::r32_float, {2, 2, 1}, {1.0f, 2.0f, 3.0f, 4.0f});

        auto chain = generate_mips(source);
        CT_REQUIRE(chain.has_value());
        CT_REQUIRE(chain->mip_levels() == 2);
        CT_REQUIRE(f32_at(*chain, 1, 0) == 2.5f);

        // Float texels are not clamped to [0, 1]: an HDR image whose values run past 1 keeps them.
        const image hdr = make_f32(format::r32_float, {2, 2, 1}, {4.0f, 8.0f, 12.0f, 16.0f});
        auto hdr_chain = generate_mips(hdr);
        CT_REQUIRE(hdr_chain.has_value());
        CT_REQUIRE(f32_at(*hdr_chain, 1, 0) == 10.0f);
    }

    void test_layers_are_filtered_independently()
    {
        // Two layers, uniformly 40 and 200. Nothing may leak across the boundary between them.
        std::vector<std::uint8_t> texels(2 * 2 * 2, 40);
        for (std::size_t i = 4; i < texels.size(); ++i)
            texels[i] = 200;

        const image source = make_u8(format::r8_unorm, {2, 2, 1}, texels, 2);
        CT_REQUIRE(source.consistent());

        auto chain = generate_mips(source);
        CT_REQUIRE(chain.has_value());
        CT_REQUIRE(chain->array_layers() == 2);
        CT_REQUIRE(chain->mip_levels() == 2);
        CT_REQUIRE(chain->consistent());

        CT_REQUIRE(u8_at(*chain, 0, 0, 0) == 40);
        CT_REQUIRE(u8_at(*chain, 1, 0, 0) == 40);
        CT_REQUIRE(u8_at(*chain, 0, 0, 1) == 200);
        CT_REQUIRE(u8_at(*chain, 1, 0, 1) == 200);
    }

    void test_regenerating_reproduces_itself()
    {
        const image source = make_u8(format::rgba8_unorm, {4, 4, 1}, [] {
            std::vector<std::uint8_t> data(4 * 4 * 4);
            for (std::size_t i = 0; i < data.size(); ++i)
                data[i] = static_cast<std::uint8_t>(i * 3 + 5);
            return data;
        }());

        auto once = generate_mips(source);
        CT_REQUIRE(once.has_value());

        // Run it on its own output. Levels below 0 are input to nothing, so the second pass must
        // reproduce the first exactly rather than filtering an already-filtered chain.
        auto twice = generate_mips(*once);
        CT_REQUIRE(twice.has_value());
        CT_REQUIRE(twice->mip_levels() == once->mip_levels());
        CT_REQUIRE(twice->pixels().size() == once->pixels().size());
        CT_REQUIRE(std::equal(once->pixels().begin(), once->pixels().end(), twice->pixels().begin()));
    }

    // -----------------------------------------------------------------------------
    // Refusals
    // -----------------------------------------------------------------------------

    void test_refuses_what_it_cannot_filter()
    {
        // Block-compressed: filtering means decompress, filter, re-encode, and a real BC encoder
        // belongs in a cooker. A cooked container already carries the chain this would rebuild.
        const image bc7 = image{std::vector<std::byte>(16), format::bc7_unorm, {4, 4, 1}, 1, 1};
        CT_REQUIRE(bc7.consistent());

        auto compressed = generate_mips(bc7);
        CT_REQUIRE(!compressed.has_value());
        CT_REQUIRE(compressed.error().code == error_code::unsupported_format);
        CT_REQUIRE(compressed.error().detail.find("block-compressed") != std::string::npos);

        // rgba16_float has no filter here, because half-float conversion is code this module does
        // not otherwise need. It is unsupported_format and not a silent pass-through.
        const image half = image{std::vector<std::byte>(2 * 2 * 8), format::rgba16_float, {2, 2, 1}, 1, 1};
        auto unfilterable = generate_mips(half);
        CT_REQUIRE(!unfilterable.has_value());
        CT_REQUIRE(unfilterable.error().code == error_code::unsupported_format);

        // An image whose bytes do not match its own description is a bad input, not a missing
        // feature, so it is decode_failed. Without this check the filter would read past the end.
        const image lying = image{std::vector<std::byte>(3), format::rgba8_unorm, {2, 2, 1}, 1, 1};
        CT_REQUIRE(!lying.consistent());

        auto bad = generate_mips(lying);
        CT_REQUIRE(!bad.has_value());
        CT_REQUIRE(bad.error().code == error_code::decode_failed);

        auto nothing = generate_mips(image{});
        CT_REQUIRE(!nothing.has_value());
        CT_REQUIRE(nothing.error().code == error_code::decode_failed);
    }

    void test_consistent_rejects_impossible_descriptions()
    {
        // Four levels of a 2x2 image cannot exist. The size happens to add up if each surplus level
        // claims a texel, so the check is on the description and not only on the byte count.
        const image deep = image{std::vector<std::byte>(4 + 1 + 1 + 1), format::r8_unorm, {2, 2, 1}, 4, 1};
        CT_REQUIRE(!deep.consistent());

        const image zero_extent = image{std::vector<std::byte>(0), format::r8_unorm, {0, 0, 1}, 1, 1};
        CT_REQUIRE(!zero_extent.consistent());

        const image no_format = image{std::vector<std::byte>(4), format::unknown, {2, 2, 1}, 1, 1};
        CT_REQUIRE(!no_format.consistent());

        const image good = image{std::vector<std::byte>(4 + 1), format::r8_unorm, {2, 2, 1}, 2, 1};
        CT_REQUIRE(good.consistent());
    }

} // namespace

int main()
{
    test_chain_shape();
    test_non_power_of_two_floors();
    test_axis_that_reaches_one_keeps_going();

    test_box_filter_averages();
    test_srgb_is_averaged_in_linear_light();
    test_alpha_weighting();
    test_three_dimensions_average_eight_texels();
    test_float_formats();
    test_layers_are_filtered_independently();
    test_regenerating_reproduces_itself();

    test_refuses_what_it_cannot_filter();
    test_consistent_rejects_impossible_descriptions();

    return 0;
}
