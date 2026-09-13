/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file image_ktx2.cpp
 * @brief @ref catalyst::resource::detail::decode_ktx2 -- the KTX2 container reader.
 * @details KTX2 is the cooked side of this module's image story: the format an offline tool writes
 * once, carrying the block-compressed encoding, the mip chain and the array layers that a runtime
 * decoder cannot produce and should not try to. There is no codec here. The file already holds the
 * texels a backend will sample; this reads the level index, checks that the file's own arithmetic
 * agrees with `rendering::format`'s, and rearranges.
 *
 * **The rearrangement is the substance.** KTX2 is level-major -- every layer and face of level 0,
 * then every layer and face of level 1 -- and @ref catalyst::resource::image is layer-major, whole
 * mip chain per layer, because that is the order `rendering::transfer_batch::upload` wants. So the
 * copy transposes, and the offsets it transposes between are the reason the level index exists.
 *
 * **The VkFormat table below is not a duplicate of `vulkan_convert.hpp`.** That one translates this
 * project's enum to a backend's; this one reads a number out of a file. They coincide because KTX2
 * chose to identify formats by Vulkan's enumerators, which is a fact about the file format and
 * stays true in a build with no Vulkan in it -- `catalyst_resource` links no renderer and includes
 * no Vulkan header, and this table is why it does not need to.
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
        /** @brief Bytes of the KTX2 header proper, after the 12-byte identifier. */
        constexpr std::size_t ktx2_header_bytes = 9 * 4;

        /** @brief Bytes of the index that follows it: four `uint32`, then two `uint64`. */
        constexpr std::size_t ktx2_index_bytes = 4 * 4 + 2 * 8;

        /** @brief Bytes per level-index entry: offset, length, uncompressed length. */
        constexpr std::size_t ktx2_level_entry_bytes = 3 * 8;

        /**
         * @brief `VkFormat` enumerator to `rendering::format`, or `unknown` for one this project has
         * no spelling of.
         * @details The numbers are Vulkan's, written out rather than included, for the reason in the
         * file header. Only the formats `rendering::format` actually has appear -- a KTX2 holding
         * ASTC or ETC2 is a legitimate file this build cannot represent, and saying
         * `unsupported_format` with the number in it is more useful than a partial mapping onto
         * something the backend would then sample wrongly.
         *
         * `BC1_RGB_*` folds onto the RGBA spelling: identical blocks, and the difference is only
         * the alpha a sampler is promised. See `rendering::format`.
         */
        format format_from_vk(std::uint32_t vk_format) noexcept
        {
            switch (vk_format)
            {
            case 9:   return format::r8_unorm;                  // VK_FORMAT_R8_UNORM
            case 16:  return format::rg8_unorm;                 // VK_FORMAT_R8G8_UNORM
            case 37:  return format::rgba8_unorm;               // VK_FORMAT_R8G8B8A8_UNORM
            case 43:  return format::rgba8_unorm_srgb;          // VK_FORMAT_R8G8B8A8_SRGB
            case 44:  return format::bgra8_unorm;               // VK_FORMAT_B8G8R8A8_UNORM
            case 50:  return format::bgra8_unorm_srgb;          // VK_FORMAT_B8G8R8A8_SRGB

            case 74:  return format::r16_uint;                  // VK_FORMAT_R16_UINT
            case 76:  return format::r16_float;                 // VK_FORMAT_R16_SFLOAT
            case 83:  return format::rg16_float;                // VK_FORMAT_R16G16_SFLOAT
            case 97:  return format::rgba16_float;              // VK_FORMAT_R16G16B16A16_SFLOAT

            case 98:  return format::r32_uint;                  // VK_FORMAT_R32_UINT
            case 99:  return format::r32_sint;                  // VK_FORMAT_R32_SINT
            case 100: return format::r32_float;                 // VK_FORMAT_R32_SFLOAT
            case 103: return format::rg32_float;                // VK_FORMAT_R32G32_SFLOAT
            case 106: return format::rgb32_float;               // VK_FORMAT_R32G32B32_SFLOAT
            case 109: return format::rgba32_float;              // VK_FORMAT_R32G32B32A32_SFLOAT

            case 124: return format::d16_unorm;                 // VK_FORMAT_D16_UNORM
            case 126: return format::d32_float;                 // VK_FORMAT_D32_SFLOAT
            case 129: return format::d24_unorm_s8_uint;         // VK_FORMAT_D24_UNORM_S8_UINT
            case 130: return format::d32_float_s8_uint;         // VK_FORMAT_D32_SFLOAT_S8_UINT

            case 131: return format::bc1_rgba_unorm;            // VK_FORMAT_BC1_RGB_UNORM_BLOCK
            case 132: return format::bc1_rgba_unorm_srgb;       // VK_FORMAT_BC1_RGB_SRGB_BLOCK
            case 133: return format::bc1_rgba_unorm;            // VK_FORMAT_BC1_RGBA_UNORM_BLOCK
            case 134: return format::bc1_rgba_unorm_srgb;       // VK_FORMAT_BC1_RGBA_SRGB_BLOCK
            case 135: return format::bc2_unorm;                 // VK_FORMAT_BC2_UNORM_BLOCK
            case 136: return format::bc2_unorm_srgb;            // VK_FORMAT_BC2_SRGB_BLOCK
            case 137: return format::bc3_unorm;                 // VK_FORMAT_BC3_UNORM_BLOCK
            case 138: return format::bc3_unorm_srgb;            // VK_FORMAT_BC3_SRGB_BLOCK
            case 139: return format::bc4_unorm;                 // VK_FORMAT_BC4_UNORM_BLOCK
            case 140: return format::bc4_snorm;                 // VK_FORMAT_BC4_SNORM_BLOCK
            case 141: return format::bc5_unorm;                 // VK_FORMAT_BC5_UNORM_BLOCK
            case 142: return format::bc5_snorm;                 // VK_FORMAT_BC5_SNORM_BLOCK
            case 143: return format::bc6h_ufloat;               // VK_FORMAT_BC6H_UFLOAT_BLOCK
            case 144: return format::bc6h_sfloat;               // VK_FORMAT_BC6H_SFLOAT_BLOCK
            case 145: return format::bc7_unorm;                 // VK_FORMAT_BC7_UNORM_BLOCK
            case 146: return format::bc7_unorm_srgb;            // VK_FORMAT_BC7_SRGB_BLOCK

            default:  return format::unknown;
            }
        }

        /** @brief The name of a supercompression scheme, for the failure message. */
        const char *supercompression_name(std::uint32_t scheme) noexcept
        {
            switch (scheme)
            {
            case 1:  return "BasisLZ";
            case 2:  return "Zstandard";
            case 3:  return "ZLIB";
            default: return "an unrecognised scheme";
            }
        }

        struct level_entry
        {
            std::uint64_t offset = 0;
            std::uint64_t length = 0;
            std::uint64_t uncompressed_length = 0;
        };

        [[nodiscard]] error bad_ktx2(std::string detail)
        {
            return make_error(error_code::decode_failed, {}, detail);
        }

    } // namespace

    std::expected<image, error> decode_ktx2(std::span<const std::byte> bytes, const image_decode_options &options)
    {
        byte_reader reader{bytes};
        reader.skip(sizeof(ktx2_identifier)); // Matched by the dispatcher before we were called.

        const std::uint32_t vk_format = reader.u32();
        const std::uint32_t type_size = reader.u32();
        const std::uint32_t pixel_width = reader.u32();
        const std::uint32_t pixel_height = reader.u32();
        const std::uint32_t pixel_depth = reader.u32();
        const std::uint32_t layer_count = reader.u32();
        const std::uint32_t face_count = reader.u32();
        const std::uint32_t level_count = reader.u32();
        const std::uint32_t supercompression = reader.u32();

        // The index describes the data-format descriptor, the key/value data and the
        // supercompression global data. None of the three affects the texels of an uncompressed
        // file, so it is skipped rather than parsed -- and skipping it through the reader is what
        // makes a header that stops short of it a clean `decode_failed` rather than a short read
        // further down.
        reader.skip(ktx2_index_bytes);

        if (!reader.ok())
            return std::unexpected(bad_ktx2("truncated KTX2 header"));

        static_assert(ktx2_header_bytes == 36, "KTX2 header is nine 32-bit fields");
        (void)type_size; // Meaningful only for endianness conversion, which this reader does not do.

        // Supercompression first, because every check after this one is about texels that are not
        // there yet. Zstandard and ZLIB would each be a decompressor dependency, and BasisLZ needs a
        // transcoder that picks a target format from the device's capabilities -- which is a Tier 3
        // conversation, not a decode.
        if (supercompression != 0)
            return std::unexpected(make_error(error_code::unsupported_format, {},
                                              std::string{"KTX2 is supercompressed with "} +
                                                  supercompression_name(supercompression) +
                                                  "; this build reads uncompressed KTX2 only"));

        if (vk_format == 0)
            return std::unexpected(make_error(error_code::unsupported_format, {},
                                              "KTX2 declares VK_FORMAT_UNDEFINED, which means its texels are "
                                              "Basis Universal and need a transcoder"));

        format fmt = format_from_vk(vk_format);
        if (fmt == format::unknown)
            return std::unexpected(make_error(error_code::unsupported_format, {},
                                              "KTX2 VkFormat " + std::to_string(vk_format) +
                                                  " has no counterpart in rendering::format"));

        if (pixel_width == 0)
            return std::unexpected(bad_ktx2("KTX2 declares a zero pixel width"));

        // A zero in either of these is the file saying "this dimension does not apply" -- a 1D
        // texture, a non-volume texture -- and not a degenerate extent.
        const extent3d extent{pixel_width, pixel_height == 0 ? 1u : pixel_height, pixel_depth == 0 ? 1u : pixel_depth};

        if (face_count != 1 && face_count != 6)
            return std::unexpected(bad_ktx2("KTX2 declares " + std::to_string(face_count) +
                                            " faces; only 1 or 6 are valid"));

        // `layerCount == 0` means "not an array", `levelCount == 0` means "the file stores one level
        // and would like the application to generate the rest". Both store one of the thing.
        //
        // That second one is deliberately *not* acted on here: it is a request, and honouring it
        // silently would triple the memory of every such file whether or not the caller wanted a
        // chain. `image_decode_options::generate_mips` is where that decision belongs, and it
        // applies to this file exactly as it does to a PNG.
        const std::uint32_t layers = layer_count == 0 ? 1u : layer_count;
        const std::uint32_t levels = level_count == 0 ? 1u : level_count;

        if (levels > max_mip_levels(extent))
            return std::unexpected(bad_ktx2("KTX2 declares " + std::to_string(levels) + " mip levels, but a " +
                                            std::to_string(extent.width) + "x" + std::to_string(extent.height) + "x" +
                                            std::to_string(extent.depth) + " image has at most " +
                                            std::to_string(max_mip_levels(extent))));

        // Faces become array layers. This module's `image` has no cube concept -- a cube map is six
        // layers with an interpretation, and the interpretation belongs to the texture view the GPU
        // bridge creates in Tier 3, not to a block of host memory.
        //
        // In 64 bits, because `layerCount` is a uint32 straight out of the file and multiplying it
        // by six in its own width is an overflow a crafted header can reach. The level-length checks
        // below then bound it for real: every stored level is `one image * total_layers` bytes and
        // has to fit inside the file, so a file that survives them cannot have named more layers
        // than it has bytes.
        const std::uint64_t total_layers = static_cast<std::uint64_t>(layers) * face_count;
        if (total_layers == 0 || total_layers > bytes.size())
            return std::unexpected(bad_ktx2("KTX2 declares " + std::to_string(total_layers) +
                                            " layers, which the file is far too small to contain"));

        std::vector<level_entry> index(levels);
        for (level_entry &entry : index)
        {
            entry.offset = reader.u64();
            entry.length = reader.u64();
            entry.uncompressed_length = reader.u64();
        }

        if (!reader.ok())
            return std::unexpected(bad_ktx2("truncated KTX2 level index"));

        static_assert(ktx2_level_entry_bytes == 24, "a KTX2 level index entry is three 64-bit fields");

        // Check every level against the size `rendering::format` says it must be before copying a
        // byte. Two things fall out of doing it up front: a crafted file cannot make the copy loop
        // below read out of bounds, and the total allocation is bounded by the file's own size,
        // because each level's length is checked to fit inside the file and the total is the sum of
        // those lengths.
        for (std::uint32_t level = 0; level < levels; ++level)
        {
            const std::uint64_t image_bytes = format_image_size_bytes(fmt, mip_extent(extent, level));
            const std::uint64_t expected = image_bytes * total_layers;

            if (index[level].length != expected)
                return std::unexpected(bad_ktx2("KTX2 level " + std::to_string(level) + " is " +
                                                std::to_string(index[level].length) + " bytes, but its extent and "
                                                "format require " + std::to_string(expected)));

            if (index[level].offset > bytes.size() || bytes.size() - index[level].offset < expected)
                return std::unexpected(bad_ktx2("KTX2 level " + std::to_string(level) + " runs past the end of the file"));
        }

        // Transpose: the file is level-major, an `image` is layer-major.
        std::vector<std::byte> pixels(
            static_cast<std::size_t>(packed_size_bytes(fmt, extent, levels, static_cast<std::uint32_t>(total_layers))));

        std::size_t written = 0;
        for (std::uint64_t layer = 0; layer < total_layers; ++layer)
        {
            for (std::uint32_t level = 0; level < levels; ++level)
            {
                const std::uint64_t image_bytes = format_image_size_bytes(fmt, mip_extent(extent, level));
                const std::uint64_t source = index[level].offset + image_bytes * layer;

                std::memcpy(pixels.data() + written, bytes.data() + source, static_cast<std::size_t>(image_bytes));
                written += static_cast<std::size_t>(image_bytes);
            }
        }

        // Promote only. A file that already says sRGB said so on purpose; see image_decode_options.
        if (options.srgb)
            fmt = to_srgb_format(fmt);

        return image{std::move(pixels), fmt, extent, levels, static_cast<std::uint32_t>(total_layers)};
    }

} // namespace catalyst::resource::detail
