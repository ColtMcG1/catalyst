/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file test_container.cpp
 * @brief The cooked-container readers: KTX2 and DDS, and the block-compressed arithmetic they
 * depend on.
 * @details These run in every build, including `CATALYST_RESOURCE_STB=OFF`, because neither reader
 * has a codec behind it. The fixtures are assembled field by field in scripts/gen_image_fixtures.py
 * and every level's payload is a run of one byte naming the level and layer it came from, so an
 * assertion about where a surface landed is a single byte comparison rather than a checksum.
 *
 * What is under test is the three things a reader of these formats gets wrong: the format table,
 * the level and layer arithmetic for block formats, and -- for KTX2 -- the transposition between a
 * level-major file and a layer-major `image`.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "../test_common.hpp"

#include <catalyst/resource/image.hpp>

#include <array>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

using namespace catalyst::resource;

namespace
{
#include "container_fixtures.inc"

    template <std::size_t N>
    std::span<const std::byte> bytes_of(const std::uint8_t (&data)[N])
    {
        return std::as_bytes(std::span<const std::uint8_t, N>{data});
    }

    /** @brief The single byte the generator filled level @p level of layer @p layer with. */
    constexpr std::uint8_t marker(std::uint32_t level, std::uint32_t layer)
    {
        return static_cast<std::uint8_t>(0x10 + level * 0x10 + layer);
    }

    /** @brief True when every byte of @p bytes equals @p value, and there is at least one. */
    bool all_equal(std::span<const std::byte> bytes, std::uint8_t value)
    {
        if (bytes.empty())
            return false;

        for (const std::byte b : bytes)
        {
            if (static_cast<std::uint8_t>(b) != value)
                return false;
        }
        return true;
    }

    // -----------------------------------------------------------------------------
    // The block arithmetic every reader below leans on
    // -----------------------------------------------------------------------------

    void test_block_format_arithmetic()
    {
        // A texel of BC7 has no size. That is the whole reason format_image_size_bytes exists, and
        // the reason a per-texel multiplication at a call site would be wrong rather than merely
        // imprecise.
        CT_REQUIRE(format_size_bytes(format::bc7_unorm) == 0);
        CT_REQUIRE(is_block_compressed(format::bc7_unorm));
        CT_REQUIRE(!is_block_compressed(format::rgba8_unorm));

        CT_REQUIRE(format_block_size_bytes(format::bc1_rgba_unorm) == 8);
        CT_REQUIRE(format_block_size_bytes(format::bc7_unorm) == 16);
        CT_REQUIRE(format_block_size_bytes(format::rgba8_unorm) == 4); // == the texel size
        CT_REQUIRE(format_block_width(format::bc3_unorm) == 4);
        CT_REQUIRE(format_block_height(format::rgba8_unorm) == 1);

        // Rounding up to whole blocks is the part a per-texel size cannot express: 8x8, 4x4, 2x2
        // and 1x1 are four, one, one and one block.
        CT_REQUIRE(format_image_size_bytes(format::bc7_unorm, extent3d{8, 8, 1}) == 64);
        CT_REQUIRE(format_image_size_bytes(format::bc7_unorm, extent3d{4, 4, 1}) == 16);
        CT_REQUIRE(format_image_size_bytes(format::bc7_unorm, extent3d{2, 2, 1}) == 16);
        CT_REQUIRE(format_image_size_bytes(format::bc7_unorm, extent3d{1, 1, 1}) == 16);
        CT_REQUIRE(format_image_size_bytes(format::bc7_unorm, extent3d{5, 5, 1}) == 64); // 2x2 blocks

        CT_REQUIRE(format_image_size_bytes(format::rgba8_unorm, extent3d{4, 4, 1}) == 64);
        CT_REQUIRE(format_image_size_bytes(format::rgba8_unorm, extent3d{4, 4, 3}) == 192);

        // The complete BC7 chain of an 8x8 image: 64 + 16 + 16 + 16.
        CT_REQUIRE(packed_size_bytes(format::bc7_unorm, extent3d{8, 8, 1}, 4) == 112);
        CT_REQUIRE(packed_size_bytes(format::bc7_unorm, extent3d{8, 8, 1}, 4, 2) == 224);

        CT_REQUIRE(max_mip_levels(extent3d{8, 8, 1}) == 4);
        CT_REQUIRE(max_mip_levels(extent3d{256, 1, 1}) == 9); // the short axis does not stop the chain
        CT_REQUIRE(max_mip_levels(extent3d{1, 1, 1}) == 1);
        CT_REQUIRE(max_mip_levels(extent3d{0, 4, 1}) == 0);

        CT_REQUIRE(mip_extent(extent3d{5, 5, 1}, 1) == (extent3d{2, 2, 1})); // floor, not ceil
        CT_REQUIRE(mip_extent(extent3d{8, 1, 1}, 2) == (extent3d{2, 1, 1}));
    }

    // -----------------------------------------------------------------------------
    // KTX2
    // -----------------------------------------------------------------------------

    void test_ktx2_reads_a_chain()
    {
        auto img = load_image(bytes_of(ktx2_rgba8_4x4));
        CT_REQUIRE(img.has_value());

        CT_REQUIRE(img->pixel_format() == format::rgba8_unorm);
        CT_REQUIRE(img->extent() == (extent3d{4, 4, 1}));
        CT_REQUIRE(img->mip_levels() == 3);
        CT_REQUIRE(img->array_layers() == 1);
        CT_REQUIRE(img->consistent());

        CT_REQUIRE(img->level(0).size() == 4 * 4 * 4);
        CT_REQUIRE(img->level(1).size() == 2 * 2 * 4);
        CT_REQUIRE(img->level(2).size() == 1 * 1 * 4);

        // The fixture stores its levels smallest-first, the way real writers do. Getting these in
        // the right order proves the reader used the level index rather than assuming a layout.
        CT_REQUIRE(all_equal(img->level(0), marker(0, 0)));
        CT_REQUIRE(all_equal(img->level(1), marker(1, 0)));
        CT_REQUIRE(all_equal(img->level(2), marker(2, 0)));
    }

    void test_ktx2_transposes_layers()
    {
        // The substance of the KTX2 reader. The file is level-major -- layer 0 and layer 1 of level
        // 0, then layer 0 and layer 1 of level 1 -- and an image is layer-major. If the reader
        // copied the file straight through, level(1, 0) would hold layer 1's level 0.
        auto img = load_image(bytes_of(ktx2_bc7_8x8_array));
        CT_REQUIRE(img.has_value());

        CT_REQUIRE(img->pixel_format() == format::bc7_unorm);
        CT_REQUIRE(img->compressed());
        CT_REQUIRE(img->extent() == (extent3d{8, 8, 1}));
        CT_REQUIRE(img->mip_levels() == 2);
        CT_REQUIRE(img->array_layers() == 2);
        CT_REQUIRE(img->consistent());

        CT_REQUIRE(img->level(0, 0).size() == 64); // 2x2 blocks of 16 bytes
        CT_REQUIRE(img->level(1, 0).size() == 16); // 4x4 is one block

        CT_REQUIRE(all_equal(img->level(0, 0), marker(0, 0)));
        CT_REQUIRE(all_equal(img->level(1, 0), marker(1, 0)));
        CT_REQUIRE(all_equal(img->level(0, 1), marker(0, 1)));
        CT_REQUIRE(all_equal(img->level(1, 1), marker(1, 1)));

        // Layer-major means layer 1's chain starts where layer 0's ends.
        CT_REQUIRE(img->pixels().size() == (64 + 16) * 2);
        CT_REQUIRE(static_cast<std::uint8_t>(img->pixels()[80]) == marker(0, 1));
    }

    void test_ktx2_faces_become_layers()
    {
        auto img = load_image(bytes_of(ktx2_bc1_cube));
        CT_REQUIRE(img.has_value());

        // A cube map is six layers plus an interpretation, and the interpretation belongs to the
        // texture view the GPU bridge makes, not to a block of host memory.
        CT_REQUIRE(img->pixel_format() == format::bc1_rgba_unorm);
        CT_REQUIRE(img->array_layers() == 6);
        CT_REQUIRE(img->mip_levels() == 1);
        CT_REQUIRE(img->level(0, 0).size() == 8); // one BC1 block
        CT_REQUIRE(img->consistent());

        for (std::uint32_t face = 0; face < 6; ++face)
            CT_REQUIRE(all_equal(img->level(0, face), marker(0, face)));
    }

    void test_ktx2_level_count_zero_is_one_stored_level()
    {
        // levelCount == 0 asks the application to generate the chain. It is a request, not a
        // promise about the file, and honouring it silently would triple the memory of every such
        // asset -- so the file reads as the one level it actually stores.
        auto img = load_image(bytes_of(ktx2_no_levels));
        CT_REQUIRE(img.has_value());
        CT_REQUIRE(img->mip_levels() == 1);
        CT_REQUIRE(img->consistent());

        // ...and the caller's own flag is what decides, exactly as it would for a PNG.
        auto chained = load_image(bytes_of(ktx2_no_levels), {.generate_mips = true});
        CT_REQUIRE(chained.has_value());
        CT_REQUIRE(chained->mip_levels() == 3);
        CT_REQUIRE(chained->consistent());
    }

    void test_ktx2_rejects_what_it_cannot_read()
    {
        // Supercompressed: recognised, and refused as a build capability rather than a bad file.
        auto zstd = load_image(bytes_of(ktx2_zstd));
        CT_REQUIRE(!zstd.has_value());
        CT_REQUIRE(zstd.error().code == error_code::unsupported_format);
        CT_REQUIRE(zstd.error().detail.find("Zstandard") != std::string::npos);

        // A legitimate KTX2 whose format this project has no spelling for. Also unsupported_format,
        // and the message carries the number so it can be looked up.
        auto astc = load_image(bytes_of(ktx2_astc));
        CT_REQUIRE(!astc.has_value());
        CT_REQUIRE(astc.error().code == error_code::unsupported_format);
        CT_REQUIRE(astc.error().detail.find("157") != std::string::npos);

        // A level index that disagrees with the format's arithmetic: the file is wrong, so this is
        // decode_failed and not unsupported_format. The distinction is the point of both codes.
        auto bad = load_image(bytes_of(ktx2_bad_level_size));
        CT_REQUIRE(!bad.has_value());
        CT_REQUIRE(bad.error().code == error_code::decode_failed);

        // A KTX2 identifier and nothing after it.
        const std::array<std::uint8_t, 12> stub{0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                                0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};
        auto truncated = load_image(std::as_bytes(std::span<const std::uint8_t>{stub}));
        CT_REQUIRE(!truncated.has_value());
        CT_REQUIRE(truncated.error().code == error_code::decode_failed);
    }

    // -----------------------------------------------------------------------------
    // DDS
    // -----------------------------------------------------------------------------

    void test_dds_reads_masked_bgra()
    {
        auto img = load_image(bytes_of(dds_bgra8_2x2));
        CT_REQUIRE(img.has_value());

        // Identified by its channel masks, which is the oldest of the three ways DDS names a
        // format and the only one of them that is a guess-free mapping.
        CT_REQUIRE(img->pixel_format() == format::bgra8_unorm);
        CT_REQUIRE(img->extent() == (extent3d{2, 2, 1}));
        CT_REQUIRE(img->mip_levels() == 2);
        CT_REQUIRE(img->consistent());

        const std::span<const std::byte> level0 = img->level(0);
        CT_REQUIRE(level0.size() == 2 * 2 * 4);

        // Memory order for BGRA: blue, green, red, alpha.
        CT_REQUIRE(static_cast<std::uint8_t>(level0[0]) == 0);   // red texel: B
        CT_REQUIRE(static_cast<std::uint8_t>(level0[2]) == 255); //            R
        CT_REQUIRE(static_cast<std::uint8_t>(level0[5]) == 255); // green texel: G
        CT_REQUIRE(static_cast<std::uint8_t>(level0[15]) == 128); // white texel's half alpha

        const std::span<const std::byte> level1 = img->level(1);
        CT_REQUIRE(level1.size() == 4);
        CT_REQUIRE(static_cast<std::uint8_t>(level1[0]) == 64);
        CT_REQUIRE(static_cast<std::uint8_t>(level1[3]) == 67);
    }

    void test_dds_reads_legacy_fourcc()
    {
        auto img = load_image(bytes_of(dds_dxt1_4x4));
        CT_REQUIRE(img.has_value());

        CT_REQUIRE(img->pixel_format() == format::bc1_rgba_unorm);
        CT_REQUIRE(img->extent() == (extent3d{4, 4, 1}));
        CT_REQUIRE(img->mip_levels() == 1);
        CT_REQUIRE(img->pixels().size() == 8); // one BC1 block
        CT_REQUIRE(all_equal(img->level(0), marker(0, 0)));
    }

    void test_dds_reads_dx10_bc7_chain()
    {
        auto img = load_image(bytes_of(dds_bc7_8x8));
        CT_REQUIRE(img.has_value());

        CT_REQUIRE(img->pixel_format() == format::bc7_unorm);
        CT_REQUIRE(img->mip_levels() == 4);
        CT_REQUIRE(img->array_layers() == 1);
        CT_REQUIRE(img->consistent());

        // Every level below the first is a single block whatever its extent claims.
        CT_REQUIRE(img->level(0).size() == 64);
        CT_REQUIRE(img->level(1).size() == 16);
        CT_REQUIRE(img->level(2).size() == 16);
        CT_REQUIRE(img->level(3).size() == 16);

        // DDS is already layer-major, so unlike KTX2 nothing is transposed -- and the levels still
        // have to land in order.
        for (std::uint32_t level = 0; level < 4; ++level)
            CT_REQUIRE(all_equal(img->level(level), marker(level, 0)));
    }

    void test_dds_cube_faces_become_layers()
    {
        auto full = load_image(bytes_of(dds_dxt1_cube));
        CT_REQUIRE(full.has_value());
        CT_REQUIRE(full->array_layers() == 6);
        CT_REQUIRE(full->consistent());
        for (std::uint32_t face = 0; face < 6; ++face)
            CT_REQUIRE(all_equal(full->level(0, face), marker(0, face)));

        // A partial cube map is legal DDS and was how reflection probes shipped. The reader counts
        // the face bits rather than assuming six, so the four-face file is four layers and not a
        // size mismatch.
        auto partial = load_image(bytes_of(dds_dxt1_cube_partial));
        CT_REQUIRE(partial.has_value());
        CT_REQUIRE(partial->array_layers() == 4);
        CT_REQUIRE(partial->consistent());
        CT_REQUIRE(all_equal(partial->level(0, 3), marker(0, 3)));
    }

    void test_dds_rejects_what_it_cannot_read()
    {
        // X8R8G8B8: refused rather than mapped onto bgra8_unorm. The fourth byte is undefined, and
        // passing it through as alpha would make the image arbitrarily transparent with nothing in
        // the log to explain it.
        auto x8 = load_image(bytes_of(dds_x8r8g8b8));
        CT_REQUIRE(!x8.has_value());
        CT_REQUIRE(x8.error().code == error_code::unsupported_format);
        CT_REQUIRE(x8.error().detail.find("alpha") != std::string::npos);

        auto rgb24 = load_image(bytes_of(dds_rgb24));
        CT_REQUIRE(!rgb24.has_value());
        CT_REQUIRE(rgb24.error().code == error_code::unsupported_format);

        // The file is one byte short of its own description. That is a bad file, not a missing
        // feature.
        auto truncated = load_image(bytes_of(dds_bc7_truncated));
        CT_REQUIRE(!truncated.has_value());
        CT_REQUIRE(truncated.error().code == error_code::decode_failed);

        // More levels than the extent can produce. Caught on the description rather than on the
        // size, because a chain padded past 1x1 can be made to add up.
        auto deep = load_image(bytes_of(dds_too_many_mips));
        CT_REQUIRE(!deep.has_value());
        CT_REQUIRE(deep.error().code == error_code::decode_failed);

        auto bad_size = load_image(bytes_of(dds_bad_header_size));
        CT_REQUIRE(!bad_size.has_value());
        CT_REQUIRE(bad_size.error().code == error_code::decode_failed);
    }

    void test_counts_that_overflow_are_refused()
    {
        // Both files declare a layer count that wraps when multiplied by six in the file's own
        // 32-bit width -- 0x80000000 * 6 is zero mod 2^32. A reader doing that arithmetic in 32
        // bits would size the image at nothing and hand back an `image` claiming no layers at all,
        // which is worse than a rejection because every later size check would agree with it.
        auto dds = load_image(bytes_of(dds_absurd_array_size));
        CT_REQUIRE(!dds.has_value());
        CT_REQUIRE(dds.error().code == error_code::decode_failed);

        auto ktx2 = load_image(bytes_of(ktx2_absurd_layers));
        CT_REQUIRE(!ktx2.has_value());
        CT_REQUIRE(ktx2.error().code == error_code::decode_failed);
    }

    // -----------------------------------------------------------------------------
    // Dispatch
    // -----------------------------------------------------------------------------

    void test_dispatch_is_by_magic_number()
    {
        // Neither reader is reached by name or extension: the vfs deals in URIs a pack source may
        // have stripped of any suffix, and the magic number is the part of an asset's identity a
        // content author cannot change by accident.
        CT_REQUIRE(load_image(bytes_of(ktx2_rgba8_4x4)).has_value());
        CT_REQUIRE(load_image(bytes_of(dds_dxt1_4x4)).has_value());

        // Empty input is a decode failure with a reason, not a crash and not unsupported_format.
        auto empty = load_image(std::span<const std::byte>{});
        CT_REQUIRE(!empty.has_value());
        CT_REQUIRE(empty.error().code == error_code::decode_failed);

        // Bytes nothing claims. In a build with stb this is stb answering; in one without, it is
        // the stub. Either way the code is the same, which is what makes it safe to branch on.
        const std::array<std::uint8_t, 8> noise{1, 2, 3, 4, 5, 6, 7, 8};
        auto unknown = load_image(std::as_bytes(std::span<const std::uint8_t>{noise}));
        CT_REQUIRE(!unknown.has_value());
        CT_REQUIRE(unknown.error().code == error_code::unsupported_format);
    }

    void test_srgb_flag_promotes_but_never_demotes()
    {
        // A container declares its own format, and the cooker's sRGB decision is deliberate. So the
        // flag promotes a linear container...
        auto promoted = load_image(bytes_of(dds_bc7_8x8), {.srgb = true});
        CT_REQUIRE(promoted.has_value());
        CT_REQUIRE(promoted->pixel_format() == format::bc7_unorm_srgb);
        CT_REQUIRE(is_srgb_format(promoted->pixel_format()));

        // ...and leaving it off does not demote one that already says sRGB. (BC1 here is linear in
        // the file, so this also checks that the default really is "whatever the file said".)
        auto untouched = load_image(bytes_of(dds_bc7_8x8));
        CT_REQUIRE(untouched.has_value());
        CT_REQUIRE(untouched->pixel_format() == format::bc7_unorm);

        // Promotion changes the spelling and not a byte of the payload.
        CT_REQUIRE(promoted->pixels().size() == untouched->pixels().size());
        CT_REQUIRE(all_equal(promoted->level(0), marker(0, 0)));

        auto ktx_promoted = load_image(bytes_of(ktx2_rgba8_4x4), {.srgb = true});
        CT_REQUIRE(ktx_promoted.has_value());
        CT_REQUIRE(ktx_promoted->pixel_format() == format::rgba8_unorm_srgb);
    }

    void test_block_compressed_containers_refuse_mip_generation()
    {
        // Asking for a chain on a block-compressed decode fails the whole call rather than quietly
        // handing back one level: a caller that wanted a chain and got one level has a texture that
        // shimmers and nothing to explain why.
        auto result = load_image(bytes_of(dds_dxt1_4x4), {.generate_mips = true});
        CT_REQUIRE(!result.has_value());
        CT_REQUIRE(result.error().code == error_code::unsupported_format);

        // A container that already carries a chain is never regenerated, so the flag is a no-op
        // there and the BC7 file loads as it stands.
        auto chained = load_image(bytes_of(dds_bc7_8x8), {.generate_mips = true});
        CT_REQUIRE(chained.has_value());
        CT_REQUIRE(chained->mip_levels() == 4);
    }

} // namespace

int main()
{
    test_block_format_arithmetic();

    test_ktx2_reads_a_chain();
    test_ktx2_transposes_layers();
    test_ktx2_faces_become_layers();
    test_ktx2_level_count_zero_is_one_stored_level();
    test_ktx2_rejects_what_it_cannot_read();

    test_dds_reads_masked_bgra();
    test_dds_reads_legacy_fourcc();
    test_dds_reads_dx10_bc7_chain();
    test_dds_cube_faces_become_layers();
    test_dds_rejects_what_it_cannot_read();
    test_counts_that_overflow_are_refused();

    test_dispatch_is_by_magic_number();
    test_srgb_flag_promotes_but_never_demotes();
    test_block_compressed_containers_refuse_mip_generation();

    return 0;
}
