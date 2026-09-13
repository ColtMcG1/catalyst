/**
 * @file parser.cpp
 * @brief Implements the JSON scanning and number conversion declared in parser.hpp, and the two
 * entry points that drive the grammar over a sink.
 * @details The grammar itself is @ref catalyst::resource::json::detail::basic_parser, a template on
 * the sink, so it stays in the header where both sinks can instantiate it. What is here is the part
 * that is the same whichever representation is being built: string decoding, number scanning, and
 * the conversion from a scanned number to an `int64` or a `double`.
 * License: MIT (see LICENSE).
 */

#include <catalyst/resource/json/parser.hpp>

#include <catalyst/resource/json/document.hpp>
#include <catalyst/resource/json/error.hpp>
#include <catalyst/resource/json/value.hpp>

#include <catalyst/text/utf8.hpp>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace catalyst::resource::json::detail
{

    [[noreturn]] void fail_at(std::size_t pos, parse_error_code code)
    {
        throw parse_failure(parse_error{code, pos});
    }

    unsigned parse_hex4(std::string_view s, std::size_t &pos)
    {
        if (s.size() - pos < 4)
            fail_at(pos, parse_error_code::invalid_unicode_escape);
        unsigned v = 0;
        for (int i = 0; i < 4; ++i)
        {
            const char c = s[pos++];
            v <<= 4;
            if (c >= '0' && c <= '9')
                v |= static_cast<unsigned>(c - '0');
            else if (c >= 'a' && c <= 'f')
                v |= static_cast<unsigned>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F')
                v |= static_cast<unsigned>(c - 'A' + 10);
            else
                fail_at(pos - 1, parse_error_code::invalid_unicode_escape);
        }
        return v;
    }

    void decode_string_into(std::string_view s, std::size_t &pos, std::string &out)
    {
        const std::size_t start = pos;
        std::size_t i = scan::swar(s, start);

        // Fast path: no escapes -> one bulk append.
        if (i < s.size() && s[i] == '"')
        {
            out.append(s.data() + start, i - start);
            pos = i + 1;
            return;
        }

        // Slow path: at least one escape (or an error lies at `i`).
        out.append(s.data() + start, i - start);
        pos = i;
        while (true)
        {
            if (pos >= s.size())
                fail_at(pos, parse_error_code::unterminated_string);
            const char c = s[pos++];
            if (c == '"')
                return;
            if (c == '\\')
            {
                if (pos >= s.size())
                    fail_at(pos, parse_error_code::unterminated_string);
                const char e = s[pos++];
                switch (e)
                {
                case '"':
                    out.push_back('"');
                    break;
                case '\\':
                    out.push_back('\\');
                    break;
                case '/':
                    out.push_back('/');
                    break;
                case 'b':
                    out.push_back('\b');
                    break;
                case 'f':
                    out.push_back('\f');
                    break;
                case 'n':
                    out.push_back('\n');
                    break;
                case 'r':
                    out.push_back('\r');
                    break;
                case 't':
                    out.push_back('\t');
                    break;
                case 'u':
                {
                    char32_t cp = parse_hex4(s, pos);
                    if (text::utf8::is_high_surrogate(cp))
                    { // high surrogate: a low surrogate escape must follow
                        if (pos + 1 >= s.size() || s[pos] != '\\' || s[pos + 1] != 'u')
                            fail_at(pos, parse_error_code::invalid_surrogate);
                        pos += 2; // consume "\u"
                        const char32_t lo = parse_hex4(s, pos);
                        if (!text::utf8::is_low_surrogate(lo))
                            fail_at(pos, parse_error_code::invalid_surrogate);
                        cp = text::utf8::combine_surrogates(cp, lo);
                    }
                    else if (text::utf8::is_low_surrogate(cp))
                    {
                        fail_at(pos, parse_error_code::invalid_surrogate);
                    }
                    // Every remaining code point is valid here -- a lone surrogate was just
                    // rejected -- so encode() never has to substitute.
                    text::utf8::encode(cp, out);
                    break;
                }
                default:
                    fail_at(pos - 1, parse_error_code::invalid_escape);
                }
            }
            else if (static_cast<unsigned char>(c) < 0x20)
            {
                fail_at(pos - 1, parse_error_code::control_character);
            }
            else
            {
                // Bulk-append the next run of ordinary characters in one go.
                const std::size_t run_start = pos - 1;
                const std::size_t run_end = scan::swar(s, run_start);
                out.append(s.data() + run_start, run_end - run_start);
                pos = run_end;
            }
        }
    }

    number_token scan_number(std::string_view s, std::size_t &pos)
    {
        constexpr int kMaxFastDigits = 19; // 19 nines still fit in uint64
        const std::size_t start = pos;
        const auto is_digit = [](char c) { return c >= '0' && c <= '9'; };
        const auto cur = [&] { return pos < s.size() ? s[pos] : '\0'; };
        // A missing digit is "unexpected end" if the input ran out, else "invalid number".
        const auto need_digit = [&]
        {
            if (!is_digit(cur()))
                fail_at(pos, pos >= s.size() ? parse_error_code::unexpected_end : parse_error_code::invalid_number);
        };

        number_token n;
        int ndigits = 0; // significant digits accumulated (leading zeros don't count)
        const auto accumulate = [&](char c)
        {
            if (ndigits < kMaxFastDigits)
            {
                n.mant = n.mant * 10 + static_cast<std::uint64_t>(c - '0');
                ndigits += (n.mant != 0);
            }
            else
            {
                n.fast = false; // too many digits; let from_chars round it
            }
        };

        n.neg = cur() == '-';
        if (n.neg)
            ++pos;
        if (cur() == '0')
        {
            ++pos;
        }
        else
        {
            need_digit();
            while (is_digit(cur()))
                accumulate(s[pos++]);
        }

        if (cur() == '.')
        {
            n.is_float = true;
            ++pos;
            need_digit();
            while (is_digit(cur()))
            {
                accumulate(s[pos++]);
                if (n.fast)
                    --n.exp10;
            }
        }
        if (cur() == 'e' || cur() == 'E')
        {
            n.is_float = true;
            ++pos;
            const bool eneg = cur() == '-';
            if (cur() == '+' || cur() == '-')
                ++pos;
            need_digit();
            int e = 0;
            while (is_digit(cur()))
            {
                if (e < 10000)
                    e = e * 10 + (s[pos] - '0');
                else
                    n.fast = false; // absurd exponent; from_chars reports the range error
                ++pos;
            }
            n.exp10 += eneg ? -e : e;
        }
        n.text = s.substr(start, pos - start);
        return n;
    }

    number_value convert_number(const number_token &n, std::size_t pos)
    {
        if (!n.is_float)
        {
            if (n.fast)
            {
                // mant <= 2^63-1, or exactly 2^63 when negative (INT64_MIN).
                const std::uint64_t limit = std::uint64_t(1) << 63;
                if (n.mant < limit || (n.neg && n.mant == limit))
                    return {true, static_cast<std::int64_t>(n.neg ? 0 - n.mant : n.mant), 0.0};
            }
            else
            {
                std::int64_t i = 0;
                const char *last = n.text.data() + n.text.size();
                auto [ptr, ec] = std::from_chars(n.text.data(), last, i);
                if (ec == std::errc{} && ptr == last)
                    return {true, i, 0.0};
            }
            // Overflows int64 -> fall through to double.
        }

        if (n.fast && n.mant <= (std::uint64_t(1) << 53) && n.exp10 >= -22 && n.exp10 <= 22)
        {
            static constexpr double p10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,
                                             1e8,  1e9,  1e10, 1e11, 1e12, 1e13, 1e14, 1e15,
                                             1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
            double d = static_cast<double>(n.mant);
            d = n.exp10 < 0 ? d / p10[-n.exp10] : d * p10[n.exp10];
            return {false, 0, n.neg ? -d : d};
        }

        double d = 0.0;
        const char *last = n.text.data() + n.text.size();
        auto [ptr, ec] = std::from_chars(n.text.data(), last, d);
        if (ec != std::errc{} || ptr != last)
            fail_at(pos, parse_error_code::number_out_of_range);
        return {false, 0, d};
    }

    void value_sink::array_handle::add(value &&x)
    {
        if (arr.capacity() == 0)
            arr.reserve(kInitialReserve);
        arr.push_back(std::move(x));
    }

    void value_sink::object_handle::add(value &&x)
    {
        if (obj.capacity() == 0)
            obj.reserve(kInitialReserve);
        obj.emplace_back(std::move(key), std::move(x));
    }

    std::string &value_sink::begin_key(object_handle &h)
    {
        h.key.clear();
        return h.key;
    }

} // namespace catalyst::resource::json::detail

namespace catalyst::resource::json
{

    std::expected<value, parse_error> parse(std::string_view text)
    {
        detail::value_sink sink;
        return detail::run(text, sink);
    }

    std::expected<document, parse_error> parse_document(std::string_view text)
    {
        detail::tape_sink sink(text.size());
        return detail::run(text, sink);
    }

} // namespace catalyst::resource::json