/**
 * @file percent.cpp
 * @brief Implements the percent-encoding helpers declared in percent.hpp.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/resource/uri/percent.hpp>

#include <catalyst/resource/uri/charset.hpp>
#include <catalyst/resource/uri/error.hpp>

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>

namespace catalyst::resource
{

    std::string percent_encode(std::string_view text, std::string_view extra_safe)
    {
        using namespace detail::uri_chars;

        std::string out;
        out.reserve(text.size());
        for (const char ch : text)
        {
            const auto uc = static_cast<unsigned char>(ch);
            if (is_unreserved(uc) || extra_safe.find(ch) != std::string_view::npos)
            {
                out.push_back(ch);
            }
            else
            {
                out.push_back('%');
                out.push_back(hex_digit_upper(uc >> 4));
                out.push_back(hex_digit_upper(uc & 0x0Fu));
            }
        }
        return out;
    }

    std::expected<std::string, uri_error> percent_decode(std::string_view text,
                                                                             bool plus_is_space)
    {
        using namespace detail::uri_chars;

        std::string out;
        out.reserve(text.size());
        for (std::size_t i = 0; i < text.size(); ++i)
        {
            const char ch = text[i];
            if (ch == '%')
            {
                if (i + 2 >= text.size() || !is_hex(static_cast<unsigned char>(text[i + 1])) ||
                    !is_hex(static_cast<unsigned char>(text[i + 2])))
                    return std::unexpected(uri_error{uri_error_code::invalid_percent_encoding, i});

                const unsigned v = (hex_value(static_cast<unsigned char>(text[i + 1])) << 4) |
                                   hex_value(static_cast<unsigned char>(text[i + 2]));
                out.push_back(static_cast<char>(v));
                i += 2;
            }
            else if (plus_is_space && ch == '+')
            {
                out.push_back(' ');
            }
            else
            {
                out.push_back(ch);
            }
        }
        return out;
    }

} // namespace catalyst::resource
