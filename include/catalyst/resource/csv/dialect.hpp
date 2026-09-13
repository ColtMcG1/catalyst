/**
 * @file dialect.hpp
 * @brief The @ref catalyst::resource::csv::dialect knobs that describe a particular flavour of CSV.
 * @details "CSV" names a family, not a format: the delimiter is a comma except when it is a tab or a
 * semicolon, the first record is a header except when it is data, and rows are all the same width
 * except when they are not. Rather than spread those choices across parser and writer overloads,
 * they live in one aggregate that both sides take, so a file read with a dialect writes back out
 * with the same one.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <cstdint>
#include <string_view>

namespace catalyst::resource::csv
{
    /**
     * @enum line_ending
     * @brief Which newline @ref dump writes between records.
     *
     * Only the writer consults this. The parser always accepts all three of CRLF, LF and a lone CR,
     * because a file that has been through a text editor or a version control system may contain
     * any of them and refusing one would be refusing the file.
     */
    enum class line_ending : std::uint8_t
    {
        crlf = 0, ///< `\r\n`, as RFC 4180 specifies.
        lf = 1    ///< `\n`.
    };

    /**
     * @struct dialect
     * @brief How to read and write one flavour of delimiter-separated text.
     *
     * The defaults are RFC 4180 with a header row, which is what a spreadsheet exports.
     */
    struct dialect
    {
        /// The field separator. Any byte except the quote, CR and LF.
        char delimiter = ',';

        /// The quote character. A field beginning with it is read as quoted, and a quote inside such
        /// a field is written twice.
        char quote = '"';

        /// Whether the first record names the columns. When `true` it is not a data row, and
        /// @ref table::column_index can look columns up by name.
        bool has_header = true;

        /// Strip leading and trailing spaces and tabs from unquoted fields. Quoted fields are never
        /// trimmed: the quotes are precisely how a file says the whitespace is data.
        bool trim_whitespace = false;

        /// Permit records with differing field counts. When `false` a mismatch is
        /// @ref parse_error_code::inconsistent_column_count.
        bool allow_ragged = false;

        /// Drop records that are one empty unquoted field, i.e. blank lines. Written `""`, a blank
        /// line is a real one-field record and is kept regardless.
        bool skip_blank_lines = true;

        /// Newline written between records by @ref dump. Ignored when parsing.
        line_ending newline = line_ending::crlf;

        [[nodiscard]] friend constexpr bool operator==(const dialect &, const dialect &) noexcept = default;

        /// @brief RFC 4180 comma-separated values with a header row. Same as a default-constructed dialect.
        [[nodiscard]] static constexpr dialect comma() noexcept { return dialect{}; }

        /// @brief Tab-separated values with a header row.
        [[nodiscard]] static constexpr dialect tab() noexcept
        {
            dialect d;
            d.delimiter = '\t';
            return d;
        }

        /// @brief Semicolon-separated values, the shape a European spreadsheet locale exports.
        [[nodiscard]] static constexpr dialect semicolon() noexcept
        {
            dialect d;
            d.delimiter = ';';
            return d;
        }
    };

} // namespace catalyst::resource::csv
