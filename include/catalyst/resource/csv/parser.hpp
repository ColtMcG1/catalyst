/**
 * @file parser.hpp
 * @brief The CSV parser: @ref catalyst::resource::csv::parse_table and the scanning it is built on.
 * @details The grammar is RFC 4180 relaxed in the ways real files need it to be -- any of CRLF, LF or
 * a lone CR ends a record, the last record need not be terminated, and the dialect decides about
 * headers, trimming and ragged rows.
 *
 * The inner loop is `catalyst::text::scan`, with the stop set that header predicted for CSV: the
 * delimiter, the quote, CR and LF. Because a delimiter is a runtime value and the scanner's stop set
 * is a template parameter, the parser resolves one function pointer per parse -- a SWAR scan for the
 * delimiters that actually occur in the wild, a scalar loop for anything else. The choice is made
 * once, outside the loop, so the pointer is predictable and the loop stays tight.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/text/scan.hpp>

#include <cstddef>
#include <expected>
#include <string_view>

#include "dialect.hpp"
#include "error.hpp"
#include "table.hpp"
#include "tape.hpp"

namespace catalyst::resource::csv::detail
{
    /// @brief Signature of a stop-byte scan: index of the next interesting byte at or after `from`.
    using scan_fn = std::size_t (*)(std::string_view, std::size_t) noexcept;

    /// @brief Scan for the end of an unquoted field: the delimiter, a quote, CR or LF.
    template <char Delimiter, char Quote>
    [[nodiscard]] inline std::size_t scan_field(std::string_view s, std::size_t from) noexcept
    {
        return text::scan::swar<text::scan::control_bytes::allowed, Delimiter, Quote, '\r', '\n'>(s, from);
    }

    /// @brief Scan for the next quote inside a quoted field. CR and LF are content here.
    template <char Quote>
    [[nodiscard]] inline std::size_t scan_quoted(std::string_view s, std::size_t from) noexcept
    {
        return text::scan::swar<text::scan::control_bytes::allowed, Quote>(s, from);
    }

    /**
     * @fn select_field_scan
     * @brief The SWAR field scan for @p d, or `nullptr` if this dialect has no specialization.
     *
     * Only the delimiters that turn up in real files are instantiated. An exotic dialect still parses
     * correctly, just through the scalar fallback, which is the right trade: four instantiations of a
     * small function against a table of every possible byte.
     */
    [[nodiscard]] scan_fn select_field_scan(const dialect &d) noexcept;

    /// @brief The SWAR quoted-body scan for @p d, or `nullptr` for a non-standard quote.
    [[nodiscard]] scan_fn select_quoted_scan(const dialect &d) noexcept;

    /**
     * @class parser
     * @brief One parse of one buffer. Not reusable; @ref parse_table constructs one and runs it.
     */
    class parser
    {
    public:
        parser(std::string_view text, const dialect &d)
            : text_(text), dialect_(d), builder_(text.size()), scan_field_(select_field_scan(d)),
              scan_quoted_(select_quoted_scan(d))
        {
        }

        [[nodiscard]] std::expected<table, parse_error> run();

    private:
        /// @brief Parse one record: at least one field, delimiters between them, stopping at a newline.
        [[nodiscard]] std::expected<void, parse_error> parse_record();

        [[nodiscard]] std::expected<void, parse_error> parse_field();

        /**
         * @fn parse_unquoted_field
         * @brief Copy bytes up to the next delimiter or newline.
         *
         * A quote partway through is @ref parse_error_code::bare_quote rather than content. That is
         * stricter than some parsers, and deliberately so: `a"b` is almost always a file that was
         * built by string concatenation and is about to lose data somewhere else too.
         */
        [[nodiscard]] std::expected<void, parse_error> parse_unquoted_field();

        /**
         * @fn parse_quoted_field
         * @brief Copy the body of a quoted field, collapsing each doubled quote to one.
         *
         * Delimiters and newlines inside the quotes are content, which is the whole reason quoting
         * exists; the only byte that means anything here is the quote itself.
         */
        [[nodiscard]] std::expected<void, parse_error> parse_quoted_field();

        /// @brief Consume one CRLF, LF or CR, if the input is sitting on one.
        void consume_newline() noexcept;

        [[nodiscard]] std::size_t next_field_stop(std::size_t from) const noexcept;

        [[nodiscard]] std::size_t next_quote(std::size_t from) const noexcept;

        [[nodiscard]] static std::string_view trim(std::string_view s) noexcept;

        [[nodiscard]] parse_error error_at(parse_error_code code, std::size_t offset) const noexcept;

        std::string_view text_;
        const dialect &dialect_;
        table_builder builder_;
        scan_fn scan_field_ = nullptr;
        scan_fn scan_quoted_ = nullptr;

        std::size_t pos_ = 0;
        std::size_t line_ = 1;   ///< 1-based record number, for error messages.
        std::size_t column_ = 1; ///< 1-based field number within the record, for error messages.
        std::size_t width_ = 0;  ///< Field count of the first record; 0 until one has been closed.
        bool last_field_quoted_ = false;
        bool header_taken_ = false;
    };

} // namespace catalyst::resource::csv::detail

namespace catalyst::resource::csv
{
    /**
     * @fn parse_table
     * @brief Parse @p text as CSV into an owning @ref table.
     *
     * The text is not retained: every field is copied into the table's arena, decoded, so the buffer
     * may be freed as soon as this returns.
     *
     * @param text The CSV text. Empty text yields an empty table, which is not an error.
     * @param d The flavour to read; see @ref dialect. Defaults to RFC 4180 with a header row.
     * @return The table, or the first @ref parse_error found.
     */
    [[nodiscard]] std::expected<table, parse_error> parse_table(std::string_view text,
                                                                const dialect &d = dialect{});

} // namespace catalyst::resource::csv
