/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file image.cpp
 * @brief Mip and layer arithmetic for catalyst::resource::image.
 * @details Depends on nothing but the header, which is what lets it stay in the build when every
 * decoder is configured out. The arithmetic itself is `constexpr` in image.hpp -- `mip_extent`,
 * `format_image_size_bytes`, `packed_size_bytes` -- and these are the member spellings of it, so
 * there is exactly one implementation of the rule that a level is the floor of half the one above
 * and that a compressed level rounds up to whole blocks.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/resource/image.hpp>

namespace catalyst::resource
{

    extent3d image::extent_at(std::uint32_t level) const noexcept
    {
        return mip_extent(extent_, level);
    }

    std::size_t image::level_size_bytes(std::uint32_t level) const noexcept
    {
        return static_cast<std::size_t>(format_image_size_bytes(format_, mip_extent(extent_, level)));
    }

    std::span<const std::byte> image::level(std::uint32_t level, std::uint32_t layer) const noexcept
    {
        if (level >= mip_levels_ || layer >= array_layers_)
            return {};

        // Layers are whole mip chains laid end to end, so skip `layer` chains then `level` levels.
        std::uint64_t offset = packed_size_bytes(format_, extent_, mip_levels_, 1) * layer;
        offset += packed_size_bytes(format_, extent_, level, 1);

        const std::uint64_t size = format_image_size_bytes(format_, mip_extent(extent_, level));
        if (offset + size > pixels_.size())
            return {};

        return std::span<const std::byte>{pixels_}.subspan(static_cast<std::size_t>(offset),
                                                           static_cast<std::size_t>(size));
    }

    bool image::consistent() const noexcept
    {
        if (format_ == format::unknown || mip_levels_ == 0 || array_layers_ == 0)
            return false;

        if (extent_.width == 0 || extent_.height == 0 || extent_.depth == 0)
            return false;

        // More levels than the extent can produce is not merely a size mismatch: 1x1 has no level
        // below it, so the excess levels would each claim a whole block again and the total would
        // happen to match a buffer that is wrong. Reject the description, not just the size.
        if (mip_levels_ > max_mip_levels(extent_))
            return false;

        return pixels_.size() == packed_size_bytes(format_, extent_, mip_levels_, array_layers_);
    }

} // namespace catalyst::resource
