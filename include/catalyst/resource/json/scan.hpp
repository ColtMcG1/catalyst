/**
 * @file scan.hpp
 * @brief Low-level byte scanners shared by the JSON parser and serializer: finding the end of a string run
 * (the closing quote, an escape, or a control byte) and skipping whitespace.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace catalyst::resource::json::detail::scan
{
    /**
     * @fn scalar
     * @brief Scalar (byte-at-a-time) string-body scan.
     *
     * Returns the index of the first byte that ends a JSON string run: the closing quote (`"`), an
     * escape (`\`), or an unescaped control byte (`< 0x20`), or `s.size()` if the input ends first.
     * UTF-8 lead/continuation bytes (`>= 0x80`) never end a run.
     *
     * This is the obvious reference implementation; @ref swar must agree with it on every input. The
     * same byte set is exactly the set the serializer has to escape, so both directions share these
     * scanners.
     */
    [[nodiscard]] inline std::size_t scalar(std::string_view s, std::size_t from) noexcept
    {
        const std::size_t n = s.size();
        std::size_t i = from;
        while (i < n)
        {
            const unsigned char uc = static_cast<unsigned char>(s[i]);
            if (uc == '"' || uc == '\\' || uc < 0x20)
                break;
            ++i;
        }
        return i;
    }

    /**
     * @fn swar
     * @brief SWAR (SIMD-within-a-register) string-body scan.
     *
     * The production fast path: tests 8 bytes per iteration with ordinary 64-bit arithmetic. Returns
     * the same index as @ref scalar; a differential check in `tests/resource/test_json.cpp` pins that
     * they agree.
     */
    [[nodiscard]] inline std::size_t swar(std::string_view s, std::size_t from) noexcept
    {
        constexpr std::uint64_t ones = 0x0101010101010101ULL; // 1 per byte
        constexpr std::uint64_t high = 0x8080808080808080ULL; // MSB per byte

        const std::size_t n = s.size();
        const char *const data = s.data();
        std::size_t i = from;

        // Process whole 8-byte words. The `i + 8 <= n` guard means we never read past the end, so
        // this is safe even on a bare buffer.
        while (i + 8 <= n)
        {
            std::uint64_t w;
            std::memcpy(&w, data + i, 8);

            // For each test, a matched byte gets its high bit set and every other bit cleared
            // (classic "Bit Twiddling Hacks" masks), so the OR of all three leaves only high bits in
            // stop-byte positions.
            const std::uint64_t lt20 = (w - ones * 0x20) & ~w & high; // < 0x20
            const std::uint64_t xq = w ^ (ones * std::uint8_t('"'));
            const std::uint64_t isq = (xq - ones) & ~xq & high; // == '"'
            const std::uint64_t xb = w ^ (ones * std::uint8_t('\\'));
            const std::uint64_t isb = (xb - ones) & ~xb & high; // == '\\'

            const std::uint64_t hits = lt20 | isq | isb;
            if (hits)
            {
                // The set high bit nearest the first in-memory byte marks the first match; which end
                // that is depends on byte order.
                if constexpr (std::endian::native == std::endian::little)
                    return i + (std::size_t(std::countr_zero(hits)) >> 3);
                else
                    return i + (std::size_t(std::countl_zero(hits)) >> 3);
            }
            i += 8;
        }

        // Unaligned tail (fewer than 8 bytes left).
        return scalar(s, i);
    }

    /// @brief Test whether a byte is JSON whitespace (space, tab, LF, CR).
    [[nodiscard]] inline bool is_ws(unsigned char c) noexcept
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }

    /**
     * @fn ws_scalar
     * @brief Skip a run of JSON whitespace.
     *
     * Returns the index of the first byte that is NOT JSON whitespace, or `s.size()` if the rest is all
     * whitespace.
     *
     * @note This stays a byte-at-a-time scalar loop on purpose. A SWAR variant was implemented and
     * benchmarked and lost: real JSON whitespace runs are short (0 bytes in compact output, a newline
     * plus a few indent spaces in pretty output), so the fixed per-call SWAR setup cost is never
     * amortized. Unlike string bodies (@ref swar), which can run for hundreds of bytes, whitespace does
     * not benefit from batching.
     */
    [[nodiscard]] inline std::size_t ws_scalar(std::string_view s, std::size_t from) noexcept
    {
        const std::size_t n = s.size();
        std::size_t i = from;
        while (i < n && is_ws(static_cast<unsigned char>(s[i])))
            ++i;
        return i;
    }

} // namespace catalyst::resource::json::detail::scan
