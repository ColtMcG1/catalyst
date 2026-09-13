/**
 * @file detail_convert.hpp
 * @brief The module's one conversion between `sample` and the fixed-point and wide-float layouts
 * that devices actually accept.
 * @details Every backend meets the same problem at the driver boundary: the module's buffers are
 * interleaved float32, and a device wants 16-, 24- or 32-bit integers, or doubles, in whichever
 * byte order its API happens to name. This used to be answered only inside the ASIO backend, which
 * left WASAPI with no answer at all - it refused any negotiated format that was not float32, which
 * is exactly the format an exclusive-mode driver is most likely not to offer.
 *
 * The conversion is resolved to a function pointer once, when a stream is configured, so the
 * real-time path is an indirect call rather than a chain of format comparisons per sample. The
 * device side of every conversion is packed; the float side is strided, which is what lets one set
 * of functions serve both shapes a backend needs:
 *
 * - a planar device buffer holding one channel (ASIO): `count` = frames, `stride` = channels, with
 *   the float pointer offset to that channel;
 * - an interleaved device buffer holding every channel (WASAPI): `count` = frames x channels,
 *   `stride` = 1.
 *
 * Byte order is swapped in the same pass as the conversion rather than in a second sweep over the
 * block, since the block is almost certainly no longer in cache by then.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/audio/types.hpp>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <utility>

namespace catalyst::audio::detail
{

    /**
     * @enum sample_format
     * @brief A device sample layout, without the byte order - that is carried separately, because
     * the same layout appears in both orders and every backend names the pair differently.
     */
    enum class sample_format : std::uint8_t
    {
        /** @brief Nothing this module can convert. A stream that negotiates one must fail. */
        unknown = 0,
        int16,
        /** @brief Three bytes per sample, packed - not a 32-bit container. */
        int24,
        int32,
        float32,
        float64,
    };

    /** @brief How many bytes one sample of @p format occupies in a device buffer. */
    [[nodiscard]] constexpr std::size_t bytes_per_sample(sample_format format) noexcept
    {
        switch (format)
        {
        case sample_format::int16:
            return 2;
        case sample_format::int24:
            return 3;
        case sample_format::int32:
            return 4;
        case sample_format::float32:
            return 4;
        case sample_format::float64:
            return 8;
        case sample_format::unknown:
            break;
        }
        return 0;
    }

    /** @brief True when the module's own `sample` can be handed to a device without conversion. */
    [[nodiscard]] constexpr bool is_native_float(sample_format format, std::endian order) noexcept
    {
        return format == sample_format::float32 && order == std::endian::native;
    }

    namespace convert
    {

        /** @brief Keeps a sample inside the range the fixed-point stores can represent. */
        [[nodiscard]] constexpr sample clamp_unit(sample value) noexcept
        {
            if (value < -1.0f)
                return -1.0f;
            if (value > 1.0f)
                return 1.0f;
            return value;
        }

        // ------------------------------------------------------------------------------------
        // Sample stores. Each knows its width and how to move one sample to or from float,
        // byteswapping as it goes.
        // ------------------------------------------------------------------------------------

        struct store_f32
        {
            static constexpr std::size_t width = 4;

            static void write(std::byte *destination, sample value, bool swap) noexcept
            {
                auto bits = std::bit_cast<std::uint32_t>(value);
                if (swap)
                    bits = std::byteswap(bits);
                std::memcpy(destination, &bits, sizeof(bits));
            }

            static sample read(const std::byte *source, bool swap) noexcept
            {
                std::uint32_t bits = 0;
                std::memcpy(&bits, source, sizeof(bits));
                if (swap)
                    bits = std::byteswap(bits);
                return std::bit_cast<sample>(bits);
            }
        };

        struct store_f64
        {
            static constexpr std::size_t width = 8;

            static void write(std::byte *destination, sample value, bool swap) noexcept
            {
                auto bits = std::bit_cast<std::uint64_t>(static_cast<double>(value));
                if (swap)
                    bits = std::byteswap(bits);
                std::memcpy(destination, &bits, sizeof(bits));
            }

            static sample read(const std::byte *source, bool swap) noexcept
            {
                std::uint64_t bits = 0;
                std::memcpy(&bits, source, sizeof(bits));
                if (swap)
                    bits = std::byteswap(bits);
                return static_cast<sample>(std::bit_cast<double>(bits));
            }
        };

        struct store_i16
        {
            static constexpr std::size_t width = 2;

            static void write(std::byte *destination, sample value, bool swap) noexcept
            {
                const auto scaled = static_cast<std::int16_t>(
                    std::lround(static_cast<double>(clamp_unit(value)) * 32767.0));

                auto bits = static_cast<std::uint16_t>(scaled);
                if (swap)
                    bits = std::byteswap(bits);
                std::memcpy(destination, &bits, sizeof(bits));
            }

            static sample read(const std::byte *source, bool swap) noexcept
            {
                std::uint16_t bits = 0;
                std::memcpy(&bits, source, sizeof(bits));
                if (swap)
                    bits = std::byteswap(bits);
                return static_cast<sample>(static_cast<std::int16_t>(bits)) * (1.0f / 32768.0f);
            }
        };

        struct store_i32
        {
            static constexpr std::size_t width = 4;

            static void write(std::byte *destination, sample value, bool swap) noexcept
            {
                const auto scaled = static_cast<std::int32_t>(
                    std::llround(static_cast<double>(clamp_unit(value)) * 2147483647.0));

                auto bits = static_cast<std::uint32_t>(scaled);
                if (swap)
                    bits = std::byteswap(bits);
                std::memcpy(destination, &bits, sizeof(bits));
            }

            static sample read(const std::byte *source, bool swap) noexcept
            {
                std::uint32_t bits = 0;
                std::memcpy(&bits, source, sizeof(bits));
                if (swap)
                    bits = std::byteswap(bits);
                return static_cast<sample>(static_cast<std::int32_t>(bits)) * (1.0f / 2147483648.0f);
            }
        };

        struct store_i24
        {
            static constexpr std::size_t width = 3;

            static void write(std::byte *destination, sample value, bool swap) noexcept
            {
                const auto scaled = static_cast<std::int32_t>(
                    std::llround(static_cast<double>(clamp_unit(value)) * 8388607.0));

                std::uint8_t bytes[width] = {
                    static_cast<std::uint8_t>(scaled & 0xFF),
                    static_cast<std::uint8_t>((scaled >> 8) & 0xFF),
                    static_cast<std::uint8_t>((scaled >> 16) & 0xFF),
                };

                if (swap)
                    std::swap(bytes[0], bytes[2]);

                std::memcpy(destination, bytes, sizeof(bytes));
            }

            static sample read(const std::byte *source, bool swap) noexcept
            {
                std::uint8_t bytes[width];
                std::memcpy(bytes, source, sizeof(bytes));
                if (swap)
                    std::swap(bytes[0], bytes[2]);

                auto scaled = static_cast<std::int32_t>(bytes[0]) |
                              (static_cast<std::int32_t>(bytes[1]) << 8) |
                              (static_cast<std::int32_t>(bytes[2]) << 16);

                // Sign-extend from 24 bits.
                if (scaled & 0x00800000)
                    scaled |= static_cast<std::int32_t>(0xFF000000u);

                return static_cast<sample>(scaled) * (1.0f / 8388608.0f);
            }
        };

        template <typename Store, bool Swap>
        void pack_with(std::byte *device, const sample *frames, std::size_t count, std::size_t stride) noexcept
        {
            for (std::size_t i = 0; i < count; ++i)
                Store::write(device + i * Store::width, frames[i * stride], Swap);
        }

        template <typename Store, bool Swap>
        void unpack_with(sample *frames, const std::byte *device, std::size_t count, std::size_t stride) noexcept
        {
            for (std::size_t i = 0; i < count; ++i)
                frames[i * stride] = Store::read(device + i * Store::width, Swap);
        }

    } // namespace convert

    /** @brief Writes @p count samples to a packed device buffer, reading floats every @p stride. */
    using pack_fn = void (*)(
        std::byte *device, const sample *frames, std::size_t count, std::size_t stride) noexcept;

    /** @brief Reads @p count samples from a packed device buffer, writing floats every @p stride. */
    using unpack_fn = void (*)(
        sample *frames, const std::byte *device, std::size_t count, std::size_t stride) noexcept;

    /** @brief The float-to-device conversion for @p format in @p order, or null if there is none. */
    [[nodiscard]] inline pack_fn pack_for(sample_format format, std::endian order) noexcept
    {
        const bool swap = order != std::endian::native;

        using namespace convert;
        switch (format)
        {
        case sample_format::int16:
            return swap ? &pack_with<store_i16, true> : &pack_with<store_i16, false>;
        case sample_format::int24:
            return swap ? &pack_with<store_i24, true> : &pack_with<store_i24, false>;
        case sample_format::int32:
            return swap ? &pack_with<store_i32, true> : &pack_with<store_i32, false>;
        case sample_format::float32:
            return swap ? &pack_with<store_f32, true> : &pack_with<store_f32, false>;
        case sample_format::float64:
            return swap ? &pack_with<store_f64, true> : &pack_with<store_f64, false>;
        case sample_format::unknown:
            break;
        }
        return nullptr;
    }

    /** @brief The device-to-float conversion for @p format in @p order, or null if there is none. */
    [[nodiscard]] inline unpack_fn unpack_for(sample_format format, std::endian order) noexcept
    {
        const bool swap = order != std::endian::native;

        using namespace convert;
        switch (format)
        {
        case sample_format::int16:
            return swap ? &unpack_with<store_i16, true> : &unpack_with<store_i16, false>;
        case sample_format::int24:
            return swap ? &unpack_with<store_i24, true> : &unpack_with<store_i24, false>;
        case sample_format::int32:
            return swap ? &unpack_with<store_i32, true> : &unpack_with<store_i32, false>;
        case sample_format::float32:
            return swap ? &unpack_with<store_f32, true> : &unpack_with<store_f32, false>;
        case sample_format::float64:
            return swap ? &unpack_with<store_f64, true> : &unpack_with<store_f64, false>;
        case sample_format::unknown:
            break;
        }
        return nullptr;
    }

} // namespace catalyst::audio::detail
