/**
 * @file serializer.hpp
 * @brief Writing CSV: the incremental @ref catalyst::resource::csv::writer and the whole-table
 * @ref catalyst::resource::csv::dump.
 * @details The writer quotes a field exactly when leaving it bare would not read back as the same
 * field, and no more often than that. Quoting everything would also be correct and would make every
 * file bigger and every diff noisier, so the round trip is pinned by tests instead of by caution.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "dialect.hpp"
#include "table.hpp"
#include "tape.hpp"

namespace catalyst::resource::csv
{
    /**
     * @fn needs_quoting
     * @brief Whether @p text must be quoted to survive a round trip under @p d.
     *
     * True when the field contains the delimiter, the quote, a CR or an LF -- the four bytes that
     * would otherwise be read as structure. Also true for leading or trailing spaces and tabs when
     * the dialect trims, since trimming would eat them on the way back in.
     */
    [[nodiscard]] bool needs_quoting(std::string_view text, const dialect &d) noexcept;

    /**
     * @class writer
     * @brief Builds CSV text one field at a time.
     *
     * Use this when the rows are being produced rather than read from a @ref table -- exporting a
     * container, dumping a stat block, streaming a log. Call @ref field for each cell and
     * @ref end_row to close the record; the newline the dialect names is written between records.
     */
    class writer
    {
    public:
        writer() = default;
        explicit writer(dialect d) : dialect_(d) {}

        /// @brief Reserve space in the output buffer, if the size is known up front.
        void reserve(std::size_t bytes) { out_.reserve(bytes); }

        /**
         * @fn field
         * @brief Append one field to the record in progress, quoting it if it needs it.
         * @param text The field's bytes, undecorated: quotes are added here, not by the caller.
         * @return `*this`, so calls chain.
         */
        writer &field(std::string_view text);

        /// @brief Append a field already known not to need quoting, without checking. The caller owns
        /// that claim; getting it wrong produces a file that will not read back.
        writer &raw_field(std::string_view text);

        /**
         * @fn end_row
         * @brief Close the record in progress.
         *
         * A record with no fields writes an empty line, which @ref parse_table will skip unless the
         * dialect keeps blank lines.
         */
        writer &end_row();

        /// @brief Append a whole record from a @ref row.
        writer &row(const csv::row &r);

        /// @return `true` if nothing has been written yet.
        [[nodiscard]] bool empty() const noexcept { return out_.empty(); }

        /// @brief The text written so far.
        [[nodiscard]] const std::string &str() const noexcept { return out_; }

        /// @brief Move the text out, leaving the writer empty and reusable.
        [[nodiscard]] std::string take();

    private:
        dialect dialect_{};
        std::string out_{};
        bool first_in_row_ = true;
    };

    /**
     * @fn dump
     * @brief Serialize a whole @ref table.
     *
     * The header is written first when the table has one. Passing the dialect the table was parsed
     * with round-trips the file; passing a different one converts between flavours.
     *
     * @param t The table to write.
     * @param d The flavour to write. Defaults to RFC 4180.
     * @return A freshly allocated string.
     */
    [[nodiscard]] std::string dump(const table &t, const dialect &d = dialect{});

} // namespace catalyst::resource::csv
