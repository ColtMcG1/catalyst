/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file image_decode.cpp
 * @brief @ref catalyst::resource::load_image -- the container sniff that picks a reader, the
 * optional mip pass after it, and the `loader<image>` specialisation that puts both behind
 * `load<image>`.
 * @details There is one public entry point for decoding an image and it does not take a format
 * argument. A caller that had to name the container would have to sniff it first, and every caller
 * would sniff it slightly differently; worse, a cooked pipeline that swaps a PNG for a KTX2 would
 * then be a change at every call site instead of a change to a file on disk.
 *
 * The readers themselves are in image_stb.cpp, image_ktx2.cpp and image_dds.cpp. Only the first is
 * touched by `CATALYST_RESOURCE_STB`; see detail/image_codec.hpp.
 * License: MIT (see LICENSE).
 */

#include <catalyst/resource/image.hpp>

#include "detail/image_codec.hpp"

namespace catalyst::resource
{

    std::expected<image, error> load_image(std::span<const std::byte> bytes, image_decode_options options)
    {
        if (bytes.empty())
            return std::unexpected(make_error(error_code::decode_failed, {}, "no bytes to decode"));

        // Magic number, not file extension. The vfs deals in URIs that a pack source may well have
        // stripped of any suffix, and a decoder that trusted a name would be trusting the one piece
        // of an asset's identity that content authors change most freely.
        auto decoded = detail::starts_with(bytes, detail::ktx2_identifier)
                           ? detail::decode_ktx2(bytes, options)
                       : detail::starts_with(bytes, detail::dds_magic)
                           ? detail::decode_dds(bytes, options)
                           : detail::decode_stb(bytes, options);

        if (!decoded)
            return decoded;

        // A container that brought its own chain keeps it. This flag asks for the levels a file did
        // not have, and a cooker's chain is better than anything a box filter here will produce.
        if (options.generate_mips && decoded->mip_levels() == 1)
            return generate_mips(*decoded);

        return decoded;
    }

    std::expected<image, error> load_image(const blob &data, image_decode_options options)
    {
        return load_image(data.bytes(), options);
    }

    std::expected<image, error> loader<image>::decode(std::span<const std::byte> bytes, const load_context &,
                                                      const options &opt)
    {
        // The context goes unused: an image has no dependencies to resolve. A material's loader is
        // where that parameter starts earning its place.
        return load_image(bytes, opt);
    }

} // namespace catalyst::resource
