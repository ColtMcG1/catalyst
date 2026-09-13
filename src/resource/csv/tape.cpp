/**
 * @file tape.cpp
 * @brief Implements the tape handles declared in tape.hpp: column lookup, the typed @ref
 * catalyst::resource::csv::field accessors, and @ref catalyst::resource::csv::row access.
 * @details A field's conversions are all `std::from_chars` over the field's own bytes, which is why
 * they are exact and why they refuse trailing junk: the whole field must be the number, because a
 * cell that reads `12kg` is a mistake in the file rather than the number twelve.
 * License: MIT (see LICENSE).
 */

#include <catalyst/resource/csv/tape.hpp>

#include <catalyst/resource/csv/error.hpp>

#include <algorithm>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace catalyst::resource::csv
{

    namespace detail
    {
        void header_map::assign(std::vector<std::string> names)
        {
            names_ = std::move(names);
            order_.resize(names_.size());
            for (std::size_t i = 0; i < order_.size(); ++i)
                order_[i] = static_cast<std::uint32_t>(i);
            std::sort(order_.begin(), order_.end(),
                      [this](std::uint32_t a, std::uint32_t b) { return names_[a] < names_[b]; });
        }

        std::string_view header_map::name(std::size_t index) const noexcept
        {
            return index < names_.size() ? std::string_view(names_[index]) : std::string_view();
        }

        std::optional<std::size_t> header_map::find(std::string_view name) const noexcept
        {
            const auto it = std::lower_bound(order_.begin(), order_.end(), name,
                                             [this](std::uint32_t a, std::string_view n)
                                             { return std::string_view(names_[a]) < n; });
            std::optional<std::size_t> best;
            for (auto i = it; i != order_.end() && std::string_view(names_[*i]) == name; ++i)
                if (!best || *i < *best)
                    best = *i;
            return best;
        }

    } // namespace detail

    std::optional<std::int64_t> field::try_int() const noexcept
    {
        std::int64_t v = 0;
        const char *const begin = text_.data();
        const char *const end = begin + text_.size();
        const auto [ptr, ec] = std::from_chars(begin, end, v);
        if (ec != std::errc() || ptr != end)
            return std::nullopt;
        return v;
    }

    std::optional<double> field::try_double() const noexcept
    {
        double v = 0.0;
        const char *const begin = text_.data();
        const char *const end = begin + text_.size();
        const auto [ptr, ec] = std::from_chars(begin, end, v);
        if (ec != std::errc() || ptr != end)
            return std::nullopt;
        return v;
    }

    std::optional<bool> field::try_bool() const noexcept
    {
        if (text_.empty() || text_.size() > 5)
            return std::nullopt;

        char lowered[5]{};
        for (std::size_t i = 0; i < text_.size(); ++i)
        {
            const char c = text_[i];
            lowered[i] = (c >= 'A' && c <= 'Z') ? static_cast<char>(c + ('a' - 'A')) : c;
        }
        const std::string_view s(lowered, text_.size());

        if (s == "true" || s == "yes" || s == "y" || s == "1" || s == "t")
            return true;
        if (s == "false" || s == "no" || s == "n" || s == "0" || s == "f")
            return false;
        return std::nullopt;
    }

    std::int64_t field::as_int() const
    {
        if (const auto v = try_int())
            return *v;
        throw type_error("CSV field is not an integer: '" + std::string(text_) + "'");
    }

    double field::as_double() const
    {
        if (const auto v = try_double())
            return *v;
        throw type_error("CSV field is not a number: '" + std::string(text_) + "'");
    }

    bool field::as_bool() const
    {
        if (const auto v = try_bool())
            return *v;
        throw type_error("CSV field is not a boolean: '" + std::string(text_) + "'");
    }

    field row::iterator::operator[](difference_type n) const noexcept
    {
        return (*row_)[index_ + static_cast<std::size_t>(n)];
    }

    field row::operator[](std::size_t index) const noexcept
    {
        if (index >= count_)
            return field();
        const std::uint32_t begin = fields_[first_ + index];
        const std::uint32_t end = fields_[first_ + index + 1];
        return field(std::string_view(arena_ + begin, end - begin));
    }

    field row::operator[](std::string_view name) const noexcept
    {
        if (header_ == nullptr)
            return field();
        const auto index = header_->find(name);
        return index ? (*this)[*index] : field();
    }

    field row::at(std::size_t index) const
    {
        if (index >= count_)
            throw std::out_of_range("CSV row has no column " + std::to_string(index));
        return (*this)[index];
    }

    field row::at(std::string_view name) const
    {
        if (header_ == nullptr)
            throw std::out_of_range("CSV table has no header, so column '" + std::string(name) +
                                    "' cannot be looked up by name");
        const auto index = header_->find(name);
        if (!index)
            throw std::out_of_range("CSV table has no column named '" + std::string(name) + "'");
        return at(*index);
    }

    bool row::contains(std::string_view name) const noexcept
    {
        return header_ != nullptr && header_->find(name).has_value();
    }

} // namespace catalyst::resource::csv
