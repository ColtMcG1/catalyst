/**
 * @file parser.cpp
 * @brief Implements the URI parser and composer declared in parser.hpp and reference.hpp.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/resource/uri/parser.hpp>

#include <catalyst/resource/uri/charset.hpp>
#include <catalyst/resource/uri/error.hpp>
#include <catalyst/resource/uri/reference.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>

namespace catalyst::resource
{

    namespace detail
    {
        std::expected<void, uri_error> validate_authority(std::string_view auth, std::size_t base)
        {
            using namespace uri_chars;

            std::string_view rest = auth;
            std::size_t off = base;

            // userinfo: everything before the last '@', which is the one RFC 3986 splits on.
            if (const std::size_t at = rest.rfind('@'); at != std::string_view::npos)
            {
                const auto ok = validate_escaped(
                    rest.substr(0, at), off,
                    [](unsigned char c) { return is_unreserved(c) || is_sub_delim(c) || c == ':'; },
                    uri_error_code::invalid_userinfo);
                if (!ok)
                    return ok;
                rest.remove_prefix(at + 1);
                off += at + 1;
            }

            // host: an IP-literal in brackets, or a registered name.
            std::size_t host_len = 0;
            if (!rest.empty() && rest.front() == '[')
            {
                const std::size_t close = rest.find(']');
                if (close == std::string_view::npos)
                    return std::unexpected(uri_error{uri_error_code::invalid_host, off});
                // Inside the brackets: hex, ':' and '.' for IPv6 and IPvFuture's tail.
                for (std::size_t i = 1; i < close; ++i)
                {
                    const auto uc = static_cast<unsigned char>(rest[i]);
                    if (!is_hex(uc) && uc != ':' && uc != '.' && uc != 'v' && uc != 'V')
                        return std::unexpected(uri_error{uri_error_code::invalid_host, off + i});
                }
                if (close == 1)
                    return std::unexpected(uri_error{uri_error_code::invalid_host, off});
                host_len = close + 1;
            }
            else
            {
                const std::size_t colon = rest.find(':');
                host_len = (colon == std::string_view::npos) ? rest.size() : colon;
                const auto ok = validate_escaped(
                    rest.substr(0, host_len), off,
                    [](unsigned char c) { return is_unreserved(c) || is_sub_delim(c); },
                    uri_error_code::invalid_host);
                if (!ok)
                    return ok;
            }

            // port: digits only, and small enough to be a port.
            std::string_view tail = rest.substr(host_len);
            if (!tail.empty())
            {
                if (tail.front() != ':')
                    return std::unexpected(uri_error{uri_error_code::invalid_host, off + host_len});
                tail.remove_prefix(1);
                unsigned n = 0;
                for (std::size_t i = 0; i < tail.size(); ++i)
                {
                    const auto uc = static_cast<unsigned char>(tail[i]);
                    if (!is_digit(uc))
                        return std::unexpected(uri_error{uri_error_code::invalid_port, off + host_len + 1 + i});
                    n = n * 10 + hex_value(uc);
                    if (n > 65535)
                        return std::unexpected(uri_error{uri_error_code::invalid_port, off + host_len + 1 + i});
                }
            }
            return {};
        }

    } // namespace detail

    std::expected<uri, uri_error> uri::parse(std::string_view text)
    {
        using namespace detail;
        using namespace detail::uri_chars;

        if (text.size() > 0xFFFFFFFEu)
            return std::unexpected(uri_error{uri_error_code::too_long, 0xFFFFFFFEu});

        uri u;
        std::size_t pos = 0;

        // scheme ":" -- only when a ':' comes before any '/', '?' or '#', otherwise the ':' belongs
        // to a path segment and this is a relative reference.
        if (const std::size_t colon = text.find_first_of(":/?#");
            colon != std::string_view::npos && text[colon] == ':' && colon > 0)
        {
            const std::string_view scheme_text = text.substr(0, colon);
            if (!((scheme_text[0] >= 'a' && scheme_text[0] <= 'z') || (scheme_text[0] >= 'A' && scheme_text[0] <= 'Z')))
                return std::unexpected(uri_error{uri_error_code::invalid_scheme, 0});
            for (std::size_t i = 1; i < scheme_text.size(); ++i)
            {
                const auto uc = static_cast<unsigned char>(scheme_text[i]);
                if (!(is_unreserved(uc) && uc != '_' && uc != '~') && uc != '+')
                    return std::unexpected(uri_error{uri_error_code::invalid_scheme, i});
            }
            u.scheme_len_ = static_cast<std::uint32_t>(scheme_text.size());
            pos = colon + 1;
        }

        // "//" authority
        std::size_t authority_begin = std::string_view::npos;
        std::size_t authority_size = 0;
        if (text.size() - pos >= 2 && text[pos] == '/' && text[pos + 1] == '/')
        {
            const std::size_t begin = pos + 2;
            const std::size_t end_rel = text.substr(begin).find_first_of("/?#");
            const std::size_t end = (end_rel == std::string_view::npos) ? text.size() : begin + end_rel;
            if (const auto ok = validate_authority(text.substr(begin, end - begin), begin); !ok)
                return std::unexpected(ok.error());
            authority_begin = begin;
            authority_size = end - begin;
            pos = end;
        }

        // path: up to the first '?' or '#'.
        const std::size_t path_begin = pos;
        const std::size_t path_end_rel = text.substr(pos).find_first_of("?#");
        const std::size_t path_end = (path_end_rel == std::string_view::npos) ? text.size() : pos + path_end_rel;
        const std::string_view path_text = text.substr(path_begin, path_end - path_begin);

        if (const auto ok = validate_escaped(
                path_text, path_begin, [](unsigned char c) { return is_pchar_literal(c) || c == '/'; },
                uri_error_code::invalid_path);
            !ok)
            return std::unexpected(ok.error());

        if (authority_begin != std::string_view::npos)
        {
            // path-abempty: with an authority, the path is empty or rooted.
            if (!path_text.empty() && path_text.front() != '/')
                return std::unexpected(uri_error{uri_error_code::path_must_be_absolute, path_begin});
        }
        else if (u.scheme_len_ == 0 && !path_text.empty() && path_text.front() != '/')
        {
            // path-noscheme: a ':' in the first segment would reparse as a scheme.
            const std::size_t first_slash = path_text.find('/');
            const std::size_t first_colon = path_text.substr(0, first_slash).find(':');
            if (first_colon != std::string_view::npos)
                return std::unexpected(uri_error{uri_error_code::relative_path_with_colon, path_begin + first_colon});
        }
        pos = path_end;

        // "?" query
        std::size_t query_begin = std::string_view::npos;
        std::size_t query_size = 0;
        if (pos < text.size() && text[pos] == '?')
        {
            const std::size_t begin = pos + 1;
            const std::size_t hash = text.find('#', begin);
            const std::size_t end = (hash == std::string_view::npos) ? text.size() : hash;
            if (const auto ok = validate_escaped(
                    text.substr(begin, end - begin), begin,
                    [](unsigned char c) { return is_pchar_literal(c) || c == '/' || c == '?'; },
                    uri_error_code::invalid_query);
                !ok)
                return std::unexpected(ok.error());
            query_begin = begin;
            query_size = end - begin;
            pos = end;
        }

        // "#" fragment
        std::size_t fragment_begin = std::string_view::npos;
        std::size_t fragment_size = 0;
        if (pos < text.size() && text[pos] == '#')
        {
            const std::size_t begin = pos + 1;
            if (const auto ok = validate_escaped(
                    text.substr(begin), begin,
                    [](unsigned char c) { return is_pchar_literal(c) || c == '/' || c == '?'; },
                    uri_error_code::invalid_fragment);
                !ok)
                return std::unexpected(ok.error());
            fragment_begin = begin;
            fragment_size = text.size() - begin;
        }

        u.text_.assign(text);
        if (authority_begin != std::string_view::npos)
        {
            u.authority_off_ = static_cast<std::uint32_t>(authority_begin);
            u.authority_len_ = static_cast<std::uint32_t>(authority_size);
        }
        u.path_off_ = static_cast<std::uint32_t>(path_begin);
        u.path_len_ = static_cast<std::uint32_t>(path_text.size());
        if (query_begin != std::string_view::npos)
        {
            u.query_off_ = static_cast<std::uint32_t>(query_begin);
            u.query_len_ = static_cast<std::uint32_t>(query_size);
        }
        if (fragment_begin != std::string_view::npos)
        {
            u.fragment_off_ = static_cast<std::uint32_t>(fragment_begin);
            u.fragment_len_ = static_cast<std::uint32_t>(fragment_size);
        }
        return u;
    }

    std::expected<uri, uri_error> uri::from_parts(const parts &p)
    {
        // RFC 3986 section 5.3 recomposes components assuming they are already consistent with one
        // another. They are not always: an authority next to a rootless path concatenates into a
        // longer authority, and a path that looks like one takes the delimiter's place. Every such
        // pair would compose text that parses back as a *different* URI, so they are rejected here
        // rather than silently honoured. The round-trip test pins that no other pair can.
        if (p.scheme && p.scheme->find(':') != std::string_view::npos)
            return std::unexpected(uri_error{uri_error_code::invalid_scheme, 0});
        if (p.authority && p.authority->find_first_of("/?#") != std::string_view::npos)
            return std::unexpected(uri_error{uri_error_code::invalid_host, 0});
        if (p.authority && !p.path.empty() && p.path.front() != '/')
            return std::unexpected(uri_error{uri_error_code::path_must_be_absolute, 0});
        if (!p.authority && p.path.starts_with("//"))
            return std::unexpected(uri_error{uri_error_code::path_would_reparse, 0});
        if (!p.scheme && !p.authority && !p.path.starts_with("/"))
        {
            const std::size_t slash = p.path.find('/');
            if (p.path.substr(0, slash).find(':') != std::string_view::npos)
                return std::unexpected(uri_error{uri_error_code::relative_path_with_colon, 0});
        }

        std::string text;
        if (p.scheme)
        {
            text.append(*p.scheme);
            text.push_back(':');
        }
        if (p.authority)
        {
            text.append("//");
            text.append(*p.authority);
        }
        text.append(p.path);
        if (p.query)
        {
            text.push_back('?');
            text.append(*p.query);
        }
        if (p.fragment)
        {
            text.push_back('#');
            text.append(*p.fragment);
        }
        return parse(text);
    }

} // namespace catalyst::resource
