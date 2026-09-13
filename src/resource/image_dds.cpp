/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file image_dds.cpp
 * @brief @ref catalyst::resource::detail::decode_dds -- the DDS container reader.
 * @details The other cooked container, and the one most existing tooling writes. Like KTX2 it holds
 * a mip chain, array slices and a block-compressed format, and like KTX2 there is no codec here --
 * only a header to validate and a region to hand over.
 *
 * Unlike KTX2, no transposition is needed: DDS stores each array slice or cube face as a whole mip
 * chain, one after another, which is already @ref catalyst::resource::image's layout. So this
 * reader's real work is the format, and DDS makes that harder than KTX2 does. It identifies formats
 * three different ways depending on when the file was written -- a FourCC, a set of channel bit
 * masks, or a `DXGI_FORMAT` in an extension header -- and only the third is unambiguous. The other
 * two are legacy and are supported because the files exist, not because they are good.
 *
 * @note What this reader will not do is guess. A `DDPF_RGB` file with no alpha mask (`X8R8G8B8` and
 * its relatives) is rejected rather than mapped onto `bgra8_unorm`: the fourth byte of such a file
 * is undefined, `rendering::format` has no X-channel spelling to carry that meaning, and a reader
 * that quietly handed those bytes over as alpha would produce an arbitrarily transparent texture
 * with nothing in the log. Re-cooking the file is the fix, and the failure message says so.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "detail/image_codec.hpp"

#include <cstring>
#include <string>
#include <vector>

namespace catalyst::resource::detail
{
    namespace
    {
        constexpr std::uint32_t dds_header_bytes = 124;
        constexpr std::uint32_t dds_pixelformat_bytes = 32;
        constexpr std::uint32_t dds_dxt10_header_bytes = 20;

        // DDS_PIXELFORMAT::dwFlags
        constexpr std::uint32_t ddpf_alphapixels = 0x0000'0001;
        constexpr std::uint32_t ddpf_alpha = 0x0000'0002;
        constexpr std::uint32_t ddpf_fourcc = 0x0000'0004;
        constexpr std::uint32_t ddpf_rgb = 0x0000'0040;
        constexpr std::uint32_t ddpf_luminance = 0x0002'0000;

        // DDS_HEADER::dwCaps2
        constexpr std::uint32_t ddscaps2_cubemap = 0x0000'0200;
        constexpr std::uint32_t ddscaps2_cubemap_faces = 0x0000'FC00; // the six face bits
        constexpr std::uint32_t ddscaps2_volume = 0x0020'0000;

        // DDS_HEADER_DXT10::miscFlag
        constexpr std::uint32_t dds_misc_texturecube = 0x0000'0004;

        [[nodiscard]] constexpr std::uint32_t fourcc(char a, char b, char c, char d) noexcept
        {
            return static_cast<std::uint32_t>(static_cast<unsigned char>(a)) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(b)) << 8) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(c)) << 16) |
                   (static_cast<std::uint32_t>(static_cast<unsigned char>(d)) << 24);
        }

        /** @brief The DDS pixel-format block, read verbatim. */
        struct pixel_format
        {
            std::uint32_t size = 0;
            std::uint32_t flags = 0;
            std::uint32_t four_cc = 0;
            std::uint32_t rgb_bit_count = 0;
            std::uint32_t r_mask = 0;
            std::uint32_t g_mask = 0;
            std::uint32_t b_mask = 0;
            std::uint32_t a_mask = 0;
        };

        [[nodiscard]] error bad_dds(std::string detail)
        {
            return make_error(error_code::decode_failed, {}, detail);
        }

        [[nodiscard]] error unsupported_dds(std::string detail)
        {
            return make_error(error_code::unsupported_format, {}, detail);
        }

        /**
         * @brief Size of one surface in @p fmt at @p extent, refusing anything larger than @p limit.
         * @details Every factor is a header field, which means every factor is attacker-controlled:
         * a DDS claiming 4294967295 in both axes would overflow a `uint64` product before anything
         * had a chance to compare it against the file's length. So each multiplication is checked
         * against the limit before it happens, and the limit is the bytes the file actually holds --
         * which bounds the whole computation to something a real file could justify.
         */
        [[nodiscard]] bool surface_fits(format fmt, extent3d extent, std::uint64_t limit, std::uint64_t &out) noexcept
        {
            const std::uint64_t block_w = format_block_width(fmt);
            const std::uint64_t block_h = format_block_height(fmt);
            const std::uint64_t block_bytes = format_block_size_bytes(fmt);

            if (block_bytes == 0 || extent.depth == 0)
                return false;

            const std::uint64_t blocks_x = (extent.width + block_w - 1) / block_w;
            const std::uint64_t blocks_y = (extent.height + block_h - 1) / block_h;

            if (blocks_y == 0 || blocks_x > limit / blocks_y)
                return false;

            std::uint64_t blocks = blocks_x * blocks_y;
            if (blocks > limit / extent.depth)
                return false;

            blocks *= extent.depth;
            if (blocks > limit / block_bytes)
                return false;

            out = blocks * block_bytes;
            return true;
        }

        /** @brief `DXGI_FORMAT` to `rendering::format`, for a file carrying the DX10 header. */
        format format_from_dxgi(std::uint32_t dxgi) noexcept
        {
            switch (dxgi)
            {
            case 2:   return format::rgba32_float;      // DXGI_FORMAT_R32G32B32A32_FLOAT
            case 6:   return format::rgb32_float;       // DXGI_FORMAT_R32G32B32_FLOAT
            case 10:  return format::rgba16_float;      // DXGI_FORMAT_R16G16B16A16_FLOAT
            case 16:  return format::rg32_float;        // DXGI_FORMAT_R32G32_FLOAT
            case 28:  return format::rgba8_unorm;       // DXGI_FORMAT_R8G8B8A8_UNORM
            case 29:  return format::rgba8_unorm_srgb;  // DXGI_FORMAT_R8G8B8A8_UNORM_SRGB
            case 34:  return format::rg16_float;        // DXGI_FORMAT_R16G16_FLOAT
            case 41:  return format::r32_float;         // DXGI_FORMAT_R32_FLOAT
            case 42:  return format::r32_uint;          // DXGI_FORMAT_R32_UINT
            case 43:  return format::r32_sint;          // DXGI_FORMAT_R32_SINT
            case 49:  return format::rg8_unorm;         // DXGI_FORMAT_R8G8_UNORM
            case 54:  return format::r16_float;         // DXGI_FORMAT_R16_FLOAT
            case 57:  return format::r16_uint;          // DXGI_FORMAT_R16_UINT
            case 61:  return format::r8_unorm;          // DXGI_FORMAT_R8_UNORM
            case 71:  return format::bc1_rgba_unorm;    // DXGI_FORMAT_BC1_UNORM
            case 72:  return format::bc1_rgba_unorm_srgb;
            case 74:  return format::bc2_unorm;         // DXGI_FORMAT_BC2_UNORM
            case 75:  return format::bc2_unorm_srgb;
            case 77:  return format::bc3_unorm;         // DXGI_FORMAT_BC3_UNORM
            case 78:  return format::bc3_unorm_srgb;
            case 80:  return format::bc4_unorm;         // DXGI_FORMAT_BC4_UNORM
            case 81:  return format::bc4_snorm;
            case 83:  return format::bc5_unorm;         // DXGI_FORMAT_BC5_UNORM
            case 84:  return format::bc5_snorm;
            case 87:  return format::bgra8_unorm;       // DXGI_FORMAT_B8G8R8A8_UNORM
            case 91:  return format::bgra8_unorm_srgb;
            case 95:  return format::bc6h_ufloat;       // DXGI_FORMAT_BC6H_UF16
            case 96:  return format::bc6h_sfloat;       // DXGI_FORMAT_BC6H_SF16
            case 98:  return format::bc7_unorm;         // DXGI_FORMAT_BC7_UNORM
            case 99:  return format::bc7_unorm_srgb;
            default:  return format::unknown;
            }
        }

        /**
         * @brief The legacy FourCC identifiers, including the small integers that are really
         * `D3DFORMAT` enumerators stuffed into the same field.
         * @details `DXT2` and `DXT4` are the premultiplied-alpha spellings of `DXT3` and `DXT5`.
         * The blocks are bit-identical; the difference is a statement about what the colour means,
         * which no format in `rendering::format` records either. Mapping them is correct at the
         * level this reader operates on, and a premultiplied asset needs the blend state to match
         * regardless -- which is the caller's business and was already.
         */
        format format_from_fourcc(std::uint32_t code) noexcept
        {
            switch (code)
            {
            case fourcc('D', 'X', 'T', '1'): return format::bc1_rgba_unorm;
            case fourcc('D', 'X', 'T', '2'):
            case fourcc('D', 'X', 'T', '3'): return format::bc2_unorm;
            case fourcc('D', 'X', 'T', '4'):
            case fourcc('D', 'X', 'T', '5'): return format::bc3_unorm;
            case fourcc('A', 'T', 'I', '1'):
            case fourcc('B', 'C', '4', 'U'): return format::bc4_unorm;
            case fourcc('B', 'C', '4', 'S'): return format::bc4_snorm;
            case fourcc('A', 'T', 'I', '2'):
            case fourcc('B', 'C', '5', 'U'): return format::bc5_unorm;
            case fourcc('B', 'C', '5', 'S'): return format::bc5_snorm;

            // D3DFORMAT enumerators, written into the FourCC field as bare integers by tools that
            // predate the DX10 header. The float formats are the only ones of these worth carrying:
            // the rest are 16-bit packed layouts this project's format enum has no spelling for.
            case 111: return format::r16_float;    // D3DFMT_R16F
            case 112: return format::rg16_float;   // D3DFMT_G16R16F
            case 113: return format::rgba16_float; // D3DFMT_A16B16G16R16F
            case 114: return format::r32_float;    // D3DFMT_R32F
            case 115: return format::rg32_float;   // D3DFMT_G32R32F
            case 116: return format::rgba32_float; // D3DFMT_A32B32G32R32F

            default: return format::unknown;
            }
        }

        /** @brief A FourCC rendered back to text for a failure message, or its number if unprintable. */
        std::string fourcc_text(std::uint32_t code)
        {
            std::string out;
            for (int shift = 0; shift < 32; shift += 8)
            {
                const auto ch = static_cast<char>((code >> shift) & 0xFF);
                if (ch < 0x20 || ch > 0x7E)
                    return std::to_string(code);
                out.push_back(ch);
            }
            return out;
        }

        /** @brief Mask-described uncompressed formats. See the file header for what is refused. */
        std::expected<format, error> format_from_masks(const pixel_format &pf)
        {
            const bool has_alpha = (pf.flags & ddpf_alphapixels) != 0 && pf.a_mask != 0;

            if ((pf.flags & ddpf_rgb) != 0)
            {
                if (pf.rgb_bit_count != 32)
                    return std::unexpected(unsupported_dds(
                        "DDS declares a " + std::to_string(pf.rgb_bit_count) +
                        "-bit packed RGB layout; rendering::format has no 16- or 24-bit spelling of it"));

                if (!has_alpha)
                    return std::unexpected(unsupported_dds(
                        "DDS is 32-bit RGB with no alpha channel (X8R8G8B8 or similar). Its fourth byte is "
                        "undefined and rendering::format has no X-channel spelling to carry that, so passing it "
                        "through as alpha would make the image arbitrarily transparent. Re-cook it as BGRA8"));

                if (pf.r_mask == 0x00FF'0000 && pf.g_mask == 0x0000'FF00 && pf.b_mask == 0x0000'00FF &&
                    pf.a_mask == 0xFF00'0000)
                    return format::bgra8_unorm;

                if (pf.r_mask == 0x0000'00FF && pf.g_mask == 0x0000'FF00 && pf.b_mask == 0x00FF'0000 &&
                    pf.a_mask == 0xFF00'0000)
                    return format::rgba8_unorm;

                return std::unexpected(unsupported_dds("DDS declares a 32-bit channel layout that is neither RGBA "
                                                       "nor BGRA"));
            }

            if ((pf.flags & ddpf_luminance) != 0)
            {
                if (pf.rgb_bit_count == 8 && pf.r_mask == 0x0000'00FF && !has_alpha)
                    return format::r8_unorm;

                if (pf.rgb_bit_count == 16 && pf.r_mask == 0x0000'00FF && pf.a_mask == 0x0000'FF00)
                    return format::rg8_unorm;

                return std::unexpected(unsupported_dds("DDS declares a luminance layout this reader has no mapping for"));
            }

            if ((pf.flags & ddpf_alpha) != 0)
                return std::unexpected(unsupported_dds(
                    "DDS is alpha-only (A8). rendering::format has no A8, and mapping it to r8_unorm would put the "
                    "data in a channel the shader does not expect it in"));

            return std::unexpected(unsupported_dds("DDS pixel format declares neither a FourCC nor a channel layout"));
        }

    } // namespace

    std::expected<image, error> decode_dds(std::span<const std::byte> bytes, const image_decode_options &options)
    {
        byte_reader reader{bytes};
        reader.skip(sizeof(dds_magic)); // Matched by the dispatcher before we were called.

        const std::uint32_t header_size = reader.u32();
        reader.skip(4); // dwFlags: advisory, and every field it gates is validated here anyway.
        const std::uint32_t height = reader.u32();
        const std::uint32_t width = reader.u32();
        reader.skip(4); // dwPitchOrLinearSize: describes the top level only, and is widely wrong.
        const std::uint32_t header_depth = reader.u32();
        const std::uint32_t mip_map_count = reader.u32();
        reader.skip(11 * 4); // dwReserved1

        pixel_format pf;
        pf.size = reader.u32();
        pf.flags = reader.u32();
        pf.four_cc = reader.u32();
        pf.rgb_bit_count = reader.u32();
        pf.r_mask = reader.u32();
        pf.g_mask = reader.u32();
        pf.b_mask = reader.u32();
        pf.a_mask = reader.u32();

        reader.skip(4); // dwCaps
        const std::uint32_t caps2 = reader.u32();
        reader.skip(3 * 4); // dwCaps3, dwCaps4, dwReserved2

        if (!reader.ok())
            return std::unexpected(bad_dds("truncated DDS header"));

        // The two size fields are the only self-description DDS has, and a file that gets them
        // wrong is not a DDS file whatever its magic number says.
        if (header_size != dds_header_bytes)
            return std::unexpected(bad_dds("DDS header claims to be " + std::to_string(header_size) + " bytes, not " +
                                           std::to_string(dds_header_bytes)));

        if (pf.size != dds_pixelformat_bytes)
            return std::unexpected(bad_dds("DDS pixel format claims to be " + std::to_string(pf.size) +
                                           " bytes, not " + std::to_string(dds_pixelformat_bytes)));

        const bool has_dxt10 = (pf.flags & ddpf_fourcc) != 0 && pf.four_cc == fourcc('D', 'X', '1', '0');

        format fmt = format::unknown;
        std::uint32_t array_size = 1;
        bool cube = (caps2 & ddscaps2_cubemap) != 0;
        bool volume = (caps2 & ddscaps2_volume) != 0;

        if (has_dxt10)
        {
            const std::uint32_t dxgi_format = reader.u32();
            const std::uint32_t resource_dimension = reader.u32();
            const std::uint32_t misc_flag = reader.u32();
            const std::uint32_t dxt10_array_size = reader.u32();
            reader.skip(4); // miscFlags2: alpha mode, which rendering::format does not record.

            if (!reader.ok())
                return std::unexpected(bad_dds("truncated DDS DX10 header"));

            static_assert(dds_dxt10_header_bytes == 20, "the DX10 header is five 32-bit fields");

            fmt = format_from_dxgi(dxgi_format);
            if (fmt == format::unknown)
                return std::unexpected(unsupported_dds("DDS DXGI_FORMAT " + std::to_string(dxgi_format) +
                                                       " has no counterpart in rendering::format"));

            // The DX10 header supersedes the caps bits: it is the field DirectXTex writes and the
            // one a modern file means.
            array_size = dxt10_array_size == 0 ? 1u : dxt10_array_size;
            cube = (misc_flag & dds_misc_texturecube) != 0;
            volume = resource_dimension == 4; // D3D10_RESOURCE_DIMENSION_TEXTURE3D
        }
        else if ((pf.flags & ddpf_fourcc) != 0)
        {
            fmt = format_from_fourcc(pf.four_cc);
            if (fmt == format::unknown)
                return std::unexpected(unsupported_dds("DDS FourCC '" + fourcc_text(pf.four_cc) +
                                                       "' has no counterpart in rendering::format"));
        }
        else
        {
            auto from_masks = format_from_masks(pf);
            if (!from_masks)
                return std::unexpected(std::move(from_masks.error()));
            fmt = *from_masks;
        }

        if (width == 0 || height == 0)
            return std::unexpected(bad_dds("DDS declares a zero extent"));

        const std::uint32_t depth = (volume && header_depth > 0) ? header_depth : 1u;
        const extent3d extent{width, height, depth};

        const std::uint32_t levels = mip_map_count == 0 ? 1u : mip_map_count;
        if (levels > max_mip_levels(extent))
            return std::unexpected(bad_dds("DDS declares " + std::to_string(levels) + " mip levels, but a " +
                                           std::to_string(width) + "x" + std::to_string(height) + "x" +
                                           std::to_string(depth) + " image has at most " +
                                           std::to_string(max_mip_levels(extent))));

        // How many whole mip chains follow. A legacy cube map stores only the faces whose caps2 bit
        // is set -- partial cube maps are legal and were used for reflection probes -- so the faces
        // are counted rather than assumed to be six.
        //
        // In 64 bits throughout: `arraySize` is a uint32 straight out of the file, and multiplying
        // it by six in its own width is an overflow a crafted header can reach.
        std::uint64_t layers = array_size;
        if (cube)
        {
            std::uint32_t faces = 0;
            if (has_dxt10)
            {
                faces = 6; // The DX10 spelling has no per-face bits; a cube is always complete.
            }
            else
            {
                for (std::uint32_t bit = 0x0000'0400; bit <= 0x0000'8000; bit <<= 1)
                    faces += (caps2 & ddscaps2_cubemap_faces & bit) != 0 ? 1u : 0u;
            }

            if (faces == 0)
                return std::unexpected(bad_dds("DDS is flagged as a cube map but stores no faces"));

            layers *= faces;
        }

        if (layers == 0)
            return std::unexpected(bad_dds("DDS declares no array slices"));

        if (volume && layers != 1)
            return std::unexpected(bad_dds("DDS is a volume texture with " + std::to_string(layers) +
                                           " array slices; DDS has no 3D arrays"));

        const std::size_t data_offset = reader.offset();
        const std::uint64_t available = bytes.size() - data_offset;

        // Accumulate one chain against what the file actually holds, rather than computing a size
        // from the header and comparing afterwards. Every level is bounded by `available` as it is
        // added, so no product here can wrap before it is checked.
        std::uint64_t chain = 0;
        for (std::uint32_t level = 0; level < levels; ++level)
        {
            std::uint64_t level_bytes = 0;
            if (!surface_fits(fmt, mip_extent(extent, level), available, level_bytes) ||
                level_bytes > available - chain)
                return std::unexpected(bad_dds("DDS declares a surface larger than the file that contains it"));

            chain += level_bytes;
        }

        if (chain == 0 || layers > available / chain)
            return std::unexpected(bad_dds("DDS holds " + std::to_string(available) +
                                           " bytes of surface data, but its description requires at least " +
                                           std::to_string(chain) + " for each of " + std::to_string(layers) +
                                           " array slices"));

        const std::uint64_t needed = chain * layers;

        // No transposition: DDS already stores each slice as a whole mip chain, which is this
        // module's layout. Trailing bytes past `needed` are ignored rather than rejected, because
        // some writers pad the file and none of them agree on how much.
        std::vector<std::byte> pixels(static_cast<std::size_t>(needed));
        std::memcpy(pixels.data(), bytes.data() + data_offset, static_cast<std::size_t>(needed));

        // Promote only. A file that already says sRGB said so on purpose; see image_decode_options.
        if (options.srgb)
            fmt = to_srgb_format(fmt);

        return image{std::move(pixels), fmt, extent, levels, static_cast<std::uint32_t>(layers)};
    }

} // namespace catalyst::resource::detail
