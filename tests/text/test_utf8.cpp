/**
 * @file test_utf8.cpp
 * @brief Tests for catalyst::text::utf8: encoding boundaries, surrogate arithmetic, and the
 * substitution that keeps encode() total.
 * @details The boundary cases are the whole point of a shared codec -- 0x7F/0x80, 0x7FF/0x800,
 * 0xFFFF/0x10000 are where a hand-rolled copy goes wrong -- so each is pinned against the byte
 * sequence the standard requires, not against another implementation of the same arithmetic.
 * License: MIT (see LICENSE).
 */

#include "../test_common.hpp"

#include <catalyst/text/utf8.hpp>

#include <string>

using namespace catalyst::text;

namespace
{
    /// The UTF-8 encoding of a single code point, as a fresh string.
    std::string enc(char32_t cp)
    {
        std::string out;
        utf8::encode(cp, out);
        return out;
    }

    void test_encode_boundaries()
    {
        // 1 byte: U+0000..U+007F
        CT_REQUIRE(enc(0x00) == std::string(1, '\0'));
        CT_REQUIRE(enc('A') == "A");
        CT_REQUIRE(enc(0x7F) == "\x7F");

        // 2 bytes: U+0080..U+07FF
        CT_REQUIRE(enc(0x80) == "\xC2\x80");
        CT_REQUIRE(enc(0xE9) == "\xC3\xA9"); // U+00E9 LATIN SMALL LETTER E WITH ACUTE
        CT_REQUIRE(enc(0x7FF) == "\xDF\xBF");

        // 3 bytes: U+0800..U+FFFF
        CT_REQUIRE(enc(0x800) == "\xE0\xA0\x80");
        CT_REQUIRE(enc(0x20AC) == "\xE2\x82\xAC"); // U+20AC EURO SIGN
        CT_REQUIRE(enc(0xFFFF) == "\xEF\xBF\xBF");

        // 4 bytes: U+10000..U+10FFFF
        CT_REQUIRE(enc(0x10000) == "\xF0\x90\x80\x80");
        CT_REQUIRE(enc(0x1F600) == "\xF0\x9F\x98\x80"); // emoji
        CT_REQUIRE(enc(utf8::max_code_point) == "\xF4\x8F\xBF\xBF");
    }

    /// encoded_length() must predict exactly what encode() appends, for every code point.
    void test_encoded_length_agrees()
    {
        for (char32_t cp = 0; cp <= 0x11000; ++cp)
            CT_REQUIRE(enc(cp).size() == utf8::encoded_length(cp));

        CT_REQUIRE(utf8::encoded_length(0x7F) == 1);
        CT_REQUIRE(utf8::encoded_length(0x80) == 2);
        CT_REQUIRE(utf8::encoded_length(0x7FF) == 2);
        CT_REQUIRE(utf8::encoded_length(0x800) == 3);
        CT_REQUIRE(utf8::encoded_length(0xFFFF) == 3);
        CT_REQUIRE(utf8::encoded_length(0x10000) == 4);
    }

    /// encode() is total: nothing it produces is ever ill-formed UTF-8.
    void test_invalid_substitutes()
    {
        const std::string fffd = "\xEF\xBF\xBD";

        CT_REQUIRE(enc(0xD800) == fffd); // lone high surrogate
        CT_REQUIRE(enc(0xDBFF) == fffd);
        CT_REQUIRE(enc(0xDC00) == fffd); // lone low surrogate
        CT_REQUIRE(enc(0xDFFF) == fffd);
        CT_REQUIRE(enc(0x110000) == fffd); // past the last code point
        CT_REQUIRE(enc(0xFFFFFFFF) == fffd);

        // The code points bracketing the surrogate block are ordinary and must survive.
        CT_REQUIRE(enc(0xD7FF) == "\xED\x9F\xBF");
        CT_REQUIRE(enc(0xE000) == "\xEE\x80\x80");
    }

    void test_classification()
    {
        CT_REQUIRE(utf8::is_high_surrogate(0xD800));
        CT_REQUIRE(utf8::is_high_surrogate(0xDBFF));
        CT_REQUIRE(!utf8::is_high_surrogate(0xDC00));
        CT_REQUIRE(!utf8::is_high_surrogate(0xD7FF));

        CT_REQUIRE(utf8::is_low_surrogate(0xDC00));
        CT_REQUIRE(utf8::is_low_surrogate(0xDFFF));
        CT_REQUIRE(!utf8::is_low_surrogate(0xDBFF));
        CT_REQUIRE(!utf8::is_low_surrogate(0xE000));

        CT_REQUIRE(utf8::is_surrogate(0xD800));
        CT_REQUIRE(utf8::is_surrogate(0xDFFF));
        CT_REQUIRE(!utf8::is_surrogate(0xD7FF));
        CT_REQUIRE(!utf8::is_surrogate(0xE000));

        CT_REQUIRE(utf8::is_valid('A'));
        CT_REQUIRE(utf8::is_valid(0x10FFFF));
        CT_REQUIRE(!utf8::is_valid(0x110000));
        CT_REQUIRE(!utf8::is_valid(0xD800));
    }

    /**
     * @brief combine_surrogates() must invert the UTF-16 split over the whole supplementary range.
     * @details Checked against the split computed independently here, rather than against the same
     * expression, so a sign or shift error in either direction shows up.
     */
    void test_combine_surrogates()
    {
        CT_REQUIRE(utf8::combine_surrogates(0xD800, 0xDC00) == 0x10000);
        CT_REQUIRE(utf8::combine_surrogates(0xDBFF, 0xDFFF) == 0x10FFFF);
        CT_REQUIRE(utf8::combine_surrogates(0xD83D, 0xDE00) == 0x1F600); // emoji

        for (char32_t cp = 0x10000; cp <= 0x10FFFF; cp += 97)
        {
            const char32_t v = cp - 0x10000;
            const char32_t high = 0xD800 + (v / 0x400);
            const char32_t low = 0xDC00 + (v % 0x400);

            CT_REQUIRE(utf8::is_high_surrogate(high));
            CT_REQUIRE(utf8::is_low_surrogate(low));
            CT_REQUIRE(utf8::combine_surrogates(high, low) == cp);
        }
    }

    void test_encode_run()
    {
        CT_REQUIRE(utf8::encode(U"") == "");
        CT_REQUIRE(utf8::encode(U"hello") == "hello");
        // Non-ASCII is spelled with escapes, never as literal bytes: the repo's sources are ASCII,
        // and MSVC without /utf-8 would otherwise read these files in the active code page.
        CT_REQUIRE(utf8::encode(U"caf\u00E9") == "caf\xC3\xA9");
        CT_REQUIRE(utf8::encode(U"a\U0001F600b") == "a\xF0\x9F\x98\x80" "b");

        // The appending overload keeps what is already there.
        std::string out = "prefix:";
        utf8::encode(U"\u20AC", out);
        CT_REQUIRE(out == "prefix:\xE2\x82\xAC");

        // An invalid code point mid-run is substituted without disturbing its neighbours.
        const std::u32string bad = U"a" + std::u32string(1, 0xD800) + U"b";
        CT_REQUIRE(utf8::encode(bad) == "a\xEF\xBF\xBD" "b");
    }

    /// The classification helpers are constexpr, so they must be usable at compile time.
    void test_constexpr()
    {
        static_assert(utf8::is_high_surrogate(0xD800));
        static_assert(utf8::is_low_surrogate(0xDC00));
        static_assert(!utf8::is_valid(0xD800));
        static_assert(utf8::is_valid(0x10FFFF));
        static_assert(utf8::combine_surrogates(0xD83D, 0xDE00) == 0x1F600);
        static_assert(utf8::encoded_length(0x10000) == 4);
    }

} // namespace

int main()
{
    test_encode_boundaries();
    test_encoded_length_agrees();
    test_invalid_substitutes();
    test_classification();
    test_combine_surrogates();
    test_encode_run();
    test_constexpr();

    std::cout << "catalyst.text.utf8: all tests passed\n";
    return 0;
}
