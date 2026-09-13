/**
 * @file test_convert.cpp
 * @brief Exercises the two pure pieces the Windows backends share: the sample-conversion table in
 * detail_convert.hpp, and the GUID text round trip in win32/detail_win32.hpp.
 * @details Neither needs a device, and both are the kind of code whose bugs are inaudible until
 * they are not. A sign error or a missing clamp in a converter is a full-scale click, not a wrong
 * answer; a GUID parser that quietly rejects a well-formed CLSID makes every installed ASIO driver
 * disappear from enumeration. Both are now reached by more than one backend, so both are asserted
 * here rather than left to be noticed on hardware.
 * License: MIT (see LICENSE).
 */

#include "../test_common.hpp"

#include <audio/detail_convert.hpp>

#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

#if defined(_WIN32)
#  include <audio/win32/detail_win32.hpp>
#endif

// Spelled through an alias rather than a using-directive: `catalyst::detail` and
// `catalyst::audio::detail` are both in scope here, and an unqualified `detail` is ambiguous.
namespace audio_detail = catalyst::audio::detail;

using catalyst::audio::sample;
using audio_detail::pack_for;
using audio_detail::sample_format;
using audio_detail::unpack_for;

namespace
{

    constexpr std::endian little = std::endian::little;
    constexpr std::endian big = std::endian::big;

    [[nodiscard]] bool close_enough(sample a, sample b, sample tolerance) noexcept
    {
        return std::fabs(a - b) <= tolerance;
    }

    void test_widths()
    {
        CT_REQUIRE(audio_detail::bytes_per_sample(sample_format::int16) == 2);
        CT_REQUIRE(audio_detail::bytes_per_sample(sample_format::int24) == 3);
        CT_REQUIRE(audio_detail::bytes_per_sample(sample_format::int32) == 4);
        CT_REQUIRE(audio_detail::bytes_per_sample(sample_format::float32) == 4);
        CT_REQUIRE(audio_detail::bytes_per_sample(sample_format::float64) == 8);
        CT_REQUIRE(audio_detail::bytes_per_sample(sample_format::unknown) == 0);

        // Only a float32 device buffer in the host's own byte order can be written into directly,
        // which is the condition WASAPI's zero-copy render path turns on.
        CT_REQUIRE(audio_detail::is_native_float(sample_format::float32, std::endian::native));
        CT_REQUIRE(!audio_detail::is_native_float(sample_format::float32,
                                            std::endian::native == little ? big : little));
        CT_REQUIRE(!audio_detail::is_native_float(sample_format::int32, std::endian::native));
    }

    void test_unknown_format_has_no_conversion()
    {
        // A backend that negotiates something we cannot convert must get a null back and refuse to
        // open, rather than silently converting as some neighbouring format.
        CT_REQUIRE(pack_for(sample_format::unknown, little) == nullptr);
        CT_REQUIRE(unpack_for(sample_format::unknown, little) == nullptr);
    }

    /// Packs @p values, unpacks them again and checks nothing moved further than @p tolerance.
    void check_round_trip(sample_format format, std::endian order, sample tolerance)
    {
        static constexpr sample values[] = {
            0.0f, 1.0f, -1.0f, 0.5f, -0.5f, 0.25f, -0.25f, 0.125f, 1.0f / 3.0f, -0.999f};

        constexpr std::size_t count = std::size(values);

        const auto pack = pack_for(format, order);
        const auto unpack = unpack_for(format, order);
        CT_REQUIRE(pack != nullptr);
        CT_REQUIRE(unpack != nullptr);

        std::vector<std::byte> device(count * audio_detail::bytes_per_sample(format), std::byte{0xCD});
        std::vector<sample> restored(count, 0.0f);

        pack(device.data(), values, count, 1);
        unpack(restored.data(), device.data(), count, 1);

        for (std::size_t i = 0; i < count; ++i)
            CT_REQUIRE(close_enough(restored[i], values[i], tolerance));
    }

    void test_round_trips()
    {
        // Two quantisation steps, not one, and the reason is worth stating: writing scales by
        // 2^n - 1 so that full scale cannot overflow, while reading divides by 2^n so that the
        // range maps to [-1, 1). The asymmetry costs up to one step on top of the half-step of
        // rounding. The 32-bit and float layouts are limited by `sample`'s own precision instead.
        check_round_trip(sample_format::int16, little, 2.0f / 32768.0f);
        check_round_trip(sample_format::int16, big, 2.0f / 32768.0f);
        check_round_trip(sample_format::int24, little, 2.0f / 8388608.0f);
        check_round_trip(sample_format::int24, big, 2.0f / 8388608.0f);
        check_round_trip(sample_format::int32, little, 1.0e-6f);
        check_round_trip(sample_format::int32, big, 1.0e-6f);
        check_round_trip(sample_format::float32, little, 0.0f);
        check_round_trip(sample_format::float32, big, 0.0f);
        check_round_trip(sample_format::float64, little, 0.0f);
        check_round_trip(sample_format::float64, big, 0.0f);
    }

    /// The failure this guards against is a wrap, not a rounding error: an unclamped +1.5 through
    /// the 16-bit store lands near negative full scale, which is the loudest sound a program can
    /// accidentally make.
    void test_fixed_point_clamps_instead_of_wrapping()
    {
        static constexpr sample overs[] = {1.5f, -1.5f, 8.0f, -8.0f, 1.0000001f};
        static constexpr sample_format formats[] = {
            sample_format::int16, sample_format::int24, sample_format::int32};

        for (const sample_format format : formats)
        {
            const auto pack = pack_for(format, little);
            const auto unpack = unpack_for(format, little);

            std::vector<std::byte> device(std::size(overs) * audio_detail::bytes_per_sample(format));
            std::vector<sample> restored(std::size(overs), 0.0f);

            pack(device.data(), overs, std::size(overs), 1);
            unpack(restored.data(), device.data(), std::size(overs), 1);

            for (std::size_t i = 0; i < std::size(overs); ++i)
            {
                const sample expected_sign = overs[i] < 0.0f ? -1.0f : 1.0f;
                CT_REQUIRE(restored[i] * expected_sign > 0.99f);
                CT_REQUIRE(restored[i] * expected_sign <= 1.0f);
            }
        }
    }

    /// Float layouts deliberately do not clamp: the device format can represent the value, and
    /// deciding it should not is the application's call, not the backend's.
    void test_float_formats_pass_through_unclamped()
    {
        static constexpr sample overs[] = {1.5f, -2.5f};

        for (const sample_format format : {sample_format::float32, sample_format::float64})
        {
            std::vector<std::byte> device(std::size(overs) * audio_detail::bytes_per_sample(format));
            std::vector<sample> restored(std::size(overs), 0.0f);

            pack_for(format, little)(device.data(), overs, std::size(overs), 1);
            unpack_for(format, little)(restored.data(), device.data(), std::size(overs), 1);

            CT_REQUIRE(restored[0] == overs[0]);
            CT_REQUIRE(restored[1] == overs[1]);
        }
    }

    /// The shape ASIO needs: one packed device buffer per channel, read from and written back into
    /// an interleaved float block. Getting the stride wrong here swaps or silences a channel.
    void test_stride_addresses_one_channel_of_an_interleaved_block()
    {
        constexpr std::size_t frames = 4;
        constexpr std::size_t channels = 2;

        const sample interleaved[frames * channels] = {
            0.1f, -0.1f, 0.2f, -0.2f, 0.3f, -0.3f, 0.4f, -0.4f};

        const auto pack = pack_for(sample_format::int24, little);
        const auto unpack = unpack_for(sample_format::int24, little);
        const std::size_t width = audio_detail::bytes_per_sample(sample_format::int24);

        for (std::size_t channel = 0; channel < channels; ++channel)
        {
            std::vector<std::byte> planar(frames * width);
            pack(planar.data(), interleaved + channel, frames, channels);

            // Read back into a fresh interleaved block at the same channel offset, and check that
            // exactly that channel was touched.
            std::vector<sample> restored(frames * channels, 99.0f);
            unpack(restored.data() + channel, planar.data(), frames, channels);

            for (std::size_t frame = 0; frame < frames; ++frame)
            {
                const std::size_t mine = frame * channels + channel;
                const std::size_t other = frame * channels + (1 - channel);

                CT_REQUIRE(close_enough(restored[mine], interleaved[mine], 2.0f / 8388608.0f));
                CT_REQUIRE(restored[other] == 99.0f);
            }
        }
    }

    /// A driver that reports an MSB sample type gets its bytes the other way round, in the same
    /// pass as the conversion.
    void test_byte_order_is_honoured()
    {
        const sample value = 0.5f;

        std::byte as_little[2]{};
        std::byte as_big[2]{};

        pack_for(sample_format::int16, little)(as_little, &value, 1, 1);
        pack_for(sample_format::int16, big)(as_big, &value, 1, 1);

        // 0.5 is 0x4000 at 16 bits, so the two orders are byte-reversed images of each other.
        CT_REQUIRE(as_little[0] == std::byte{0x00});
        CT_REQUIRE(as_little[1] == std::byte{0x40});
        CT_REQUIRE(as_big[0] == std::byte{0x40});
        CT_REQUIRE(as_big[1] == std::byte{0x00});
    }

#if defined(_WIN32)

    using audio_detail::win32::guid_to_string;
    using audio_detail::win32::try_parse_guid;

    [[nodiscard]] bool same(const GUID &a, const GUID &b) noexcept
    {
        if (a.Data1 != b.Data1 || a.Data2 != b.Data2 || a.Data3 != b.Data3)
            return false;
        for (std::size_t i = 0; i < 8; ++i)
        {
            if (a.Data4[i] != b.Data4[i])
                return false;
        }
        return true;
    }

    /// The exact byte-to-text mapping, asserted against a GUID whose every field is distinct, so a
    /// swapped field or a byte-order slip cannot pass.
    void test_guid_formatting_is_exact()
    {
        const GUID guid = {
            0x01234567, 0x89AB, 0xCDEF, {0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10}};

        CT_REQUIRE(guid_to_string(guid) == "{01234567-89AB-CDEF-FEDC-BA9876543210}");
    }

    void test_guid_round_trips()
    {
        // Including IID_IUnknown, which is the one the ASIO loader actually passes to a driver.
        const GUID guids[] = {
            {0x00000000, 0x0000, 0x0000, {0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46}},
            {0x01234567, 0x89AB, 0xCDEF, {0xFE, 0xDC, 0xBA, 0x98, 0x76, 0x54, 0x32, 0x10}},
            {0xFFFFFFFF, 0xFFFF, 0xFFFF, {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}},
            {0x00000000, 0x0000, 0x0000, {0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00}},
        };

        for (const GUID &guid : guids)
        {
            const std::string text = guid_to_string(guid);
            const std::wstring wide(text.begin(), text.end());

            const auto parsed = try_parse_guid(wide);
            CT_REQUIRE(parsed.has_value());
            CT_REQUIRE(same(*parsed, guid));
        }
    }

    void test_guid_parsing_accepts_both_spellings()
    {
        // Registry values appear both braced and bare, and drivers are inconsistent about case.
        const auto braced = try_parse_guid(L"{01234567-89ab-cdef-fedc-ba9876543210}");
        const auto bare = try_parse_guid(L"01234567-89AB-CDEF-FEDC-BA9876543210");

        CT_REQUIRE(braced.has_value());
        CT_REQUIRE(bare.has_value());
        CT_REQUIRE(same(*braced, *bare));
    }

    void test_guid_parsing_rejects_malformed_text()
    {
        const wchar_t *rejected[] = {
            L"",
            L"{}",
            L"{01234567-89AB-CDEF-FEDC-BA9876543210",         // unbalanced brace
            L"01234567-89AB-CDEF-FEDC-BA9876543210}",         // unbalanced brace
            L"01234567-89AB-CDEF-FEDC-BA987654321",           // one digit short
            L"01234567-89AB-CDEF-FEDC-BA98765432100",         // one digit long
            L"0123456789AB-CDEF-FEDC-BA9876543210",           // missing a separator
            L"01234567_89AB-CDEF-FEDC-BA9876543210",          // wrong separator
            L"0123456G-89AB-CDEF-FEDC-BA9876543210",          // not hex
            L"01234567-89AB-CDEF-FEDC-BA98765432 0",          // not hex, in the last field
            L"not a guid at all, not even close to one xx",   // right length, wrong everything
        };

        for (const wchar_t *text : rejected)
            CT_REQUIRE(!try_parse_guid(text).has_value());
    }

#endif // _WIN32

} // namespace

int main()
{
    test_widths();
    test_unknown_format_has_no_conversion();
    test_round_trips();
    test_fixed_point_clamps_instead_of_wrapping();
    test_float_formats_pass_through_unclamped();
    test_stride_addresses_one_channel_of_an_interleaved_block();
    test_byte_order_is_honoured();

#if defined(_WIN32)
    test_guid_formatting_is_exact();
    test_guid_round_trips();
    test_guid_parsing_accepts_both_spellings();
    test_guid_parsing_rejects_malformed_text();
#endif

    return 0;
}
