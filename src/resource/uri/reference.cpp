/**
 * @file reference.cpp
 * @brief Implements the @ref catalyst::resource::uri component accessors declared in reference.hpp.
 * @details Everything here reads the one text buffer a `uri` owns. The offsets the parser recorded
 * cover the components RFC 3986 delimits with punctuation the parser had to find anyway; the
 * sub-parts of the authority are not among them, so `userinfo`, `host` and `port` split it again
 * here rather than costing three more offsets in every `uri` that has no authority at all.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/resource/uri/reference.hpp>

#include <catalyst/resource/uri/percent.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace catalyst::resource
{

    std::optional<std::string_view> uri::authority() const noexcept
    {
        return component(authority_off_, authority_len_);
    }

    std::string_view uri::path() const noexcept
    {
        return std::string_view(text_).substr(path_off_, path_len_);
    }

    std::optional<std::string_view> uri::query() const noexcept
    {
        return component(query_off_, query_len_);
    }

    std::optional<std::string_view> uri::fragment() const noexcept
    {
        return component(fragment_off_, fragment_len_);
    }

    std::expected<std::string, uri_error> uri::decoded_path() const
    {
        return percent_decode(path());
    }

    bool uri::equivalent(const uri &a, const uri &b)
    {
        return a.normalized().text_ == b.normalized().text_;
    }

    std::optional<std::string_view> uri::scheme() const noexcept
    {
        if (scheme_len_ == 0)
            return std::nullopt;
        return std::string_view(text_).substr(0, scheme_len_);
    }

    std::optional<std::string_view> uri::userinfo() const noexcept
    {
        const auto auth = authority();
        if (!auth)
            return std::nullopt;
        const std::size_t at = auth->rfind('@');
        if (at == std::string_view::npos)
            return std::nullopt;
        return auth->substr(0, at);
    }

    std::optional<std::string_view> uri::host() const noexcept
    {
        const auto auth = authority();
        if (!auth)
            return std::nullopt;
        std::string_view rest = *auth;
        if (const std::size_t at = rest.rfind('@'); at != std::string_view::npos)
            rest.remove_prefix(at + 1);
        return rest.substr(0, host_length(rest));
    }

    std::optional<std::string_view> uri::port() const noexcept
    {
        const auto auth = authority();
        if (!auth)
            return std::nullopt;
        std::string_view rest = *auth;
        if (const std::size_t at = rest.rfind('@'); at != std::string_view::npos)
            rest.remove_prefix(at + 1);
        const std::size_t hlen = host_length(rest);
        if (hlen >= rest.size() || rest[hlen] != ':')
            return std::nullopt;
        return rest.substr(hlen + 1);
    }

    std::optional<std::uint16_t> uri::port_number() const noexcept
    {
        const auto p = port();
        if (!p || p->empty())
            return std::nullopt;
        unsigned n = 0;
        for (const char c : *p)
            n = n * 10 + static_cast<unsigned>(c - '0'); // digits and range checked at parse
        return static_cast<std::uint16_t>(n);
    }

    std::vector<uri::query_parameter> uri::query_parameters() const
    {
        std::vector<query_parameter> out;
        const auto q = query();
        if (!q || q->empty())
            return out;

        std::string_view rest = *q;
        while (!rest.empty())
        {
            const std::size_t sep = rest.find_first_of("&;");
            const std::string_view pair = rest.substr(0, sep);
            rest = (sep == std::string_view::npos) ? std::string_view() : rest.substr(sep + 1);
            if (pair.empty())
                continue;

            const std::size_t eq = pair.find('=');
            if (eq == std::string_view::npos)
                out.push_back(query_parameter{pair, std::nullopt});
            else
                out.push_back(query_parameter{pair.substr(0, eq), pair.substr(eq + 1)});
        }
        return out;
    }

    std::optional<uri::query_parameter> uri::find_query_parameter(std::string_view name) const
    {
        for (const query_parameter &p : query_parameters())
            if (p.name == name)
                return p;
        return std::nullopt;
    }

    std::optional<std::string> uri::decoded_fragment() const
    {
        const auto f = fragment();
        if (!f)
            return std::nullopt;
        auto d = percent_decode(*f);
        if (!d)
            return std::nullopt;
        return std::move(*d);
    }

    std::vector<std::string_view> uri::path_segments() const
    {
        std::vector<std::string_view> out;
        std::string_view rest = path();
        while (!rest.empty())
        {
            const std::size_t slash = rest.find('/');
            const std::string_view seg = rest.substr(0, slash);
            if (!seg.empty())
                out.push_back(seg);
            if (slash == std::string_view::npos)
                break;
            rest = rest.substr(slash + 1);
        }
        return out;
    }

    std::optional<std::string_view> uri::component(std::uint32_t off, std::uint32_t len) const noexcept
    {
        if (off == npos32)
            return std::nullopt;
        return std::string_view(text_).substr(off, len);
    }

    std::size_t uri::host_length(std::string_view host_and_port) noexcept
    {
        if (!host_and_port.empty() && host_and_port.front() == '[')
        {
            const std::size_t close = host_and_port.find(']');
            return close == std::string_view::npos ? host_and_port.size() : close + 1;
        }
        const std::size_t colon = host_and_port.find(':');
        return colon == std::string_view::npos ? host_and_port.size() : colon;
    }

} // namespace catalyst::resource
