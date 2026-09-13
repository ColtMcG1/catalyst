/**
 * @file serializer.cpp
 * @brief Implements the CSV writer and @ref catalyst::resource::csv::dump, declared in
 * serializer.hpp.
 * @details Quoting is decided per field by @ref catalyst::resource::csv::needs_quoting rather than
 * applied to everything, because a file that quotes only what it must is the one a human opening it
 * in a spreadsheet expects to see, and it round-trips identically either way.
 * License: MIT (see LICENSE).
 */

#include <catalyst/resource/csv/serializer.hpp>

#include <catalyst/resource/csv/dialect.hpp>
#include <catalyst/resource/csv/table.hpp>
#include <catalyst/resource/csv/tape.hpp>

#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace catalyst::resource::csv
{

    bool needs_quoting(std::string_view text, const dialect &d) noexcept
    {
        for (const char c : text)
            if (c == d.delimiter || c == d.quote || c == '\r' || c == '\n')
                return true;

        if (d.trim_whitespace && !text.empty())
        {
            const char first = text.front();
            const char last = text.back();
            if (first == ' ' || first == '\t' || last == ' ' || last == '\t')
                return true;
        }
        return false;
    }

    writer &writer::field(std::string_view text)
    {
        if (!first_in_row_)
            out_.push_back(dialect_.delimiter);
        first_in_row_ = false;

        if (!needs_quoting(text, dialect_))
        {
            out_.append(text);
            return *this;
        }

        out_.push_back(dialect_.quote);
        for (const char c : text)
        {
            if (c == dialect_.quote)
                out_.push_back(dialect_.quote); // a literal quote is written twice
            out_.push_back(c);
        }
        out_.push_back(dialect_.quote);
        return *this;
    }

    writer &writer::raw_field(std::string_view text)
    {
        if (!first_in_row_)
            out_.push_back(dialect_.delimiter);
        first_in_row_ = false;
        out_.append(text);
        return *this;
    }

    writer &writer::end_row()
    {
        out_.append(dialect_.newline == line_ending::crlf ? "\r\n" : "\n");
        first_in_row_ = true;
        return *this;
    }

    writer &writer::row(const csv::row &r)
    {
        for (const csv::field f : r)
            field(f.view());
        return end_row();
    }

    std::string writer::take()
    {
        first_in_row_ = true;
        return std::move(out_);
    }

    std::string dump(const table &t, const dialect &d)
    {
        writer w(d);
        // Two bytes of separator and newline per field is a fair first guess; quoting grows it.
        w.reserve(t.field_count() * 2 + 64);

        if (t.has_header())
        {
            for (const std::string &name : t.header())
                w.field(name);
            w.end_row();
        }
        for (const row r : t)
            w.row(r);

        return w.take();
    }

} // namespace catalyst::resource::csv
