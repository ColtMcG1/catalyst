/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file detail/image_codec.hpp
 * @brief Internal: the three readers behind @ref catalyst::resource::load_image, and the little
 * byte reader the two container readers share.
 * @details Not installed and not reachable from `<catalyst/resource/...>`. `load_image` in
 * image_decode.cpp sniffs the magic number and calls one of these; nothing else does.
 *
 * Splitting them into separate translation units is not tidiness. `decode_stb` is the only one that
 * includes stb_image.h and the only one a `CATALYST_RESOURCE_STB=OFF` build neutralises, and the
 * two container readers are pure rearrangement with no codec behind them -- so they stay in the
 * build regardless, and a headless tool that reads cooked KTX2 needs no third-party code at all.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/resource/image.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>

namespace catalyst::resource::detail
{

    /**
     * @class byte_reader
     * @brief A cursor over a byte span that reads little-endian integers and latches failure.
     * @details Both container formats are a fixed header followed by a table, and both are read
     * from a buffer whose length is attacker-controlled. Checking every field's length at its own
     * call site is where off-by-one bounds bugs live, so this latches instead: a read past the end
     * yields zero and clears @ref ok, and every subsequent read is a no-op. Read the whole header,
     * then ask @ref ok once.
     *
     * Little-endian explicitly, byte by byte, rather than by `memcpy`-ing the platform's own layout:
     * both KTX2 and DDS define their integers as little-endian on disk, and a file does not stop
     * being little-endian because the machine reading it is not.
     */
    class byte_reader
    {
    public:
        explicit byte_reader(std::span<const std::byte> bytes) noexcept : bytes_(bytes) {}

        /** @brief False once any read has run past the end of the span. */
        [[nodiscard]] bool ok() const noexcept { return ok_; }

        /** @brief Bytes not yet consumed, or 0 once @ref ok is false. */
        [[nodiscard]] std::size_t remaining() const noexcept { return ok_ ? bytes_.size() - offset_ : 0; }

        /** @brief How far in the cursor has reached. */
        [[nodiscard]] std::size_t offset() const noexcept { return offset_; }

        [[nodiscard]] std::uint32_t u32() noexcept
        {
            if (!take(4))
                return 0;

            const std::size_t at = offset_ - 4;
            return static_cast<std::uint32_t>(bytes_[at]) | (static_cast<std::uint32_t>(bytes_[at + 1]) << 8) |
                   (static_cast<std::uint32_t>(bytes_[at + 2]) << 16) |
                   (static_cast<std::uint32_t>(bytes_[at + 3]) << 24);
        }

        [[nodiscard]] std::uint64_t u64() noexcept
        {
            const std::uint64_t low = u32();
            const std::uint64_t high = u32();
            return low | (high << 32);
        }

        /** @brief Advances past @p count bytes, latching failure if there are not that many. */
        void skip(std::size_t count) noexcept { (void)take(count); }

    private:
        [[nodiscard]] bool take(std::size_t count) noexcept
        {
            if (!ok_ || bytes_.size() - offset_ < count)
            {
                ok_ = false;
                return false;
            }
            offset_ += count;
            return true;
        }

        std::span<const std::byte> bytes_;
        std::size_t offset_ = 0;
        bool ok_ = true;
    };

    /** @brief The 12-byte KTX2 file identifier. */
    inline constexpr std::uint8_t ktx2_identifier[12] = {0xAB, 0x4B, 0x54, 0x58, 0x20, 0x32,
                                                         0x30, 0xBB, 0x0D, 0x0A, 0x1A, 0x0A};

    /** @brief The four bytes a DDS file opens with, `"DDS "`. */
    inline constexpr std::uint8_t dds_magic[4] = {0x44, 0x44, 0x53, 0x20};

    /** @brief True when @p bytes begins with @p prefix. */
    template <std::size_t N>
    [[nodiscard]] inline bool starts_with(std::span<const std::byte> bytes, const std::uint8_t (&prefix)[N]) noexcept
    {
        if (bytes.size() < N)
            return false;

        for (std::size_t i = 0; i < N; ++i)
        {
            if (static_cast<std::uint8_t>(bytes[i]) != prefix[i])
                return false;
        }
        return true;
    }

    /**
     * @brief Decodes a source image through stb_image: one uncompressed level.
     * @details In a `CATALYST_RESOURCE_STB=OFF` build this still exists and reports
     * @ref error_code::unsupported_format, so the switch removes a decoder and not a symbol.
     */
    [[nodiscard]] std::expected<image, error> decode_stb(std::span<const std::byte> bytes,
                                                         const image_decode_options &options);

    /** @brief Reads a KTX2 container: the file's own chain, layers and faces. */
    [[nodiscard]] std::expected<image, error> decode_ktx2(std::span<const std::byte> bytes,
                                                          const image_decode_options &options);

    /** @brief Reads a DDS container: the file's own chain and array slices. */
    [[nodiscard]] std::expected<image, error> decode_dds(std::span<const std::byte> bytes,
                                                         const image_decode_options &options);

} // namespace catalyst::resource::detail
