/**
 * @file error.hpp
 * @brief Why a piece of text was rejected as a URI reference: @ref catalyst::resource::uri_error_code
 * and the @ref catalyst::resource::uri_error value the parser hands back.
 * @details Malformed input is an ordinary outcome for text that came from a manifest or a user, so
 * the whole module reports it as a value through `std::expected` rather than throwing, the same
 * shape the JSON and CSV parsers use.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace catalyst::resource
{

    /**
     * @enum uri_error_code
     * @brief Why a piece of text was rejected as a URI reference.
     */
    enum class uri_error_code : std::uint8_t
    {
        none = 0,
        /// A scheme that does not match `ALPHA *( ALPHA / DIGIT / "+" / "-" / "." )`.
        invalid_scheme,
        /// A `%` not followed by two hexadecimal digits.
        invalid_percent_encoding,
        /// A byte in the userinfo component that must have been percent-encoded.
        invalid_userinfo,
        /// A malformed host: a bad byte in a registered name, or an unclosed `[` IP-literal.
        invalid_host,
        /// A port containing something other than digits, or one above 65535.
        invalid_port,
        /// A byte in the path that must have been percent-encoded.
        invalid_path,
        /// A relative reference whose first path segment contains `:`, which would reparse as a
        /// scheme. RFC 3986 calls this `path-noscheme`; such a reference must be written `./a:b`.
        relative_path_with_colon,
        /// A path that does not begin with `/` even though an authority is present.
        path_must_be_absolute,
        /// A path beginning with `//` where there is no authority, which would reparse as one.
        path_would_reparse,
        /// A byte in the query that must have been percent-encoded.
        invalid_query,
        /// A byte in the fragment that must have been percent-encoded.
        invalid_fragment,
        /// A reference could not be resolved because the base URI has no scheme.
        base_not_absolute,
        /// The text is longer than the 4 GiB the component offsets can address.
        too_long,
    };

    /**
     * @fn to_string(uri_error_code)
     * @brief A short, human-readable description of a @ref uri_error_code.
     * @param code The code to describe.
     * @return A static string; never empty.
     */
    [[nodiscard]] constexpr std::string_view to_string(uri_error_code code) noexcept
    {
        switch (code)
        {
        case uri_error_code::none:
            return "no error";
        case uri_error_code::invalid_scheme:
            return "invalid scheme";
        case uri_error_code::invalid_percent_encoding:
            return "invalid percent-encoding";
        case uri_error_code::invalid_userinfo:
            return "invalid character in userinfo";
        case uri_error_code::invalid_host:
            return "invalid host";
        case uri_error_code::invalid_port:
            return "invalid port";
        case uri_error_code::invalid_path:
            return "invalid character in path";
        case uri_error_code::relative_path_with_colon:
            return "first segment of a relative reference must not contain ':'";
        case uri_error_code::path_must_be_absolute:
            return "path must begin with '/' when an authority is present";
        case uri_error_code::path_would_reparse:
            return "path beginning with '//' would reparse as an authority";
        case uri_error_code::invalid_query:
            return "invalid character in query";
        case uri_error_code::invalid_fragment:
            return "invalid character in fragment";
        case uri_error_code::base_not_absolute:
            return "base URI has no scheme";
        case uri_error_code::too_long:
            return "URI too long";
        }
        return "unknown error";
    }

    /**
     * @struct uri_error
     * @brief The failure value of @ref uri::parse and friends.
     *
     * Malformed input is an ordinary outcome for text that came from a manifest or a user, so it is
     * reported as a value rather than thrown. The offset points at the byte that was rejected, which
     * is what an error message wants to underline.
     */
    struct uri_error
    {
        uri_error_code code = uri_error_code::none; ///< What went wrong.
        std::size_t offset = 0;                     ///< Byte offset into the input where it went wrong.

        /**
         * @fn message()
         * @brief Format the error for display, e.g. `"URI parse error at offset 4: invalid port"`.
         * @return A freshly allocated string.
         */
        [[nodiscard]] std::string message() const;

        [[nodiscard]] friend bool operator==(const uri_error &, const uri_error &) noexcept = default;
    };

} // namespace catalyst::resource
