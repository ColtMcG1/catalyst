/**
 * @file charset.hpp
 * @brief The RFC 3986 character classes, as `constexpr` predicates over a single byte.
 * @details Every other header in the module is written against these: the parser validates with
 * them, @ref catalyst::resource::percent_encode decides what to escape with them, and normalization
 * decides what to unescape with them. They live in one place so that the grammar is stated once and
 * the three uses cannot drift apart.
 * License: MIT (see LICENSE).
 */

#pragma once

namespace catalyst::resource
{

    /// @brief Implementation details of @ref uri; not part of the public API.
    namespace detail::uri_chars
    {
        /// @brief `ALPHA / DIGIT / "-" / "." / "_" / "~"` -- never needs encoding, never means anything.
        [[nodiscard]] constexpr bool is_unreserved(unsigned char c) noexcept
        {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                   c == '-' || c == '.' || c == '_' || c == '~';
        }

        /// @brief `"!" / "$" / "&" / "'" / "(" / ")" / "*" / "+" / "," / ";" / "="`.
        [[nodiscard]] constexpr bool is_sub_delim(unsigned char c) noexcept
        {
            return c == '!' || c == '$' || c == '&' || c == '\'' || c == '(' || c == ')' ||
                   c == '*' || c == '+' || c == ',' || c == ';' || c == '=';
        }

        [[nodiscard]] constexpr bool is_digit(unsigned char c) noexcept { return c >= '0' && c <= '9'; }

        [[nodiscard]] constexpr bool is_hex(unsigned char c) noexcept
        {
            return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
        }

        /// @brief Value of a single hex digit; the caller must have checked @ref is_hex.
        [[nodiscard]] constexpr unsigned hex_value(unsigned char c) noexcept
        {
            if (c <= '9')
                return static_cast<unsigned>(c - '0');
            return static_cast<unsigned>((c | 0x20) - 'a') + 10;
        }

        [[nodiscard]] constexpr char hex_digit_upper(unsigned v) noexcept
        {
            return static_cast<char>(v < 10 ? ('0' + static_cast<int>(v)) : ('A' + static_cast<int>(v) - 10));
        }

        [[nodiscard]] constexpr char to_lower(char c) noexcept
        {
            return (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
        }

        /// @brief `pchar` minus pct-encoded: `unreserved / sub-delims / ":" / "@"`.
        [[nodiscard]] constexpr bool is_pchar_literal(unsigned char c) noexcept
        {
            return is_unreserved(c) || is_sub_delim(c) || c == ':' || c == '@';
        }

    } // namespace detail::uri_chars

} // namespace catalyst::resource
