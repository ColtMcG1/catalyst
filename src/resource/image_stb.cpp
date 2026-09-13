/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file image_stb.cpp
 * @brief @ref catalyst::resource::detail::decode_stb -- the source-format reader, on top of
 * stb_image.
 * @details The one file `CATALYST_RESOURCE_STB=OFF` neutralises, and the only one in the module
 * that includes a third-party codec. Everything else about images -- the asset type, the mip
 * arithmetic, the container readers, the loader seam -- compiles and works without it.
 *
 * The shape of every decode here is the same: ask stb what the bytes are before asking it to decode
 * them. `stbi_info_from_memory` is what lets this file tell @ref error_code::unsupported_format
 * (no codec recognised the header) from @ref error_code::decode_failed (one did, and then the
 * pixels were bad) -- the distinction load_image documents, and one a bare `stbi_load` call cannot
 * make because it answers null to both.
 * License: MIT (see LICENSE).
 */

#include "detail/image_codec.hpp"

#include <cstring>
#include <limits>
#include <memory>
#include <string_view>

#if CATALYST_RESOURCE_HAS_STB
#include <stb_image.h>
#endif

namespace catalyst::resource::detail
{
#if CATALYST_RESOURCE_HAS_STB

    namespace
    {
        /** @brief stb hands back a `malloc`'d buffer; this is the only thing that frees it. */
        struct stbi_deleter
        {
            void operator()(void *p) const noexcept { stbi_image_free(p); }
        };

        template <typename T>
        using stbi_buffer = std::unique_ptr<T, stbi_deleter>;

        /** @brief stb's reason for the most recent failure, or a stand-in when it left none. */
        std::string_view failure_reason() noexcept
        {
            const char *reason = stbi_failure_reason();
            return (reason != nullptr) ? std::string_view{reason} : std::string_view{"unknown decoder failure"};
        }

        /**
         * @brief How many channels to ask stb for, given what the file holds.
         * @details Three channels always become four: `rgb8` is in no backend's sampling
         * intersection. One and two stay narrow only when the caller asked for that and did not
         * also ask for sRGB, since `r8_unorm` and `rg8_unorm` have no sRGB counterparts in
         * @ref rendering::format and silently ignoring the flag would be the worse answer.
         */
        int desired_channels(int in_file, const image_decode_options &options) noexcept
        {
            if (options.srgb || !options.preserve_channels)
                return 4;

            return (in_file == 1 || in_file == 2) ? in_file : 4;
        }

        /** @brief The format for an 8-bit decode that produced @p channels channels. */
        format format_for(int channels, bool srgb) noexcept
        {
            switch (channels)
            {
            case 1:
                return format::r8_unorm;
            case 2:
                return format::rg8_unorm;
            case 4:
                return srgb ? format::rgba8_unorm_srgb : format::rgba8_unorm;
            default:
                return format::unknown;
            }
        }

        /**
         * @brief Byte size of a decoded level, or 0 if the multiplication would wrap.
         * @details `w`, `h` and `channels` are stb's, which means they come from the file's header
         * and are therefore attacker-controlled. stb caps dimensions at 2^24 internally, but the
         * product with a texel size is still worth computing in a width that cannot overflow before
         * it is handed to an allocator.
         */
        std::size_t decoded_size_bytes(int w, int h, std::size_t texel_size) noexcept
        {
            if (w <= 0 || h <= 0 || texel_size == 0)
                return 0;

            const auto width = static_cast<std::size_t>(w);
            const auto height = static_cast<std::size_t>(h);

            constexpr std::size_t limit = std::numeric_limits<std::size_t>::max();
            if (width > limit / height)
                return 0;

            const std::size_t texels = width * height;
            if (texels > limit / texel_size)
                return 0;

            return texels * texel_size;
        }

        /** @brief Wraps a decoded buffer in an @ref image, copying it into owned storage. */
        image adopt_decoded(const void *pixels, std::size_t size, format fmt, int w, int h)
        {
            std::vector<std::byte> storage(size);
            std::memcpy(storage.data(), pixels, size);

            const extent3d extent{static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), 1u};
            return image{std::move(storage), fmt, extent, 1u, 1u};
        }

    } // namespace

    std::expected<image, error> decode_stb(std::span<const std::byte> bytes, const image_decode_options &options)
    {
        // stb's memory entry points take the length as `int`. A blob larger than that is a real
        // possibility for a texture pack, and truncating the length would hand a decoder a
        // deliberately short buffer, so refuse rather than narrow.
        if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
            return std::unexpected(make_error(error_code::decode_failed, {},
                                              "encoded image exceeds the decoder's 2 GiB limit"));

        const auto *data = reinterpret_cast<const stbi_uc *>(bytes.data());
        const auto length = static_cast<int>(bytes.size());

        // Header first: this is the only call that separates "nothing here recognises these bytes"
        // from "a codec took them and choked".
        int width = 0;
        int height = 0;
        int channels_in_file = 0;
        if (stbi_info_from_memory(data, length, &width, &height, &channels_in_file) == 0)
            return std::unexpected(make_error(error_code::unsupported_format, {}, failure_reason()));

        // Radiance .hdr is the one container here whose texels are not 8-bit. It becomes
        // `rgba32_float` -- the only float format in `rendering::format` wide enough to hold it
        // without a second conversion -- and the sRGB flag means nothing for it.
        if (stbi_is_hdr_from_memory(data, length) != 0)
        {
            int decoded_channels = 0;
            const stbi_buffer<float> pixels{
                stbi_loadf_from_memory(data, length, &width, &height, &decoded_channels, 4)};

            if (!pixels)
                return std::unexpected(make_error(error_code::decode_failed, {}, failure_reason()));

            const std::size_t size = decoded_size_bytes(width, height, format_size_bytes(format::rgba32_float));
            if (size == 0)
                return std::unexpected(make_error(error_code::decode_failed, {}, "decoded image size overflows"));

            return adopt_decoded(pixels.get(), size, format::rgba32_float, width, height);
        }

        // 16-bit-per-channel PNGs come back through the 8-bit path, which is stb converting them
        // down for us. There is nowhere better for them to go: `rendering::format` has no 16-bit
        // unorm, and promoting to `rgba16_float` would cost twice the memory to carry precision
        // the source did not have in a float encoding.
        const int requested = desired_channels(channels_in_file, options);
        const format fmt = format_for(requested, options.srgb);
        if (fmt == format::unknown)
            return std::unexpected(make_error(error_code::decode_failed, {}, "unsupported channel count"));

        int decoded_channels = 0;
        const stbi_buffer<stbi_uc> pixels{
            stbi_load_from_memory(data, length, &width, &height, &decoded_channels, requested)};

        if (!pixels)
            return std::unexpected(make_error(error_code::decode_failed, {}, failure_reason()));

        const std::size_t size = decoded_size_bytes(width, height, format_size_bytes(fmt));
        if (size == 0)
            return std::unexpected(make_error(error_code::decode_failed, {}, "decoded image size overflows"));

        return adopt_decoded(pixels.get(), size, fmt, width, height);
    }

#else // !CATALYST_RESOURCE_HAS_STB

    std::expected<image, error> decode_stb(std::span<const std::byte>, const image_decode_options &)
    {
        // A build configured with CATALYST_RESOURCE_STB=OFF links, and says so at the call site
        // rather than at link time. KTX2 and DDS still read: they are this module's own code, and
        // the switch removes the source-format codecs and nothing else.
        return std::unexpected(make_error(error_code::unsupported_format, {},
                                          "this build has no source-format image decoders "
                                          "(CATALYST_RESOURCE_STB=OFF); KTX2 and DDS still read"));
    }

#endif // CATALYST_RESOURCE_HAS_STB

} // namespace catalyst::resource::detail
