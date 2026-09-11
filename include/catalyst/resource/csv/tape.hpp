/**
 * @file tape.hpp
 * @brief The flat "tape" encoding of a parsed CSV table and the non-owning @ref field and @ref row
 * handles that navigate it.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <cstddef>
#include <cstdint>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "error.hpp"

namespace catalyst::resource::csv
{
    // -----------------------------------------------------------------------------------------------
    // Tape: a flat, allocation-free representation of a parsed table
    // -----------------------------------------------------------------------------------------------
    //
    // JSON's tape has to encode a tree, so each node carries a tag and containers carry a skip index.
    // A CSV table is a rectangle -- possibly a ragged one -- and needs none of that. Three buffers say
    // everything:
    //
    //   arena   : every field's bytes, concatenated in row-major order, unquoted and unescaped.
    //   fields  : arena offset of each field, plus one trailing sentinel. Field i is
    //             [fields[i], fields[i + 1]), so a field's length costs a subtraction.
    //   rows    : index into `fields` where each record starts, plus one trailing sentinel. Record r
    //             spans fields [rows[r], rows[r + 1]), so its width is also a subtraction -- which is
    //             what makes a ragged table cost nothing extra to support.
    //
    // The result is three allocations for a whole file, no per-field string, and O(1) access to any
    // cell. The header, when there is one, is held separately as owned strings: it is at most a few
    // dozen names, it is not data, and keeping it out of the arena means row 0 is the first data row.

    namespace detail
    {
        /**
         * @class header_map
         * @brief The column names of a table, and a name-to-index lookup over them.
         *
         * The lookup is a vector of column indices kept sorted by name, binary-searched on demand.
         * A hash map would be asymptotically better and, at the couple of dozen columns a real CSV
         * has, measurably worse: the sorted vector is one allocation and stays in cache.
         */
        class header_map
        {
        public:
            header_map() = default;

            /// @brief Take ownership of @p names and build the lookup index.
            void assign(std::vector<std::string> names);

            [[nodiscard]] bool empty() const noexcept { return names_.empty(); }
            [[nodiscard]] std::size_t size() const noexcept { return names_.size(); }

            /// @brief The name of column @p index; empty if out of range.
            [[nodiscard]] std::string_view name(std::size_t index) const noexcept;

            [[nodiscard]] const std::vector<std::string> &names() const noexcept { return names_; }

            /**
             * @fn find
             * @brief The index of the column named @p name.
             * @return The column index, or `nullopt`. When a name repeats, the lowest index wins, so
             *         a duplicated column resolves to the first one -- the same thing a spreadsheet does.
             */
            [[nodiscard]] std::optional<std::size_t> find(std::string_view name) const noexcept;

        private:
            std::vector<std::string> names_{};  ///< Column names in column order.
            std::vector<std::uint32_t> order_{}; ///< Indices into @ref names_, sorted by name.
        };

    } // namespace detail

    /**
     * @class field
     * @brief One cell of a table: its decoded bytes, plus the conversions a caller usually wants.
     *
     * CSV carries no types, so every field is text and every conversion can fail. That is why the
     * `try_*` accessors are the ones to reach for; the `as_*` accessors exist for the cases where a
     * column's contents are guaranteed by something outside the file, and they throw
     * @ref type_error when that guarantee turns out to be wrong.
     *
     * A field borrows from the table that produced it and must not outlive it.
     */
    class field
    {
    public:
        field() noexcept = default;
        explicit field(std::string_view text) noexcept : text_(text) {}

        /// @brief The field's bytes: quotes removed and doubled quotes collapsed.
        [[nodiscard]] std::string_view view() const noexcept { return text_; }
        operator std::string_view() const noexcept { return text_; }

        [[nodiscard]] bool empty() const noexcept { return text_.empty(); }
        [[nodiscard]] std::size_t size() const noexcept { return text_.size(); }

        /**
         * @fn try_int
         * @brief Parse the whole field as a base-10 integer.
         * @return The value, or `nullopt` if the field is empty, is not entirely digits (with an
         *         optional leading `-`), or does not fit an `int64_t`.
         */
        [[nodiscard]] std::optional<std::int64_t> try_int() const noexcept;

        /**
         * @fn try_double
         * @brief Parse the whole field as a floating-point number.
         * @return The value, or `nullopt` if the field is not entirely a number. A leading `+` is
         *         rejected, matching `std::from_chars`.
         */
        [[nodiscard]] std::optional<double> try_double() const noexcept;

        /**
         * @fn try_bool
         * @brief Parse the field as a boolean.
         *
         * Accepts `true`/`false`, `yes`/`no`, `y`/`n`, `1`/`0` and `t`/`f`, in any case. This list is
         * deliberately generous because a boolean column in a hand-edited CSV is spelled whichever
         * way the person editing it felt like that day.
         *
         * @return The value, or `nullopt` if the field is none of those.
         */
        [[nodiscard]] std::optional<bool> try_bool() const noexcept;

        /// @brief Like @ref try_int, but throws @ref type_error instead of returning `nullopt`.
        [[nodiscard]] std::int64_t as_int() const;

        /// @brief Like @ref try_double, but throws @ref type_error instead of returning `nullopt`.
        [[nodiscard]] double as_double() const;

        /// @brief Like @ref try_bool, but throws @ref type_error instead of returning `nullopt`.
        [[nodiscard]] bool as_bool() const;

        /// @brief A copy of the field's bytes.
        [[nodiscard]] std::string to_string() const { return std::string(text_); }

        [[nodiscard]] friend bool operator==(const field &a, const field &b) noexcept { return a.text_ == b.text_; }
        [[nodiscard]] friend bool operator==(const field &a, std::string_view b) noexcept { return a.text_ == b; }

    private:
        std::string_view text_{};
    };

    /**
     * @class row
     * @brief A non-owning handle to one record of a @ref table.
     *
     * Rows are cheap to copy, iterate over their fields, and can look a field up by column name when
     * the table had a header. A default-constructed row is empty, which is what an out-of-range
     * @ref table::row returns. Rows borrow from their table and must not outlive it.
     */
    class row
    {
    public:
        /// @brief A random-access iterator over the fields of a row.
        class iterator
        {
        public:
            using iterator_category = std::random_access_iterator_tag;
            using value_type = field;
            using difference_type = std::ptrdiff_t;
            using reference = field;
            using pointer = void;

            iterator() noexcept = default;
            iterator(const row *r, std::size_t i) noexcept : row_(r), index_(i) {}

            [[nodiscard]] field operator*() const noexcept { return (*row_)[index_]; }
            [[nodiscard]] field operator[](difference_type n) const noexcept;

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
            const row *row_ = nullptr;
            std::size_t index_ = 0;
        };

        row() noexcept = default;

        /// @brief Construct a row over a slice of a table's field index.
        row(const char *arena, const std::uint32_t *fields, std::uint32_t first, std::uint32_t count,
            const detail::header_map *header) noexcept
            : arena_(arena), fields_(fields), first_(first), count_(count), header_(header)
        {
        }

        /// @brief The number of fields in this record. May differ between rows of a ragged table.
        [[nodiscard]] std::size_t size() const noexcept { return count_; }
        [[nodiscard]] bool empty() const noexcept { return count_ == 0; }

        /**
         * @fn operator[](std::size_t)
         * @brief The field at column @p index.
         * @return The field, or an empty field if @p index is past the end of this record. Reading
         *         past the end is not an error because a ragged table makes it routine: a row that
         *         stops early has no value for the columns it stopped before.
         */
        [[nodiscard]] field operator[](std::size_t index) const noexcept;

        /**
         * @fn operator[](std::string_view)
         * @brief The field in the column named @p name.
         * @return The field, or an empty field if the table had no header or has no such column. Use
         *         @ref table::column_index when the difference between "absent" and "empty" matters.
         */
        [[nodiscard]] field operator[](std::string_view name) const noexcept;

        /**
         * @fn at(std::size_t)
         * @brief Checked access by column index.
         * @throws std::out_of_range if @p index is past the end of this record.
         */
        [[nodiscard]] field at(std::size_t index) const;

        /**
         * @fn at(std::string_view)
         * @brief Checked access by column name.
         * @throws std::out_of_range if the table has no header or no column by that name.
         */
        [[nodiscard]] field at(std::string_view name) const;

        /// @return `true` if the table had a header naming @p name.
        [[nodiscard]] bool contains(std::string_view name) const noexcept;

        [[nodiscard]] iterator begin() const noexcept { return iterator(this, 0); }
        [[nodiscard]] iterator end() const noexcept { return iterator(this, count_); }

    private:
        const char *arena_ = nullptr;
        const std::uint32_t *fields_ = nullptr;
        std::uint32_t first_ = 0;
        std::uint32_t count_ = 0;
        const detail::header_map *header_ = nullptr;
    };

} // namespace catalyst::resource::csv
