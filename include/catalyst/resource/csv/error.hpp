/**
 * @file error.hpp
 * @brief Error types for the catalyst::resource::csv module: the @ref parse_error value that
 * @ref parse_table returns through `std::expected`, and the @ref type_error exception thrown when a
 * field is read as a type it does not hold.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>

namespace catalyst::resource::csv
{
    /**
     * @enum parse_error_code
     * @brief Why a piece of text was rejected as CSV.
     *
     * CSV has far fewer ways to be wrong than JSON does, and all of them are about quoting or about
     * the shape of the rectangle. Anything else is content.
     */
    enum class parse_error_code : std::uint8_t
    {
        none = 0,
        /// The input ended inside a quoted field, with no closing quote.
        unterminated_quote,
        /// A quote appeared partway through an unquoted field, where it can only be a mistake.
        bare_quote,
        /// A closing quote was followed by something other than a delimiter, a newline or the end of
        /// input -- usually a single quote that should have been doubled.
        invalid_quoted_escape,
        /// A record has a different number of fields from the first one, and the dialect does not
        /// allow ragged rows.
        inconsistent_column_count,
        /// The input has more fields, or more bytes of field data, than the 32-bit offsets can address.
        too_large,
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
        case parse_error_code::unterminated_quote:
            return "unterminated quoted field";
        case parse_error_code::bare_quote:
            return "unescaped quote in an unquoted field";
        case parse_error_code::invalid_quoted_escape:
            return "unexpected character after a closing quote";
        case parse_error_code::inconsistent_column_count:
            return "record has a different number of fields than the first record";
        case parse_error_code::too_large:
            return "input too large";
        }
        return "unknown error";
    }

    /**
     * @struct parse_error
     * @brief The failure value of @ref parse_table.
     *
     * Carries the byte offset, like the JSON parser, and additionally the 1-based record and field
     * numbers: a CSV error message that says "line 4102, column 7" is far more useful to someone
     * looking at a spreadsheet than one that says "byte 190334".
     */
    struct parse_error
    {
        parse_error_code code = parse_error_code::none; ///< What went wrong.
        std::size_t offset = 0;                         ///< Byte offset into the input where it went wrong.
        std::size_t line = 0;                           ///< 1-based record number, counting a header record as line 1.
        std::size_t column = 0;                          ///< 1-based field number within the record.

        /**
         * @fn message()
         * @brief Format the error for display, e.g. `"CSV parse error at line 4, column 2 (offset
         * 39): unterminated quoted field"`.
         * @return A freshly allocated string.
         */
        [[nodiscard]] std::string message() const;

        [[nodiscard]] friend bool operator==(const parse_error &, const parse_error &) noexcept = default;
    };

    /**
     * @class type_error
     * @brief Thrown when a field is read as a type its text does not spell.
     *
     * The `as_*` accessors on @ref field are checked accesses in the spirit of `std::vector::at`; a
     * failure means the caller's assumption about the column was wrong. Code that is not sure -- and
     * with CSV, which carries no types at all, that is most code -- should use the `try_*`
     * accessors, which never throw.
     */
    class type_error : public std::runtime_error
    {
    public:
        using std::runtime_error::runtime_error;
    };

} // namespace catalyst::resource::csv
