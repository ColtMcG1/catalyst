/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file image.hpp
 * @brief @ref catalyst::resource::image -- decoded pixels on the CPU, described in the renderer's
 * own vocabulary so that uploading one is a copy and not a translation.
 * @details **There is no `resource::image_format`.** The pixel format is `rendering::format` and the
 * dimensions are `rendering::extent3d`, both re-exported below from `<catalyst/rendering/types.hpp>`.
 * That header is `constexpr` enums and templates only, so using it costs an include path and no
 * link dependency -- see handle.hpp for why that matters and docs/resource.md for the module graph.
 *
 * Defining a parallel `rgba8 | rgb8 | rgba16f | ...` enum here would have meant a translation
 * function, a set of formats the renderer cannot actually sample (`rgb8` is not in any backend's
 * intersection), and two enums to keep in step every time a format is added. The decoder's job is
 * to produce something `create_texture` will take; the honest way to say that is to use the type
 * `create_texture` takes.
 *
 * **Three ways to get one, and they are not interchangeable.**
 *
 *   - @ref load_image over a *source* format -- PNG, JPEG, BMP, TGA, PSD, GIF, PIC, PNM, Radiance
 *     `.hdr` -- decodes through stb_image to a single uncompressed level. This is what an artist's
 *     working tree holds and what the editor reads.
 *   - @ref load_image over a *cooked container* -- KTX2 or DDS -- reads the mip chain, the array
 *     layers and the block-compressed format that were decided when the asset was cooked. stb has
 *     no notion of either container, so these are this module's own readers; they are pure
 *     rearrangement, with no codec behind them, which is why they are in the build even when
 *     `CATALYST_RESOURCE_STB=OFF`.
 *   - @ref generate_mips builds a chain for an image that arrived without one. The run-time answer
 *     to the problem a cooked container solves at build time.
 *
 * All three come back through one `load_image` call, which sniffs the container from its magic
 * number. A caller does not choose a reader.
 *
 * **Block-compressed formats are ordinary here.** An `image` holding BC7 is a normal `image`; what
 * changes is the arithmetic, since a texel has no size and only a 4x4 block does. @ref
 * level_size_bytes and @ref packed_size_bytes are block-aware and are the only correct way to size
 * a level. @ref generate_mips is the one operation that refuses them, because filtering compressed
 * texels means decompressing them.
 *
 * The GPU upload that turns an `image` into a `rendering::texture` remains Tier 3, in the separate
 * `catalyst_resource_gpu` target that *does* link the renderer. See docs/resource.md.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/resource/blob.hpp>
#include <catalyst/resource/error.hpp>
#include <catalyst/resource/handle.hpp>
#include <catalyst/resource/loader.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <vector>

namespace catalyst::resource
{
    /// @brief The renderer's pixel-format enum. Defined once, in `<catalyst/rendering/types.hpp>`.
    using rendering::format;

    /// @brief The renderer's 3D extent. Defined once, in `<catalyst/rendering/types.hpp>`.
    using rendering::extent3d;

    /// @brief Bytes per texel of a format; 0 for a block-compressed one. Re-exported for the same reason.
    using rendering::format_size_bytes;

    /// @brief Whether a format stores texels in fixed-size blocks.
    using rendering::is_block_compressed;

    /// @brief Texel width of one compression block; 1 when uncompressed.
    using rendering::format_block_width;

    /// @brief Texel height of one compression block; 1 when uncompressed.
    using rendering::format_block_height;

    /// @brief Bytes per compression block, or per texel when uncompressed.
    using rendering::format_block_size_bytes;

    /// @brief Tightly packed size of one level of one layer. Correct for both kinds of format.
    using rendering::format_image_size_bytes;

    /// @brief Whether a format's texels are to be read as sRGB.
    using rendering::is_srgb_format;

    /// @brief The sRGB spelling of a format, where it has one.
    using rendering::to_srgb_format;

    // -----------------------------------------------------------------------------
    // Mip arithmetic, free of any particular image
    // -----------------------------------------------------------------------------

    /**
     * @brief Dimensions of mip level @p level below @p extent: each axis halved and floored, never
     * below 1.
     * @details The convention every graphics API uses, and the one this module's containers are
     * validated against. Note that it is floor and not ceil, so a 5-texel axis becomes 2 and not 3,
     * and that an axis reaching 1 stays there while the others keep shrinking -- which is what makes
     * a 256x1 image have nine levels rather than one.
     */
    [[nodiscard]] constexpr extent3d mip_extent(extent3d extent, std::uint32_t level) noexcept
    {
        for (std::uint32_t i = 0; i < level; ++i)
        {
            extent.width = extent.width > 1 ? extent.width / 2 : 1;
            extent.height = extent.height > 1 ? extent.height / 2 : 1;
            extent.depth = extent.depth > 1 ? extent.depth / 2 : 1;
        }
        return extent;
    }

    /**
     * @brief Number of levels in a complete chain from @p extent down to 1x1x1.
     * @details `floor(log2(max axis)) + 1`, computed by halving rather than with a logarithm so it
     * is exact and `constexpr` without a float round trip. Zero for a degenerate extent, which is
     * the answer that makes a caller's loop do nothing rather than produce one empty level.
     */
    [[nodiscard]] constexpr std::uint32_t max_mip_levels(extent3d extent) noexcept
    {
        if (extent.width == 0 || extent.height == 0 || extent.depth == 0)
            return 0;

        std::uint32_t levels = 1;
        while (extent.width > 1 || extent.height > 1 || extent.depth > 1)
        {
            extent = mip_extent(extent, 1);
            ++levels;
        }
        return levels;
    }

    /**
     * @brief Tightly packed bytes a whole image occupies: every level of every layer, laid end to
     * end in upload order.
     * @details The size an @ref image's pixel buffer must have to be consistent with its own
     * description, which is exactly what the container readers check a file's level index against
     * before they trust a byte of it.
     */
    [[nodiscard]] constexpr std::uint64_t packed_size_bytes(format fmt, extent3d extent, std::uint32_t mip_levels = 1,
                                                            std::uint32_t array_layers = 1) noexcept
    {
        std::uint64_t chain = 0;
        for (std::uint32_t level = 0; level < mip_levels; ++level)
            chain += format_image_size_bytes(fmt, mip_extent(extent, level));

        return chain * array_layers;
    }

    /**
     * @class image
     * @brief A decoded image held in host memory: pixels, their format, their extent and their mip
     * chain.
     * @details Mip levels are stored back to back in one allocation, level 0 first, each level's
     * rows tightly packed. Array layers are whole mip chains laid end to end, so layer 1 begins
     * after the last level of layer 0. That is the layout `rendering::create_texture` and
     * `rendering::transfer_batch::upload` expect, so residency is a `memcpy` per level with no
     * repacking.
     *
     * It is worth being explicit that this is *layer-major*, because KTX2 is the other way round --
     * it stores every layer of level 0, then every layer of level 1 -- and the reader transposes.
     * DDS is already layer-major and does not.
     */
    class image
    {
    public:
        image() = default;

        /**
         * @brief Adopts a decoded pixel buffer.
         * @param pixels All mip levels of all layers, tightly packed, layer-major and level 0 first.
         * @param fmt The texel format. Must not be `format::unknown`.
         * @param extent Level 0's dimensions; `depth` is 1 for a 2D image.
         * @param mip_levels How many levels @p pixels contains per layer. At least 1.
         * @param array_layers Array slices, each a whole mip chain after the previous one's.
         */
        image(std::vector<std::byte> pixels, format fmt, extent3d extent, std::uint32_t mip_levels = 1,
              std::uint32_t array_layers = 1) noexcept
            : pixels_(std::move(pixels)), format_(fmt), extent_(extent), mip_levels_(mip_levels),
              array_layers_(array_layers)
        {
        }

        [[nodiscard]] format pixel_format() const noexcept { return format_; }
        [[nodiscard]] extent3d extent() const noexcept { return extent_; }
        [[nodiscard]] std::uint32_t width() const noexcept { return extent_.width; }
        [[nodiscard]] std::uint32_t height() const noexcept { return extent_.height; }
        [[nodiscard]] std::uint32_t depth() const noexcept { return extent_.depth; }
        [[nodiscard]] std::uint32_t mip_levels() const noexcept { return mip_levels_; }
        [[nodiscard]] std::uint32_t array_layers() const noexcept { return array_layers_; }

        /** @brief Every level of every layer, in upload order. */
        [[nodiscard]] std::span<const std::byte> pixels() const noexcept { return pixels_; }

        /** @brief True when the image carries no pixels -- a default-constructed one, or a decode
         * that produced nothing. */
        [[nodiscard]] bool empty() const noexcept { return pixels_.empty(); }

        /** @brief True when @ref pixel_format stores texels in 4x4 blocks, in which case only the
         * block-aware sizes below mean anything. */
        [[nodiscard]] bool compressed() const noexcept { return is_block_compressed(format_); }

        /** @brief Dimensions of mip level @p level, each halved and floored, never below 1. */
        [[nodiscard]] extent3d extent_at(std::uint32_t level) const noexcept;

        /**
         * @brief Tightly packed byte size of one layer of mip level @p level.
         * @details Block-aware: a compressed level rounds each axis up to a whole block, so the
         * 2x2 and 1x1 tail of a BC7 chain are 16 bytes each and not 4 and 1.
         */
        [[nodiscard]] std::size_t level_size_bytes(std::uint32_t level) const noexcept;

        /** @brief The bytes of one mip level of one array layer, or an empty span if either index
         * is out of range, or if the pixel buffer is shorter than the description implies. */
        [[nodiscard]] std::span<const std::byte> level(std::uint32_t level, std::uint32_t layer = 0) const noexcept;

        /**
         * @brief Whether the pixel buffer is exactly the size this image's own description implies.
         * @details False means the description and the bytes disagree, which for anything that came
         * out of @ref load_image cannot happen -- the readers check it before returning. It is here
         * for the caller who built an `image` by hand and for the tests that check the readers did.
         */
        [[nodiscard]] bool consistent() const noexcept;

    private:
        std::vector<std::byte> pixels_;
        format format_ = format::unknown;
        extent3d extent_{};
        std::uint32_t mip_levels_ = 1;
        std::uint32_t array_layers_ = 1;
    };

    /** @brief A handle to an @ref image held by a `registry<image>`. */
    using image_handle = asset_handle<image>;

    // -----------------------------------------------------------------------------
    // Decoding
    // -----------------------------------------------------------------------------

    /**
     * @struct image_decode_options
     * @brief The few decisions a decoder cannot make for itself.
     * @details Everything else about a decode is dictated by the file. These are not: whether the
     * texels are meant to be read as sRGB is a property of how the image will be *used*, whether a
     * one- or two-channel file is worth keeping narrow depends on what the shader sampling it
     * expects, and whether a single-level source should arrive with a chain depends on whether the
     * caller is about to sample it minified. All three default to the conservative answer.
     */
    struct image_decode_options
    {
        /**
         * @brief Return the `_srgb` variant of the chosen format.
         * @details Off by default, and deliberately not inferred. A PNG carries no reliable
         * statement of intent -- albedo and a normal map are the same bytes in the same container,
         * and getting it wrong is a gamma bug that survives all the way to the framebuffer. The
         * caller knows which it asked for; the decoder does not.
         *
         * For a source format, only `rgba8_unorm` has an sRGB counterpart in @ref rendering::format,
         * so setting this forces the four-channel expansion described in @ref preserve_channels
         * regardless of that flag. It is ignored for `.hdr`, which is float and already linear.
         *
         * For a *cooked container* it only ever promotes: a KTX2 or DDS that already declares an
         * sRGB format keeps it whether this is set or not, because that spelling is a statement its
         * cooker made deliberately and demoting it silently would be the worse failure. Setting this
         * on a container that declares `bc7_unorm` yields `bc7_unorm_srgb` -- which is the common
         * case of a colour texture written by a tool that did not record the distinction.
         */
        bool srgb = false;

        /**
         * @brief Keep one- and two-channel sources narrow instead of widening them to RGBA.
         * @details Off by default: a decode yields `rgba8_unorm` whatever the file held, which is
         * the format every backend can sample and the one callers are least likely to get wrong.
         * Turning it on gives `r8_unorm` for a grayscale file and `rg8_unorm` for gray+alpha --
         * a quarter and a half the upload for a mask or a packed two-channel map, at the cost of
         * the caller having to swizzle in the shader.
         *
         * Three-channel sources are widened to four either way. `rgb8` is in no backend's sampling
         * intersection, which is the same reason this module has no pixel-format enum of its own.
         *
         * Ignored by the container readers, which do not choose a format at all.
         */
        bool preserve_channels = false;

        /**
         * @brief Generate a full mip chain for a decode that produced a single level.
         * @details Off by default, because a chain costs a third again in memory and upload and
         * plenty of images -- a UI atlas, a lookup table, a full-screen overlay -- are never sampled
         * minified and want none.
         *
         * A container that already carries its own chain is left exactly as it is: this flag asks
         * for the levels a file did not have, and never regenerates or overrides the ones a cooker
         * produced. When it does run it is @ref generate_mips with @ref mip_options defaults, which
         * means sRGB-correct averaging; see there for why that is not the same as averaging the
         * bytes.
         *
         * A failure to generate -- a block-compressed container, a format with no filter -- fails
         * the whole decode rather than quietly handing back one level, because a caller that asked
         * for a chain and got one level has a texture that shimmers and no indication why.
         */
        bool generate_mips = false;
    };

    /**
     * @brief Decodes @p bytes into an @ref image, sniffing the container from its magic number.
     * @param bytes A complete encoded image. Decoding is done entirely from memory.
     * @param options See @ref image_decode_options; the default is a single linear RGBA level.
     * @return An @ref image, or
     *         @ref error_code::unsupported_format when no reader in this build claims the bytes,
     *         @ref error_code::decode_failed when one claimed them and then rejected them -- with
     *         the reader's own reason in @ref error::detail.
     *
     * @details Three readers sit behind this call and the magic number picks between them:
     *
     *   | Leading bytes            | Reader   | Yields                                  |
     *   | ------------------------ | -------- | --------------------------------------- |
     *   | `AB 4B 54 58 20 32 30..` | KTX2     | the file's chain, layers and faces       |
     *   | `44 44 53 20` (`DDS `)   | DDS      | the file's chain and array slices        |
     *   | anything else            | stb_image| one uncompressed level                   |
     *
     * The distinction between the two failure codes is maintained by every reader, and is the one
     * that tells a build configured wrong from a file that is corrupt: `unsupported_format` means
     * nothing here knows what these bytes are -- or knows and cannot handle this variant of them,
     * such as a supercompressed KTX2 -- while `decode_failed` means a reader recognised them and
     * the contents did not hold up.
     */
    [[nodiscard]] std::expected<image, error> load_image(std::span<const std::byte> bytes,
                                                         image_decode_options options = {});

    /** @brief @ref load_image over a @ref blob straight from the vfs. */
    [[nodiscard]] std::expected<image, error> load_image(const blob &data, image_decode_options options = {});

    // -----------------------------------------------------------------------------
    // Mip generation
    // -----------------------------------------------------------------------------

    /**
     * @struct mip_options
     * @brief How @ref generate_mips builds the levels below level 0.
     */
    struct mip_options
    {
        /**
         * @brief How many levels the result should have, including level 0. Zero means a complete
         * chain down to 1x1.
         * @details Clamped to @ref max_mip_levels; asking for more is not an error, because the
         * caller who wants "a chain, but stop at 4x4" and the caller who wants "all of it" should
         * not have to compute the same number the function already knows.
         */
        std::uint32_t levels = 0;

        /**
         * @brief Convert to linear before averaging and back afterwards when the format is sRGB.
         * @details **On by default, and this is the flag that matters.** sRGB texels are not
         * proportional to light: averaging the stored bytes of 0 and 255 gives 128, which is about
         * 22% of the light the correct answer -- 188 -- represents. The visible result is a chain
         * that darkens as it descends, so a checkerboard fades to grey at the wrong grey and every
         * minified surface is duller than the one next to it that happened to keep level 0.
         *
         * Turn it off only to reproduce a chain some other tool built incorrectly. Decoding is a
         * 256-entry table lookup, which is exact rather than approximate because that is the whole
         * domain; encoding is a `pow` per channel written, and there are a quarter as many of those
         * as there are reads.
         *
         * It does nothing for a linear format, where averaging the bytes is already correct, and
         * nothing for the alpha channel of an sRGB one, which is linear by definition.
         */
        bool srgb_aware = true;

        /**
         * @brief Weight each texel's colour by its alpha when averaging.
         * @details Off by default. It fixes the second-most-common mip artefact after the sRGB one:
         * a cutout texture -- foliage, a decal, a font atlas -- usually has arbitrary colour in its
         * fully transparent texels, often black, and an unweighted average drags that colour into
         * the visible neighbours. Every level down, the leaves get a darker fringe.
         *
         * It is off by default because it is not free of consequence: it changes the colour of any
         * texel whose neighbourhood has varying alpha, which for an image that uses alpha as data
         * rather than as coverage -- a packed roughness or height channel -- is corruption. The
         * caller knows which of the two it has.
         *
         * Alpha itself is averaged unweighted either way. A neighbourhood that is entirely
         * transparent has no colour to preserve and keeps the plain average, since the alternative
         * is a division by zero.
         */
        bool alpha_weighted = false;
    };

    /**
     * @brief Builds a mip chain from @p source's level 0.
     * @param source The image to filter. Its own levels below 0, if it has any, are ignored.
     * @param options See @ref mip_options.
     * @return A new @ref image with the same format, extent and layer count, and
     *         @ref mip_options::levels levels, or
     *         @ref error_code::unsupported_format when the format cannot be filtered,
     *         @ref error_code::decode_failed when @p source's own bytes do not match its
     *         description.
     *
     * @details A box filter: each texel is the average of the up-to-2x2x2 texels above it, halving
     * every axis that is larger than 1, with each level filtered from the level before it rather
     * than from level 0. For an odd axis that means the last row or column is dropped rather than
     * blended, which is the same floor convention @ref mip_extent and every graphics API use, and
     * it is why a cooked chain from an offline tool with a better filter is worth having for
     * anything that matters visually. This is the runtime answer, not the good one.
     *
     * **Regenerates from level 0.** Passing an image that already has a chain rebuilds it rather
     * than extending it. Levels below 0 in @p source are read as input to nothing and the result
     * does not depend on them, so running this on its own output reproduces that output exactly.
     *
     * **Block-compressed formats are refused** with @ref error_code::unsupported_format. Filtering
     * them means a decompress, a filter and a recompress, and a real BC encoder is a large thing
     * with quality settings of its own that belongs in a cooker and not behind a convenience
     * function. A cooked container already carries the chain this would be trying to rebuild.
     *
     * Supported: the 8-bit unorm formats, their sRGB spellings, and the 32-bit float formats.
     * `rgba16_float` is absent for the same reason a half-float converter is absent from this
     * module -- see docs/resource.md.
     */
    [[nodiscard]] std::expected<image, error> generate_mips(const image &source, mip_options options = {});

    // -----------------------------------------------------------------------------
    // The loader seam
    // -----------------------------------------------------------------------------

    /**
     * @brief Teaches @ref load how to produce an @ref image, so `load<image>(files, assets, name)`
     * is one call.
     * @details Thin on purpose: it is @ref load_image and nothing else. The interesting half of
     * loading -- the cache probe, the read, the insertion, the reference the caller ends up owning
     * -- is in @ref load and is the same for every asset type, which is the entire argument for the
     * seam existing.
     *
     * It ignores its @ref load_context. An image has no dependencies; a material will not.
     */
    template <>
    struct loader<image>
    {
        using options = image_decode_options;

        [[nodiscard]] static std::expected<image, error> decode(std::span<const std::byte> bytes,
                                                                const load_context &context, const options &opt);
    };

} // namespace catalyst::resource
