/**
 * @file serializer.cpp
 * @brief Implements the scalar and string writing behind @ref catalyst::resource::json::dump,
 * declared in serializer.hpp.
 * @details The structural part of the writer -- arrays, objects, the recursion -- is a template on
 * the node type so that one writer walks both a `value` tree and a `cursor` over a tape, and it
 * stays in the header. What is here is everything that only ever sees bytes: indentation, the two
 * number formats, and the escaping a JSON string needs.
 * License: MIT (see LICENSE).
 */

#include <catalyst/resource/json/serializer.hpp>

#include <catalyst/resource/json/document.hpp>
#include <catalyst/resource/json/tape.hpp>
#include <catalyst/resource/json/value.hpp>

#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace catalyst::resource::json::detail
{

    void serializer::newline_indent(int depth)
    {
        if (!pretty_)
            return;
        out_.push_back('\n');
        out_.append(static_cast<std::size_t>(indent_) * static_cast<std::size_t>(depth), ' ');
    }

    void serializer::write_int(std::int64_t i)
    {
        char buf[24];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, i);
        out_.append(buf, ptr);
    }

    void serializer::write_double(double d)
    {
        if (!std::isfinite(d))
        {
            out_ += "null";
            return;
        }
        char buf[32];
        auto [ptr, ec] = std::to_chars(buf, buf + sizeof buf, d);
        bool floaty = false; // does it already read back as a floating value?
        for (const char *q = buf; q < ptr; ++q)
            if (*q == '.' || *q == 'e' || *q == 'E')
            {
                floaty = true;
                break;
            }
        out_.append(buf, ptr);
        if (!floaty)
            out_ += ".0";
    }

    void serializer::write_string(std::string_view s)
    {
        static constexpr char hex[] = "0123456789abcdef";
        out_.push_back('"');
        std::size_t i = 0;
        while (true)
        {
            const std::size_t stop = scan::swar(s, i);
            out_.append(s.data() + i, stop - i);
            if (stop == s.size())
                break;
            i = stop + 1;
            const char c = s[stop];
            switch (c)
            {
            case '"':
                out_ += "\\\"";
                break;
            case '\\':
                out_ += "\\\\";
                break;
            case '\b':
                out_ += "\\b";
                break;
            case '\f':
                out_ += "\\f";
                break;
            case '\n':
                out_ += "\\n";
                break;
            case '\r':
                out_ += "\\r";
                break;
            case '\t':
                out_ += "\\t";
                break;
            default:
            { // other control chars -> \u00XX
                const unsigned char uc = static_cast<unsigned char>(c);
                out_ += "\\u00";
                out_.push_back(hex[(uc >> 4) & 0xF]);
                out_.push_back(hex[uc & 0xF]);
            }
            }
        }
        out_.push_back('"');
    }

} // namespace catalyst::resource::json::detail

namespace catalyst::resource::json
{

    std::string dump(const value &v, int indent)
    {
        std::string out;
        detail::serializer(out, indent).write(v);
        return out;
    }

    std::string dump(const cursor &c, int indent)
    {
        std::string out;
        detail::serializer(out, indent).write(c);
        return out;
    }

    std::string dump(const document &doc, int indent)
    {
        if (doc.empty())
            return {};
        return dump(doc.root(), indent);
    }

} // namespace catalyst::resource::json