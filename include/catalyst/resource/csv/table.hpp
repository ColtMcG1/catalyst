/**
 * @file table.hpp
 * @brief The owning @ref table that holds a parsed CSV file in tape form, and the builder the parser
 * fills it through.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "dialect.hpp"
#include "tape.hpp"

namespace catalyst::resource::csv
{
    namespace detail
    {
        class table_builder; // fills a table's buffers; befriended below.
    }

    /**
     * @class table
     * @brief An owning, flat ("tape") representation of a parsed CSV file.
     *
     * Produced by @ref parse_table. Holds every field in three buffers with no per-field allocation;
     * see the layout note in tape.hpp. Iterate it with a range-for to walk the data records, or index
     * it for random access. The @ref row and @ref field handles it hands out borrow from it and must
     * not outlive it.
     *
     * A header record, if the dialect said there was one, is not a data row: @ref size counts only
     * data, @ref row(0) is the first data record, and the names are reachable through @ref header.
     */
    class table
    {
    public:
        /// @brief A random-access iterator over the data records of a table.
        class iterator
        {
        public:
            using iterator_category = std::random_access_iterator_tag;
            using value_type = csv::row;
            using difference_type = std::ptrdiff_t;
            using reference = csv::row;
            using pointer = void;

            iterator() noexcept = default;
            iterator(const table *t, std::size_t i) noexcept : table_(t), index_(i) {}

            [[nodiscard]] csv::row operator*() const noexcept { return table_->row(index_); }
            [[nodiscard]] csv::row operator[](difference_type n) const noexcept;

            iterator &operator++() noexcept { ++index_; return *this; }
            iterator operator++(int) noexcept { iterator t = *this; ++index_; return t; }
            iterator &operator--() noexcept { --index_; return *this; }
            iterator operator--(int) noexcept { iterator t = *this; --index_; return t; }
            iterator &operator+=(difference_type n) noexcept { index_ += static_cast<std::size_t>(n); return *this; }
            iterator &operator-=(difference_type n) noexcept { index_ -= static_cast<std::size_t>(n); return *this; }

            [[nodiscard]] friend iterator operator+(iterator i, difference_type n) noexcept { return i += n; }
            [[nodiscard]] friend iterator operator+(difference_type n, iterator i) noexcept { return i += n; }
            [[nodiscard]] friend iterator operator-(iterator i, difference_type n) noexcept { return i -= n; }
            [[nodiscard]] friend difference_type operator-(const iterator &a, const iterator &b) noexcept
            {
                return static_cast<difference_type>(a.index_) - static_cast<difference_type>(b.index_);
            }

            [[nodiscard]] friend bool operator==(const iterator &a, const iterator &b) noexcept
            {
                return a.index_ == b.index_;
            }
            [[nodiscard]] friend auto operator<=>(const iterator &a, const iterator &b) noexcept
            {
                return a.index_ <=> b.index_;
            }

        private:
            const table *table_ = nullptr;
            std::size_t index_ = 0;
        };

        table() = default;

        // -----------------------------------------------------------------
        // Records
        // -----------------------------------------------------------------

        /// @brief The number of data records, not counting a header.
        [[nodiscard]] std::size_t size() const noexcept { return rows_.empty() ? 0 : rows_.size() - 1; }

        /// @return `true` if there are no data records. A file with only a header is empty.
        [[nodiscard]] bool empty() const noexcept { return size() == 0; }

        /**
         * @fn row(std::size_t)
         * @brief The data record at @p index.
         * @return The record, or an empty row if @p index is out of range.
         */
        [[nodiscard]] csv::row row(std::size_t index) const noexcept;

        /// @brief The data record at @p index, unchecked in the same way as @ref row.
        [[nodiscard]] csv::row operator[](std::size_t index) const noexcept { return row(index); }

        /**
         * @fn at(std::size_t)
         * @brief Checked access to a data record.
         * @throws std::out_of_range if @p index is past the last record.
         */
        [[nodiscard]] csv::row at(std::size_t index) const;

        [[nodiscard]] iterator begin() const noexcept { return iterator(this, 0); }
        [[nodiscard]] iterator end() const noexcept { return iterator(this, size()); }

        // -----------------------------------------------------------------
        // Columns
        // -----------------------------------------------------------------

        /// @return `true` if the file was parsed with a header record.
        [[nodiscard]] bool has_header() const noexcept { return !header_.empty(); }

        /// @brief The column names, in column order. Empty when there was no header.
        [[nodiscard]] const std::vector<std::string> &header() const noexcept { return header_.names(); }

        /**
         * @fn columns()
         * @brief The table's width: the number of header names, or the width of the first data
         * record when there is no header.
         * @note On a ragged table this is the width of the first record, not the widest one; ask a
         *       @ref row for its own @ref row::size.
         */
        [[nodiscard]] std::size_t columns() const noexcept;

        /**
         * @fn column_index
         * @brief The index of the column named @p name.
         * @return The index, or `nullopt` if there is no header or no such column. This is the
         *         lookup to use when "the column is missing" and "the cell is empty" must be told apart.
         */
        [[nodiscard]] std::optional<std::size_t> column_index(std::string_view name) const noexcept;

        /// @return `true` if the table has a header naming @p name.
        [[nodiscard]] bool contains_column(std::string_view name) const noexcept;

        /// @brief The total number of fields across every data record. Useful for sizing a copy.
        [[nodiscard]] std::size_t field_count() const noexcept { return fields_.empty() ? 0 : fields_.size() - 1; }

    private:
        friend class detail::table_builder;

        std::string arena_{};                 ///< Every field's decoded bytes, row-major.
        std::vector<std::uint32_t> fields_{}; ///< Arena offset of each field, plus a trailing sentinel.
        std::vector<std::uint32_t> rows_{};   ///< Field index each record starts at, plus a trailing sentinel.
        detail::header_map header_{};         ///< Column names and their lookup index; empty if headerless.
    };

    namespace detail
    {
        /**
         * @class table_builder
         * @brief Accumulates fields and records into a @ref table.
         *
         * The JSON parser talks to its two document representations through a templated sink, because
         * it really does have two. CSV has one, so this is a plain class the parser calls directly
         * rather than a sink protocol with a single implementation -- the indirection would be all
         * cost and no flexibility.
         *
         * The contract is: @ref begin_field / @ref append / @ref end_field per field, @ref end_record
         * per record, then @ref finish once.
         */
        class table_builder
        {
        public:
            /**
             * @param input_size Size of the text about to be parsed, used to reserve the buffers
             *        once. Decoded fields never exceed the input (unquoting only removes bytes), and
             *        one field per ~8 input bytes is a comfortable estimate for a real table, so
             *        neither buffer normally grows again mid-parse.
             */
            explicit table_builder(std::size_t input_size);

            /// @brief Start a field. Its bytes are whatever @ref append adds before @ref end_field.
            void begin_field() { fields_.push_back(static_cast<std::uint32_t>(arena_.size())); }

            /// @brief Append decoded bytes to the field in progress.
            void append(std::string_view bytes) { arena_.append(bytes); }

            /// @brief Append one decoded byte to the field in progress.
            void append(char byte) { arena_.push_back(byte); }

            /// @brief Finish the field in progress and return its decoded bytes.
            [[nodiscard]] std::string_view end_field() const noexcept;

            /// @brief Discard the field in progress, for a blank line the dialect drops.
            void discard_field();

            /// @brief Replace the field in progress with @p bytes, which must be a subrange of it.
            void retain_field(std::string_view bytes);

            /// @brief Close the record whose fields have been emitted since the last call.
            void end_record() { rows_.push_back(static_cast<std::uint32_t>(fields_.size())); }

            /// @brief The number of fields emitted in the record currently in progress.
            [[nodiscard]] std::size_t fields_in_record() const noexcept;

            /// @brief The number of records closed so far.
            [[nodiscard]] std::size_t record_count() const noexcept { return rows_.size(); }

            /// @brief Total bytes of field data, for the overflow check.
            [[nodiscard]] std::size_t arena_size() const noexcept { return arena_.size(); }

            /// @brief Total fields emitted, for the overflow check.
            [[nodiscard]] std::size_t total_fields() const noexcept { return fields_.size(); }

            /**
             * @fn take_record
             * @brief Remove the last closed record and return its fields as owned strings.
             *
             * How the header is lifted out of the tape once it has been parsed like any other record:
             * it is the first record, it is not data, and the parser does not know whether to keep it
             * until the dialect says so.
             */
            [[nodiscard]] std::vector<std::string> take_record();

            /// @brief Seal the buffers into a table, moving @p header in as its column names.
            [[nodiscard]] table finish(std::vector<std::string> header);

        private:
            std::string arena_{};
            std::vector<std::uint32_t> fields_{};
            std::vector<std::uint32_t> rows_{}; ///< End-of-record marks; the leading 0 is added by @ref finish.
        };

    } // namespace detail

} // namespace catalyst::resource::csv
