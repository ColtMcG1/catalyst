/**
 * @file error.hpp
 * @brief Error types for the catalyst::resource::json module: the @ref parse_error value that the parsers return
 * through `std::expected`, and the @ref type_error exception thrown when a value is accessed as the wrong type.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace catalyst::resource::json
{
    /**
     * @enum parse_error_code
     * @brief Why a piece of text was rejected as JSON.
     */
    enum class parse_error_code : std::uint8_t
    {
        none = 0,
        /// The input ended in the middle of a value.
        unexpected_end,
        /// A byte that cannot begin a JSON value appeared where a value was expected.
        unexpected_character,
        /// Non-whitespace bytes followed the root value.
        trailing_characters,
        /// Something beginning with `t`, `f` or `n` that is not `true`, `false` or `null`.
        invalid_literal,
        /// A malformed number: leading zero, missing digit after `.`/`e`, lone `-`, ...
        invalid_number,
        /// A number that a `double` cannot represent (for example `1e400`).
        number_out_of_range,
        /// The input ended inside a string or inside an escape sequence.
        unterminated_string,
        /// A backslash followed by a character that is not a JSON escape.
        invalid_escape,
        /// A `\u` escape not followed by four hexadecimal digits.
        invalid_unicode_escape,
        /// A lone or mismatched UTF-16 surrogate in `\u` escapes.
        invalid_surrogate,
        /// An unescaped control byte (`< 0x20`) inside a string.
        control_character,
        /// An object member that does not begin with a string key.
        expected_key,
        /// A missing `:` after an object key.
        expected_colon,
        /// An array element not followed by `,` or `]`.
        expected_comma_or_bracket,
        /// An object member not followed by `,` or `}`.
        expected_comma_or_brace,
        /// Containers nested deeper than @ref max_depth.
        nesting_too_deep,
    };

    /**
     * @fn to_string(parse_error_code)
     * @brief A short, human-readable description of a @ref parse_error_code.
     * @param code The code to describe.
     * @return A static string; never empty.
     */
    [[nodiscard]] constexpr std::string_view to_string(parse_error_code code) noexcept
    {
        switch (code)
        {
        case parse_error_code::none:
            return "no error";
        case parse_error_code::unexpected_end:
            return "unexpected end of input";
        case parse_error_code::unexpected_character:
            return "unexpected character";
        case parse_error_code::trailing_characters:
            return "trailing characters after JSON value";
        case parse_error_code::invalid_literal:
            return "invalid literal";
        case parse_error_code::invalid_number:
            return "invalid number";
        case parse_error_code::number_out_of_range:
            return "number out of range";
        case parse_error_code::unterminated_string:
            return "unterminated string";
        case parse_error_code::invalid_escape:
            return "invalid escape character";
        case parse_error_code::invalid_unicode_escape:
            return "invalid \\u escape";
        case parse_error_code::invalid_surrogate:
            return "invalid UTF-16 surrogate pair";
        case parse_error_code::control_character:
            return "control character must be escaped in string";
        case parse_error_code::expected_key:
            return "expected string key in object";
        case parse_error_code::expected_colon:
            return "expected ':' after object key";
        case parse_error_code::expected_comma_or_bracket:
            return "expected ',' or ']' in array";
        case parse_error_code::expected_comma_or_brace:
            return "expected ',' or '}' in object";
        case parse_error_code::nesting_too_deep:
            return "nesting too deep";
        }
        return "unknown error";
    }

    /**
     * @struct parse_error
     * @brief The failure value of @ref parse and @ref parse_document.
     *
     * Malformed input is an ordinary outcome for a parser that reads files from disk or bytes off the
     * network, so it is reported as a value rather than thrown. The offset points at the byte where
     * the parser gave up, which is what an error message wants to show.
     */
    struct parse_error
    {
        parse_error_code code = parse_error_code::none; ///< What went wrong.
        std::size_t offset = 0;                         ///< Byte offset into the input where it went wrong.

        /**
         * @fn message()
         * @brief Format the error for display, e.g. `"JSON parse error at offset 12: invalid number"`.
         * @return A freshly allocated string.
         */
        [[nodiscard]] std::string message() const;

        [[nodiscard]] friend bool operator==(const parse_error &, const parse_error &) noexcept = default;
    };

    /**
     * @class type_error
     * @brief Thrown when a value is accessed as the wrong type.
     *
     * The `as_*` accessors and `at()` lookups are checked accesses in the spirit of `std::vector::at`:
     * a mismatch means the caller's assumption about the document shape was wrong. Code that is not
     * sure of the shape should use the `try_*` accessors and `find()`, which never throw.
     */
    class type_error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

} // namespace catalyst::resource::json
