/**
 * @file scan.hpp
 * @brief Byte scanners shared by every text format Catalyst parses: finding the next byte from a
 * caller-chosen stop set, and skipping whitespace.
 * @details Every delimited text format has the same inner loop -- run forward over ordinary bytes
 * until one of a handful of interesting ones shows up -- and differs only in which bytes are
 * interesting. JSON string bodies stop at `"`, `\` or a control byte; a CSV field stops at `,`, `"`,
 * CR or LF. Writing that loop once per format means writing the SWAR masks once per format, and
 * those are exactly the lines that are easy to get subtly wrong and hard to notice.
 *
 * So the stop set is a template parameter and the loop is written once. @ref scalar is the obvious
 * byte-at-a-time reference; @ref swar is the production fast path that tests eight bytes per
 * iteration with ordinary 64-bit arithmetic. They must agree on every input for every stop set,
 * which `tests/text/test_scan.cpp` pins differentially.
 *
 * Both are total on their input: a `from` past the end returns `from`, and neither ever reads past
 * `s.size()`, so they are safe on a bare buffer with no sentinel.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace catalyst::text::scan
{
    /**
     * @enum control_bytes
     * @brief Whether a scan also stops at an unescaped control byte (`< 0x20`).
     *
     * This is not just another entry in the stop set because it is a *range*, not a byte: as a range
     * it costs one SWAR mask, while spelling out all 32 bytes would cost 32. JSON forbids raw control
     * bytes inside strings and so passes @ref stop; CSV permits CR and LF inside a quoted field and
     * so passes @ref allowed, listing the bytes it actually cares about instead.
     */
    enum class control_bytes : bool
    {
        allowed = false, ///< Control bytes are ordinary content; only the listed bytes stop the scan.
        stop = true      ///< Any byte `< 0x20` stops the scan, in addition to the listed bytes.
    };

    /// @brief Implementation details; not part of the public API.
    namespace detail
    {
        inline constexpr std::uint64_t ones = 0x0101010101010101ULL; ///< 1 in each byte lane.
        inline constexpr std::uint64_t high = 0x8080808080808080ULL; ///< MSB set in each byte lane.

        /**
         * @fn eq_mask
         * @brief High bit set in each lane of @p w whose byte equals @p c, all other bits clear.
         *
         * XOR turns "equals c" into "is zero", and the classic zero-byte test from Bit Twiddling
         * Hacks finds those lanes. Valid for any byte value of @p c.
         */
        [[nodiscard]] constexpr std::uint64_t eq_mask(std::uint64_t w, unsigned char c) noexcept
        {
            const std::uint64_t x = w ^ (ones * static_cast<std::uint64_t>(c));
            return (x - ones) & ~x & high;
        }

        /**
         * @fn lt_mask
         * @brief High bit set in each lane of @p w whose byte is less than @p n, all other bits clear.
         *
         * The `& ~w` term is what keeps the subtraction's borrow from leaking between lanes, and it
         * also excludes every byte `>= 0x80` -- which is exactly right here, since a UTF-8 lead or
         * continuation byte is never a control byte. Valid for @p n up to 128.
         */
        [[nodiscard]] constexpr std::uint64_t lt_mask(std::uint64_t w, unsigned char n) noexcept
        {
            return (w - ones * static_cast<std::uint64_t>(n)) & ~w & high;
        }

    } // namespace detail

    /**
     * @fn scalar
     * @brief Scalar (byte-at-a-time) scan for the next stop byte.
     *
     * Returns the index of the first byte at or after @p from that is one of @p Stops -- or, when
     * @p Controls is @ref control_bytes::stop, any byte `< 0x20` -- or `s.size()` if the input ends
     * first.
     *
     * This is the reference implementation: obvious enough to read and agree is correct, and what
     * @ref swar is checked against.
     *
     * @tparam Controls Whether control bytes also stop the scan.
     * @tparam Stops The bytes that stop the scan. At least one is required.
     */
    template <control_bytes Controls, unsigned char... Stops>
    [[nodiscard]] inline std::size_t scalar(std::string_view s, std::size_t from) noexcept
    {
        static_assert(sizeof...(Stops) > 0, "a scan needs at least one stop byte");

        const std::size_t n = s.size();
        std::size_t i = from;
        while (i < n)
        {
            const unsigned char uc = static_cast<unsigned char>(s[i]);
            if (((uc == Stops) || ...))
                break;
            if constexpr (Controls == control_bytes::stop)
            {
                if (uc < 0x20)
                    break;
            }
            ++i;
        }
        return i;
    }

    /**
     * @fn swar
     * @brief SWAR (SIMD-within-a-register) scan for the next stop byte.
     *
     * The production fast path: tests 8 bytes per iteration with ordinary 64-bit arithmetic, no
     * intrinsics and no runtime dispatch, so it is the same code on every target. Returns the same
     * index as @ref scalar for the same template arguments.
     *
     * One mask per stop byte plus (optionally) one for the control range are OR'd together, leaving
     * set high bits only in stop-byte lanes; the lane nearest the first in-memory byte is the answer.
     *
     * @tparam Controls Whether control bytes also stop the scan.
     * @tparam Stops The bytes that stop the scan. At least one is required.
     */
    template <control_bytes Controls, unsigned char... Stops>
    [[nodiscard]] inline std::size_t swar(std::string_view s, std::size_t from) noexcept
    {
        static_assert(sizeof...(Stops) > 0, "a scan needs at least one stop byte");

        const std::size_t n = s.size();
        const char *const data = s.data();
        std::size_t i = from;

        // Whole 8-byte words. The `i + 8 <= n` guard means we never read past the end, so this is
        // safe on a bare buffer.
        while (i + 8 <= n)
        {
            std::uint64_t w;
            std::memcpy(&w, data + i, 8);

            std::uint64_t hits = (std::uint64_t{0} | ... | detail::eq_mask(w, Stops));
            if constexpr (Controls == control_bytes::stop)
                hits |= detail::lt_mask(w, 0x20);

            if (hits)
            {
                // Which end of the register holds the first in-memory byte depends on byte order.
                if constexpr (std::endian::native == std::endian::little)
                    return i + (static_cast<std::size_t>(std::countr_zero(hits)) >> 3);
                else
                    return i + (static_cast<std::size_t>(std::countl_zero(hits)) >> 3);
            }
            i += 8;
        }

        // Unaligned tail (fewer than 8 bytes left).
        return scalar<Controls, Stops...>(s, i);
    }

    /// @brief Test whether a byte is ASCII whitespace (space, tab, LF, CR).
    [[nodiscard]] constexpr bool is_ws(unsigned char c) noexcept
    {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    }

    /**
     * @fn skip_ws
     * @brief Skip a run of ASCII whitespace.
     *
     * Returns the index of the first byte at or after @p from that is not whitespace, or `s.size()`
     * if the rest is all whitespace.
     *
     * @note This stays a byte-at-a-time scalar loop on purpose. A SWAR variant was implemented and
     * benchmarked for the JSON parser and lost: real whitespace runs are short (0 bytes in compact
     * output, a newline plus a few indent spaces in pretty output), so the fixed per-call SWAR setup
     * cost is never amortized. Unlike string bodies, which can run for hundreds of bytes, whitespace
     * does not benefit from batching.
     */
    [[nodiscard]] inline std::size_t skip_ws(std::string_view s, std::size_t from) noexcept
    {
        const std::size_t n = s.size();
        std::size_t i = from;
        while (i < n && is_ws(static_cast<unsigned char>(s[i])))
            ++i;
        return i;
    }

} // namespace catalyst::text::scan
