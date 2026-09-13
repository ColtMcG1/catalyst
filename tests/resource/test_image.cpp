/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file test_image.cpp
 * @brief Tier 2 of the asset system: catalyst::resource::load_image, over stb_image.
 * @details The fixtures are encoded images built byte by byte in scripts/gen_image_fixtures.py, so
 * every assertion here can name the texel it expects rather than checking that a decode merely
 * produced something. What is actually under test is the seam, not stb: the channel-count policy,
 * the sRGB flag, the format mapping, and the split between `unsupported_format` (nothing
 * recognised the bytes) and `decode_failed` (something did and then gave up), plus the hand-off
 * from a source decode to the mip filter. The cooked containers are a separate suite, since they
 * need no decoder and exist in builds this one does not -- see test_container.cpp.
 * License: MIT (see LICENSE).
 */

#include "../test_common.hpp"

#include <catalyst/resource/blob.hpp>
#include <catalyst/resource/image.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

using namespace catalyst::resource;

namespace
{
#include "image_fixtures.inc"

    /** @brief A fixture array as the byte span load_image takes. */
    template <std::size_t N>
    std::span<const std::byte> bytes_of(const std::uint8_t (&data)[N])
    {
        return std::as_bytes(std::span<const std::uint8_t, N>{data});
    }

    /** @brief One texel of a decoded 8-bit image, as four values regardless of channel count. */
    std::array<std::uint8_t, 4> texel(const image &img, std::size_t index, std::size_t channels)
    {
        const std::span<const std::byte> pixels = img.pixels();
        std::array<std::uint8_t, 4> out{};
        for (std::size_t c = 0; c < channels; ++c)
            out[c] = static_cast<std::uint8_t>(pixels[index * channels + c]);
        return out;
    }

    float float_texel(const image &img, std::size_t index, std::size_t component)
    {
        const std::span<const std::byte> pixels = img.pixels();
        float value = 0.0f;
        const std::size_t offset = (index * 4 + component) * sizeof(float);
        std::memcpy(&value, pixels.data() + offset, sizeof(float));
        return value;
    }

    // -----------------------------------------------------------------------------
    // The default decode: linear RGBA, whatever the file held
    // -----------------------------------------------------------------------------

    void test_decodes_rgba_png()
    {
        auto img = load_image(bytes_of(png_rgba_2x2));
        CT_REQUIRE(img.has_value());

        CT_REQUIRE(img->pixel_format() == format::rgba8_unorm);
        CT_REQUIRE(img->width() == 2);
        CT_REQUIRE(img->height() == 2);
        CT_REQUIRE(img->depth() == 1);

        // A source format carries no chain, so a decode is level 0 and one layer unless the caller
        // asks otherwise. See test_decodes_with_a_generated_chain.
        CT_REQUIRE(img->mip_levels() == 1);
        CT_REQUIRE(img->array_layers() == 1);
        CT_REQUIRE(!img->empty());

        // Tightly packed, so the size the image reports and the size it holds must agree. This is
        // what makes an upload a memcpy.
        CT_REQUIRE(img->pixels().size() == 2 * 2 * 4);
        CT_REQUIRE(img->level_size_bytes(0) == img->pixels().size());
        CT_REQUIRE(img->level(0).size() == img->pixels().size());

        CT_REQUIRE((texel(*img, 0, 4) == std::array<std::uint8_t, 4>{255, 0, 0, 255}));
        CT_REQUIRE((texel(*img, 1, 4) == std::array<std::uint8_t, 4>{0, 255, 0, 255}));
        CT_REQUIRE((texel(*img, 2, 4) == std::array<std::uint8_t, 4>{0, 0, 255, 255}));
        CT_REQUIRE((texel(*img, 3, 4) == std::array<std::uint8_t, 4>{255, 255, 255, 128}));
    }

    /** @brief Three channels are widened to four, opaque: `rgb8` is in no backend's intersection. */
    void test_rgb_is_widened_to_rgba()
    {
        auto img = load_image(bytes_of(png_rgb_2x2));
        CT_REQUIRE(img.has_value());

        CT_REQUIRE(img->pixel_format() == format::rgba8_unorm);
        CT_REQUIRE(img->pixels().size() == 2 * 2 * 4);

        CT_REQUIRE((texel(*img, 0, 4) == std::array<std::uint8_t, 4>{255, 0, 0, 255}));
        CT_REQUIRE((texel(*img, 3, 4) == std::array<std::uint8_t, 4>{10, 20, 30, 255}));

        // And it stays widened even when the caller asks to preserve channels, because the
        // alternative is a format nothing can sample.
        auto preserved = load_image(bytes_of(png_rgb_2x2), image_decode_options{.preserve_channels = true});
        CT_REQUIRE(preserved.has_value());
        CT_REQUIRE(preserved->pixel_format() == format::rgba8_unorm);
    }

    /** @brief By default a grayscale file also arrives as RGBA -- gray replicated, alpha opaque. */
    void test_grayscale_expands_by_default()
    {
        auto img = load_image(bytes_of(png_gray_2x2));
        CT_REQUIRE(img.has_value());

        CT_REQUIRE(img->pixel_format() == format::rgba8_unorm);
        CT_REQUIRE(img->pixels().size() == 2 * 2 * 4);

        CT_REQUIRE((texel(*img, 0, 4) == std::array<std::uint8_t, 4>{0, 0, 0, 255}));
        CT_REQUIRE((texel(*img, 1, 4) == std::array<std::uint8_t, 4>{64, 64, 64, 255}));
        CT_REQUIRE((texel(*img, 3, 4) == std::array<std::uint8_t, 4>{255, 255, 255, 255}));
    }

    // -----------------------------------------------------------------------------
    // preserve_channels
    // -----------------------------------------------------------------------------

    void test_preserve_channels_keeps_narrow_formats()
    {
        constexpr image_decode_options narrow{.preserve_channels = true};

        auto gray = load_image(bytes_of(png_gray_2x2), narrow);
        CT_REQUIRE(gray.has_value());
        CT_REQUIRE(gray->pixel_format() == format::r8_unorm);
        CT_REQUIRE(gray->pixels().size() == 2 * 2 * 1);
        CT_REQUIRE(texel(*gray, 1, 1)[0] == 64);

        auto gray_alpha = load_image(bytes_of(png_gray_alpha_2x2), narrow);
        CT_REQUIRE(gray_alpha.has_value());
        CT_REQUIRE(gray_alpha->pixel_format() == format::rg8_unorm);
        CT_REQUIRE(gray_alpha->pixels().size() == 2 * 2 * 2);
        CT_REQUIRE((texel(*gray_alpha, 0, 2) == std::array<std::uint8_t, 4>{10, 255, 0, 0}));
        CT_REQUIRE((texel(*gray_alpha, 3, 2) == std::array<std::uint8_t, 4>{40, 0, 0, 0}));

        // The narrow formats must still be the ones format_size_bytes agrees with, or every
        // downstream size calculation is wrong.
        CT_REQUIRE(format_size_bytes(gray->pixel_format()) == 1);
        CT_REQUIRE(format_size_bytes(gray_alpha->pixel_format()) == 2);
    }

    // -----------------------------------------------------------------------------
    // srgb
    // -----------------------------------------------------------------------------

    void test_srgb_selects_the_srgb_format_without_touching_texels()
    {
        auto linear = load_image(bytes_of(png_rgba_2x2));
        auto srgb = load_image(bytes_of(png_rgba_2x2), image_decode_options{.srgb = true});
        CT_REQUIRE(linear.has_value());
        CT_REQUIRE(srgb.has_value());

        CT_REQUIRE(linear->pixel_format() == format::rgba8_unorm);
        CT_REQUIRE(srgb->pixel_format() == format::rgba8_unorm_srgb);

        // The flag is a statement about interpretation, not a conversion: the bytes are identical.
        CT_REQUIRE(linear->pixels().size() == srgb->pixels().size());
        CT_REQUIRE(std::equal(linear->pixels().begin(), linear->pixels().end(), srgb->pixels().begin()));
    }

    /** @brief There is no `r8_unorm_srgb`, so asking for sRGB wins over asking to stay narrow. */
    void test_srgb_forces_rgba_over_preserve_channels()
    {
        auto img = load_image(bytes_of(png_gray_2x2), image_decode_options{.srgb = true, .preserve_channels = true});
        CT_REQUIRE(img.has_value());
        CT_REQUIRE(img->pixel_format() == format::rgba8_unorm_srgb);
        CT_REQUIRE(img->pixels().size() == 2 * 2 * 4);
    }

    // -----------------------------------------------------------------------------
    // HDR
    // -----------------------------------------------------------------------------

    void test_decodes_radiance_hdr_as_float()
    {
        auto img = load_image(bytes_of(hdr_2x2));
        CT_REQUIRE(img.has_value());

        CT_REQUIRE(img->pixel_format() == format::rgba32_float);
        CT_REQUIRE(img->width() == 2);
        CT_REQUIRE(img->height() == 2);
        CT_REQUIRE(img->pixels().size() == 2 * 2 * 4 * sizeof(float));
        CT_REQUIRE(format_size_bytes(img->pixel_format()) == 16);

        // The fixture's exponents are chosen so the decoded values are exact in binary32.
        CT_REQUIRE(float_texel(*img, 0, 0) == 1.0f);
        CT_REQUIRE(float_texel(*img, 0, 1) == 0.5f);
        CT_REQUIRE(float_texel(*img, 0, 2) == 0.25f);
        CT_REQUIRE(float_texel(*img, 0, 3) == 1.0f);

        // A zero exponent is Radiance's encoding of black, not a value scaled by 2^-136.
        CT_REQUIRE(float_texel(*img, 3, 0) == 0.0f);
        CT_REQUIRE(float_texel(*img, 3, 1) == 0.0f);

        // sRGB means nothing for a float image and must not change the format it picks.
        auto with_srgb = load_image(bytes_of(hdr_2x2), image_decode_options{.srgb = true});
        CT_REQUIRE(with_srgb.has_value());
        CT_REQUIRE(with_srgb->pixel_format() == format::rgba32_float);
    }

    // -----------------------------------------------------------------------------
    // Failures
    // -----------------------------------------------------------------------------

    /** @brief Bytes no codec recognises are `unsupported_format`, not a decode failure. */
    void test_unrecognised_bytes_are_unsupported_format()
    {
        constexpr std::string_view text = "this is not an image, it is a sentence about one.";
        const auto span = std::as_bytes(std::span{text.data(), text.size()});

        auto img = load_image(span);
        CT_REQUIRE(!img.has_value());
        CT_REQUIRE(img.error().code == error_code::unsupported_format);

        // stb's own reason is kept, which is the whole point of error::detail.
        CT_REQUIRE(!img.error().detail.empty());
    }

    /** @brief A well-formed header over a ruined pixel stream is `decode_failed`. */
    void test_corrupt_pixels_are_decode_failed()
    {
        auto img = load_image(bytes_of(png_corrupt_2x2));
        CT_REQUIRE(!img.has_value());
        CT_REQUIRE(img.error().code == error_code::decode_failed);
        CT_REQUIRE(!img.error().detail.empty());
    }

    void test_empty_input_is_decode_failed()
    {
        auto img = load_image(std::span<const std::byte>{});
        CT_REQUIRE(!img.has_value());
        CT_REQUIRE(img.error().code == error_code::decode_failed);
    }

    // -----------------------------------------------------------------------------
    // The blob overload -- the path a real caller takes out of the vfs
    // -----------------------------------------------------------------------------

    void test_decodes_from_a_blob()
    {
        std::vector<std::byte> storage(std::size(png_rgba_2x2));
        for (std::size_t i = 0; i < storage.size(); ++i)
            storage[i] = static_cast<std::byte>(png_rgba_2x2[i]);

        blob data = blob::adopt(std::move(storage));

        auto img = load_image(data, image_decode_options{.srgb = true});
        CT_REQUIRE(img.has_value());
        CT_REQUIRE(img->pixel_format() == format::rgba8_unorm_srgb);
        CT_REQUIRE(img->width() == 2);
        CT_REQUIRE((texel(*img, 0, 4) == std::array<std::uint8_t, 4>{255, 0, 0, 255}));

        // The image owns its pixels: releasing the encoded bytes it came from changes nothing.
        data.reset();
        CT_REQUIRE((texel(*img, 3, 4) == std::array<std::uint8_t, 4>{255, 255, 255, 128}));
    }

    /** @brief Decoding is reentrant, which it must be before Tier 2 moves it to a worker. */
    void test_repeated_decodes_are_independent()
    {
        auto first = load_image(bytes_of(png_rgba_2x2));
        auto failed = load_image(bytes_of(png_corrupt_2x2));
        auto second = load_image(bytes_of(png_rgba_2x2));

        CT_REQUIRE(first.has_value());
        CT_REQUIRE(!failed.has_value());
        CT_REQUIRE(second.has_value());

        // A failure between two good decodes must leave no residue in the second.
        CT_REQUIRE(second->pixels().size() == first->pixels().size());
        CT_REQUIRE(std::equal(first->pixels().begin(), first->pixels().end(), second->pixels().begin()));
    }

    // -----------------------------------------------------------------------------
    // The hand-off from a source decode to the mip filter
    // -----------------------------------------------------------------------------

    void test_decodes_with_a_generated_chain()
    {
        // A 4x4 of alternating black and white columns, decoded as sRGB. Every 2x2 neighbourhood is
        // half of each, so level 1 is half the light -- which sRGB spells 188. Getting 128 here
        // would mean the chain was built by averaging the stored bytes, and getting one level would
        // mean the flag never reached the filter.
        auto img = load_image(bytes_of(png_srgb_4x4), {.srgb = true, .generate_mips = true});
        CT_REQUIRE(img.has_value());

        CT_REQUIRE(img->pixel_format() == format::rgba8_unorm_srgb);
        CT_REQUIRE(img->mip_levels() == 3);
        CT_REQUIRE(img->array_layers() == 1);
        CT_REQUIRE(img->consistent());
        CT_REQUIRE(img->pixels().size() == (4 * 4 + 2 * 2 + 1) * 4);

        CT_REQUIRE((texel(*img, 0, 4) == std::array<std::uint8_t, 4>{0, 0, 0, 255}));

        const std::span<const std::byte> level1 = img->level(1);
        CT_REQUIRE(level1.size() == 2 * 2 * 4);
        CT_REQUIRE(static_cast<std::uint8_t>(level1[0]) == 188);
        CT_REQUIRE(static_cast<std::uint8_t>(level1[3]) == 255); // alpha is linear, never encoded

        // Off by default: a chain costs a third again in memory and upload, and plenty of images
        // are never sampled minified.
        auto single = load_image(bytes_of(png_srgb_4x4), {.srgb = true});
        CT_REQUIRE(single.has_value());
        CT_REQUIRE(single->mip_levels() == 1);
    }

} // namespace

int main()
{
    test_decodes_rgba_png();
    test_rgb_is_widened_to_rgba();
    test_grayscale_expands_by_default();
    test_preserve_channels_keeps_narrow_formats();
    test_srgb_selects_the_srgb_format_without_touching_texels();
    test_srgb_forces_rgba_over_preserve_channels();
    test_decodes_radiance_hdr_as_float();
    test_unrecognised_bytes_are_unsupported_format();
    test_corrupt_pixels_are_decode_failed();
    test_empty_input_is_decode_failed();
    test_decodes_from_a_blob();
    test_repeated_decodes_are_independent();
    test_decodes_with_a_generated_chain();

    return 0;
}
