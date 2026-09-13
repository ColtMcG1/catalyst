/**
 * @file parser.cpp
 * @brief Implements the CSV parser declared in parser.hpp.
 * @details The inner loop is one call to the scan function the dialect selected, then one append.
 * Which scan that is gets decided once, in the constructor, so the indirect call is perfectly
 * predicted and the per-byte work is the scan itself.
 * License: MIT (see LICENSE).
 */

#include <catalyst/resource/csv/parser.hpp>

#include <catalyst/resource/csv/dialect.hpp>
#include <catalyst/resource/csv/error.hpp>
#include <catalyst/resource/csv/table.hpp>
#include <catalyst/resource/csv/tape.hpp>

#include <cstddef>
#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace catalyst::resource::csv::detail
{

    scan_fn select_field_scan(const dialect &d) noexcept
    {
        if (d.quote != '"')
            return nullptr;
        switch (d.delimiter)
        {
        case ',':
            return &scan_field<',', '"'>;
        case '\t':
            return &scan_field<'\t', '"'>;
        case ';':
            return &scan_field<';', '"'>;
        case '|':
            return &scan_field<'|', '"'>;
        default:
            return nullptr;
        }
    }

    scan_fn select_quoted_scan(const dialect &d) noexcept
    {
        return d.quote == '"' ? &scan_quoted<'"'> : nullptr;
    }

    std::expected<table, parse_error> parser::run()
    {
        std::vector<std::string> header;

        while (pos_ < text_.size())
        {
            const std::size_t record_start = pos_;
            if (auto r = parse_record(); !r)
                return std::unexpected(r.error());

            const std::size_t width = builder_.fields_in_record();

            // A blank line parses as one empty unquoted field; the dialect decides whether that
            // is data. `""` alone on a line is quoted, so it is never blank.
            if (dialect_.skip_blank_lines && width == 1 && !last_field_quoted_ && builder_.end_field().empty())
            {
                builder_.discard_field();
            }
            else
            {
                if (width_ == 0)
                {
                    width_ = width; // the first record, header or not, fixes the rectangle
                }
                else if (!dialect_.allow_ragged && width != width_)
                {
                    // Point at the first column that differs: the one this record is missing, or
                    // the first one it has too many of.
                    const std::size_t at = (width < width_ ? width : width_) + 1;
                    return std::unexpected(
                        parse_error{parse_error_code::inconsistent_column_count, record_start, line_, at});
                }

                builder_.end_record();
                if (dialect_.has_header && !header_taken_)
                {
                    header = builder_.take_record();
                    header_taken_ = true;
                }
            }

            consume_newline();
            ++line_;
        }

        if (builder_.arena_size() > 0xFFFFFFFEull || builder_.total_fields() > 0xFFFFFFFEull)
            return std::unexpected(parse_error{parse_error_code::too_large, text_.size(), line_, 1});

        return builder_.finish(std::move(header));
    }

    std::expected<void, parse_error> parser::parse_record()
    {
        column_ = 1;
        for (;;)
        {
            if (auto r = parse_field(); !r)
                return r;
            if (pos_ < text_.size() && text_[pos_] == dialect_.delimiter)
            {
                ++pos_;
                ++column_;
                continue;
            }
            return {};
        }
    }

    std::expected<void, parse_error> parser::parse_field()
    {
        builder_.begin_field();
        last_field_quoted_ = (pos_ < text_.size() && text_[pos_] == dialect_.quote);
        if (last_field_quoted_)
            return parse_quoted_field();
        return parse_unquoted_field();
    }

    std::expected<void, parse_error> parser::parse_unquoted_field()
    {
        const std::size_t stop = next_field_stop(pos_);
        if (stop < text_.size() && text_[stop] == dialect_.quote)
            return std::unexpected(error_at(parse_error_code::bare_quote, stop));

        builder_.append(text_.substr(pos_, stop - pos_));
        pos_ = stop;

        if (dialect_.trim_whitespace)
            builder_.retain_field(trim(builder_.end_field()));
        return {};
    }

    std::expected<void, parse_error> parser::parse_quoted_field()
    {
        const std::size_t open = pos_;
        ++pos_; // opening quote

        for (;;)
        {
            const std::size_t q = next_quote(pos_);
            if (q >= text_.size())
                return std::unexpected(error_at(parse_error_code::unterminated_quote, open));

            builder_.append(text_.substr(pos_, q - pos_));

            if (q + 1 < text_.size() && text_[q + 1] == dialect_.quote)
            {
                builder_.append(dialect_.quote); // doubled quote -> one literal quote
                pos_ = q + 2;
                continue;
            }

            pos_ = q + 1;
            break;
        }

        // Only a delimiter, a newline or the end of input may follow a closing quote. Trailing
        // whitespace is tolerated when the dialect trims, since that is what trimming means.
        if (dialect_.trim_whitespace)
            while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t'))
                ++pos_;

        if (pos_ < text_.size() && text_[pos_] != dialect_.delimiter && text_[pos_] != '\r' &&
            text_[pos_] != '\n')
            return std::unexpected(error_at(parse_error_code::invalid_quoted_escape, pos_));

        return {};
    }

    void parser::consume_newline() noexcept
    {
        if (pos_ >= text_.size())
            return;
        if (text_[pos_] == '\r')
        {
            ++pos_;
            if (pos_ < text_.size() && text_[pos_] == '\n')
                ++pos_;
        }
        else if (text_[pos_] == '\n')
        {
            ++pos_;
        }
    }

    std::size_t parser::next_field_stop(std::size_t from) const noexcept
    {
        if (scan_field_ != nullptr)
            return scan_field_(text_, from);
        const std::size_t n = text_.size();
        std::size_t i = from;
        while (i < n)
        {
            const char c = text_[i];
            if (c == dialect_.delimiter || c == dialect_.quote || c == '\r' || c == '\n')
                break;
            ++i;
        }
        return i;
    }

    std::size_t parser::next_quote(std::size_t from) const noexcept
    {
        if (scan_quoted_ != nullptr)
            return scan_quoted_(text_, from);
        const std::size_t n = text_.size();
        std::size_t i = from;
        while (i < n && text_[i] != dialect_.quote)
            ++i;
        return i;
    }

    std::string_view parser::trim(std::string_view s) noexcept
    {
        while (!s.empty() && (s.front() == ' ' || s.front() == '\t'))
            s.remove_prefix(1);
        while (!s.empty() && (s.back() == ' ' || s.back() == '\t'))
            s.remove_suffix(1);
        return s;
    }

    parse_error parser::error_at(parse_error_code code, std::size_t offset) const noexcept
    {
        return parse_error{code, offset, line_, column_};
    }

} // namespace catalyst::resource::csv::detail

namespace catalyst::resource::csv
{

    std::expected<table, parse_error> parse_table(std::string_view text,
                                                                      const dialect &d)
    {
        return detail::parser(text, d).run();
    }

} // namespace catalyst::resource::csv