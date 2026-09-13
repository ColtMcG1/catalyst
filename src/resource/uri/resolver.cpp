/**
 * @file resolver.cpp
 * @brief Implements reference resolution and normalization, declared in resolver.hpp and
 * reference.hpp.
 * License: MIT (see LICENSE).
 */

#include <catalyst/resource/uri/resolver.hpp>

#include <catalyst/resource/uri/charset.hpp>
#include <catalyst/resource/uri/error.hpp>
#include <catalyst/resource/uri/parser.hpp>
#include <catalyst/resource/uri/reference.hpp>

#include <cstddef>
#include <expected>
#include <string>
#include <string_view>
#include <utility>

namespace catalyst::resource
{

    namespace detail
    {
        std::string remove_dot_segments(std::string_view path)
        {
            std::string out;
            out.reserve(path.size());

            while (!path.empty())
            {
                if (path.starts_with("../"))
                {
                    path.remove_prefix(3);
                }
                else if (path.starts_with("./"))
                {
                    path.remove_prefix(2);
                }
                else if (path.starts_with("/./"))
                {
                    path.remove_prefix(2); // leaves "/..."
                }
                else if (path == "/.")
                {
                    path = "/";
                }
                else if (path.starts_with("/../"))
                {
                    path.remove_prefix(3); // leaves "/..."
                    if (const std::size_t slash = out.rfind('/'); slash != std::string::npos)
                        out.erase(slash);
                    else
                        out.clear();
                }
                else if (path == "/..")
                {
                    path = "/";
                    if (const std::size_t slash = out.rfind('/'); slash != std::string::npos)
                        out.erase(slash);
                    else
                        out.clear();
                }
                else if (path == "." || path == "..")
                {
                    path = {};
                }
                else
                {
                    // Move the next segment, including its leading '/' if it has one.
                    const std::size_t next = path.find('/', path.front() == '/' ? 1 : 0);
                    out.append(path.substr(0, next));
                    path = (next == std::string_view::npos) ? std::string_view() : path.substr(next);
                }
            }
            return out;
        }

        std::string merge_paths(const uri &base, std::string_view ref_path)
        {
            const std::string_view base_path = base.path();
            if (base.authority() && base_path.empty())
            {
                std::string merged = "/";
                merged.append(ref_path);
                return merged;
            }
            const std::size_t slash = base_path.rfind('/');
            std::string merged(slash == std::string_view::npos ? std::string_view() : base_path.substr(0, slash + 1));
            merged.append(ref_path);
            return merged;
        }

        std::string normalize_escapes(std::string_view s)
        {
            using namespace uri_chars;

            std::string out;
            out.reserve(s.size());
            for (std::size_t i = 0; i < s.size(); ++i)
            {
                if (s[i] == '%' && i + 2 < s.size() && is_hex(static_cast<unsigned char>(s[i + 1])) &&
                    is_hex(static_cast<unsigned char>(s[i + 2])))
                {
                    const unsigned v = (hex_value(static_cast<unsigned char>(s[i + 1])) << 4) |
                                       hex_value(static_cast<unsigned char>(s[i + 2]));
                    if (is_unreserved(static_cast<unsigned char>(v)))
                    {
                        out.push_back(static_cast<char>(v));
                    }
                    else
                    {
                        out.push_back('%');
                        out.push_back(hex_digit_upper(v >> 4));
                        out.push_back(hex_digit_upper(v & 0x0Fu));
                    }
                    i += 2;
                }
                else
                {
                    out.push_back(s[i]);
                }
            }
            return out;
        }

    } // namespace detail

    std::expected<uri, uri_error> uri::resolve(const uri &base, const uri &ref)
    {
        using namespace detail;

        if (!base.is_absolute())
            return std::unexpected(uri_error{uri_error_code::base_not_absolute, 0});

        parts t;
        std::string path_storage;

        if (ref.is_absolute())
        {
            t.scheme = ref.scheme();
            t.authority = ref.authority();
            path_storage = remove_dot_segments(ref.path());
            t.query = ref.query();
        }
        else
        {
            t.scheme = base.scheme();
            if (ref.authority())
            {
                t.authority = ref.authority();
                path_storage = remove_dot_segments(ref.path());
                t.query = ref.query();
            }
            else
            {
                t.authority = base.authority();
                if (ref.path().empty())
                {
                    path_storage.assign(base.path());
                    // An empty reference path keeps the base query unless the reference brought one.
                    t.query = ref.query() ? ref.query() : base.query();
                }
                else
                {
                    if (ref.path().front() == '/')
                        path_storage = remove_dot_segments(ref.path());
                    else
                        path_storage = remove_dot_segments(merge_paths(base, ref.path()));
                    t.query = ref.query();
                }
            }
        }

        t.path = path_storage;
        t.fragment = ref.fragment();
        return from_parts(t);
    }

    uri uri::normalized() const
    {
        using namespace detail;
        using namespace detail::uri_chars;

        std::string scheme_storage;
        if (const auto s = scheme())
        {
            scheme_storage.assign(*s);
            for (char &c : scheme_storage)
                c = to_lower(c);
        }

        std::string authority_storage;
        if (const auto a = authority())
        {
            // Lowercase the host only; userinfo is case-sensitive and a port is digits.
            const auto h = host();
            const std::size_t host_begin = h ? static_cast<std::size_t>(h->data() - a->data()) : 0;
            const std::size_t host_end = host_begin + (h ? h->size() : 0);

            authority_storage.assign(normalize_escapes(a->substr(0, host_begin)));
            std::string host_text = normalize_escapes(a->substr(host_begin, host_end - host_begin));
            for (char &c : host_text)
                c = to_lower(c);
            authority_storage.append(host_text);
            authority_storage.append(a->substr(host_end));
        }

        std::string path_storage = remove_dot_segments(normalize_escapes(path()));
        if (path_storage.empty() && authority())
            path_storage = "/";

        parts t;
        if (scheme())
            t.scheme = scheme_storage;
        if (authority())
            t.authority = authority_storage;
        t.path = path_storage;

        std::string query_storage;
        if (const auto q = query())
        {
            query_storage = normalize_escapes(*q);
            t.query = query_storage;
        }
        std::string fragment_storage;
        if (const auto f = fragment())
        {
            fragment_storage = normalize_escapes(*f);
            t.fragment = fragment_storage;
        }

        // Normalization only ever decodes unreserved escapes and lowercases, so the result is still
        // a valid URI; if that ever stopped being true the original is the safe answer.
        auto r = from_parts(t);
        return r ? std::move(*r) : *this;
    }

} // namespace catalyst::resource
