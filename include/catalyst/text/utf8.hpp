/**
 * @file utf8.hpp
 * @brief Encoding Unicode code points as UTF-8, and reassembling the UTF-16 surrogate pairs that
 * platform APIs hand over.
 * @details Catalyst speaks UTF-8 in its public API and UTF-32 in its input events, and the code that
 * bridges the two had been written three times: once in the JSON parser for `\u` escapes, once in
 * the Win32 window backend for `WM_CHAR`, and once more in an example that needed to print the text
 * its own input events carried. The third copy is the telling one -- it existed because the library
 * offered an application no way at all to turn a `text_input_event` into a printable string, so
 * every consumer had to write these same four shifted-and-masked branches itself.
 *
 * The surrogate helpers are separate from @ref encode because reassembly is inherently stateful at
 * the call site: Windows delivers a pair as two messages, while JSON sees both escapes at once. Only
 * the arithmetic is common, so only the arithmetic is shared.
 *
 * Everything here is `constexpr` where it can be, header-only, and depends on nothing but the
 * standard library.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

namespace catalyst::text::utf8
{
    /** @brief The largest code point Unicode defines. */
    inline constexpr char32_t max_code_point = 0x10FFFF;

    /** @brief U+FFFD REPLACEMENT CHARACTER, substituted by @ref encode for input it cannot encode. */
    inline constexpr char32_t replacement_character = 0xFFFD;

    /** @brief True for a UTF-16 high (leading) surrogate, U+D800..U+DBFF. */
    [[nodiscard]] constexpr bool is_high_surrogate(char32_t cp) noexcept
    {
        return cp >= 0xD800 && cp <= 0xDBFF;
    }

    /** @brief True for a UTF-16 low (trailing) surrogate, U+DC00..U+DFFF. */
    [[nodiscard]] constexpr bool is_low_surrogate(char32_t cp) noexcept
    {
        return cp >= 0xDC00 && cp <= 0xDFFF;
    }

    /**
     * @brief True for either half of a surrogate pair, U+D800..U+DFFF.
     * @details These are not code points in their own right: they exist only to let UTF-16 address
     * the supplementary planes, and UTF-8 must never encode one on its own.
     */
    [[nodiscard]] constexpr bool is_surrogate(char32_t cp) noexcept
    {
        return cp >= 0xD800 && cp <= 0xDFFF;
    }

    /**
     * @brief True if @p cp is a code point that can be encoded: in range and not a surrogate half.
     */
    [[nodiscard]] constexpr bool is_valid(char32_t cp) noexcept
    {
        return cp <= max_code_point && !is_surrogate(cp);
    }

    /**
     * @fn combine_surrogates
     * @brief Combine a UTF-16 surrogate pair into the supplementary-plane code point it encodes.
     * @param high A high surrogate; @ref is_high_surrogate must hold.
     * @param low A low surrogate; @ref is_low_surrogate must hold.
     * @return The code point, always in U+10000..U+10FFFF.
     * @note The caller checks the halves. Both call sites have to test them anyway to decide what to
     * do about a mismatch -- the JSON parser rejects the document, the Win32 backend drops the unit --
     * and that decision is not something this function could make for them.
     */
    [[nodiscard]] constexpr char32_t combine_surrogates(char32_t high, char32_t low) noexcept
    {
        return 0x10000u + ((high - 0xD800u) << 10) + (low - 0xDC00u);
    }

    /**
     * @fn encoded_length
     * @brief How many bytes @ref encode will append for @p cp.
     * @return 1, 2, 3 or 4; 3 for invalid input, which encodes as @ref replacement_character.
     */
    [[nodiscard]] constexpr std::size_t encoded_length(char32_t cp) noexcept
    {
        if (cp <= 0x7F)
            return 1;
        if (cp <= 0x7FF)
            return 2;
        if (!is_valid(cp))
            return 3; // U+FFFD
        if (cp <= 0xFFFF)
            return 3;
        return 4;
    }

    /**
     * @fn encode(char32_t, std::string&)
     * @brief Append one code point to @p out as UTF-8.
     * @param cp The code point. Anything @ref is_valid rejects -- a lone surrogate, or a value above
     *        @ref max_code_point -- is encoded as @ref replacement_character instead.
     * @param out The string to append to; existing content is kept.
     * @note Substituting rather than rejecting makes this total, so @p out is always well-formed
     * UTF-8 no matter where its code points came from. That matters most for text arriving from a
     * platform IME, where a lone surrogate is a real possibility and there is nothing useful for a
     * caller to do about one.
     */
    inline void encode(char32_t cp, std::string &out)
    {
        if (cp <= 0x7F)
        {
            out.push_back(static_cast<char>(cp));
            return;
        }
        if (cp <= 0x7FF)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            return;
        }
        if (!is_valid(cp))
            cp = replacement_character;
        if (cp <= 0xFFFF)
        {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
            return;
        }
        out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
        out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
    }

    /**
     * @fn encode(std::u32string_view, std::string&)
     * @brief Append a run of UTF-32 to @p out as UTF-8.
     * @param text The code points to encode; invalid ones become @ref replacement_character.
     * @param out The string to append to; existing content is kept.
     */
    inline void encode(std::u32string_view text, std::string &out)
    {
        out.reserve(out.size() + text.size()); // exact for ASCII, a floor otherwise
        for (const char32_t cp : text)
            encode(cp, out);
    }

    /**
     * @fn encode(std::u32string_view)
     * @brief Convert a run of UTF-32 to a UTF-8 string.
     * @param text The code points to encode; invalid ones become @ref replacement_character.
     * @return The encoded text. This is the call an application makes on
     *         `catalyst::input::text_input_event::text()` to get something printable.
     */
    [[nodiscard]] inline std::string encode(std::u32string_view text)
    {
        std::string out;
        encode(text, out);
        return out;
    }

} // namespace catalyst::text::utf8
