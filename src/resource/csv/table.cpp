/**
 * @file table.cpp
 * @brief Implements the owning @ref catalyst::resource::csv::table and the builder the parser fills
 * it through, both declared in table.hpp.
 * @details Every record boundary here is a subtraction between two entries of the offset vectors, so
 * a ragged table costs the same as a rectangular one. The builder is the only code that writes those
 * vectors, and it appends the two sentinels in @ref catalyst::resource::csv::detail::table_builder::finish
 * so that the last field and the last record can be measured the same way as every other.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/resource/csv/table.hpp>

#include <catalyst/resource/csv/tape.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace catalyst::resource::csv
{

    csv::row table::iterator::operator[](difference_type n) const noexcept
    {
        return table_->row(index_ + static_cast<std::size_t>(n));
    }

    csv::row table::row(std::size_t index) const noexcept
    {
        if (index >= size())
            return csv::row();
        const std::uint32_t first = rows_[index];
        const std::uint32_t last = rows_[index + 1];
        return csv::row(arena_.data(), fields_.data(), first, last - first, &header_);
    }

    csv::row table::at(std::size_t index) const
    {
        if (index >= size())
            throw std::out_of_range("CSV table has no row " + std::to_string(index));
        return row(index);
    }

    std::size_t table::columns() const noexcept
    {
        if (has_header())
            return header_.size();
        return empty() ? 0 : row(0).size();
    }

    std::optional<std::size_t> table::column_index(std::string_view name) const noexcept
    {
        return header_.find(name);
    }

    bool table::contains_column(std::string_view name) const noexcept
    {
        return header_.find(name).has_value();
    }

    namespace detail
    {
        table_builder::table_builder(std::size_t input_size)
        {
            arena_.reserve(input_size);
            fields_.reserve(input_size / 8 + 8);
        }

        std::string_view table_builder::end_field() const noexcept
        {
            const std::uint32_t begin = fields_.back();
            return std::string_view(arena_).substr(begin);
        }

        void table_builder::discard_field()
        {
            arena_.resize(fields_.back());
            fields_.pop_back();
        }

        void table_builder::retain_field(std::string_view bytes)
        {
            const std::uint32_t begin = fields_.back();
            const auto offset = static_cast<std::uint32_t>(bytes.data() - arena_.data());
            if (offset != begin)
                arena_.erase(begin, offset - begin);
            arena_.resize(begin + bytes.size());
        }

        std::size_t table_builder::fields_in_record() const noexcept
        {
            const std::size_t start = rows_.empty() ? 0 : rows_.back();
            return fields_.size() - start;
        }

        std::vector<std::string> table_builder::take_record()
        {
            std::vector<std::string> out;
            const std::size_t start = rows_.size() >= 2 ? rows_[rows_.size() - 2] : 0;
            const std::size_t stop = rows_.back();
            out.reserve(stop - start);
            for (std::size_t i = start; i < stop; ++i)
            {
                const std::uint32_t begin = fields_[i];
                const std::uint32_t end =
                    (i + 1 < fields_.size()) ? fields_[i + 1] : static_cast<std::uint32_t>(arena_.size());
                out.emplace_back(arena_, begin, end - begin);
            }
            arena_.resize(fields_[start]);
            fields_.resize(start);
            rows_.pop_back();
            return out;
        }

        table table_builder::finish(std::vector<std::string> header)
        {
            table t;
            if (!rows_.empty())
            {
                // Sentinels: one past the last field, and one past the last record.
                fields_.push_back(static_cast<std::uint32_t>(arena_.size()));
                rows_.insert(rows_.begin(), 0);
            }
            t.arena_ = std::move(arena_);
            t.fields_ = std::move(fields_);
            t.rows_ = std::move(rows_);
            if (!header.empty())
                t.header_.assign(std::move(header));
            return t;
        }

    } // namespace detail
} // namespace catalyst::resource::csv
