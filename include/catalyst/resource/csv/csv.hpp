/**
 * @file csv.hpp
 * @brief Umbrella header for the catalyst::resource::csv module.
 * @details Including this header pulls in the whole CSV module: the @ref dialect that describes a
 * flavour, the owning @ref table with its @ref row and @ref field handles, the @ref parse_table entry
 * point, and the @ref writer and @ref dump serializers. Individual headers can be included instead
 * when only part of the module is needed.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/resource/csv/dialect.hpp>
#include <catalyst/resource/csv/error.hpp>
#include <catalyst/resource/csv/parser.hpp>
#include <catalyst/resource/csv/serializer.hpp>
#include <catalyst/resource/csv/table.hpp>
#include <catalyst/resource/csv/tape.hpp>

/**
 * @namespace catalyst::resource::csv
 * @brief Delimiter-separated text: parsing to a flat table, and writing one back out.
 * @details `parse_table` reads a whole file into an owning `table`: one string arena holding every
 * decoded field, plus two offset vectors that index it by field and by record. That is three
 * allocations for the file and none per cell, and it makes both random access and a ragged row cost
 * nothing extra. A `row` and a `field` are non-owning handles into it, so walking a table copies no
 * text; a `field` is the place the typed accessors live, in the same two flavours the JSON module
 * uses -- `as_*` throws `type_error` on a mismatch, `try_*` returns an empty `std::optional`.
 *
 * CSV is a family rather than a format, so what varies lives in one `dialect` aggregate that both
 * the parser and the writer take: delimiter, quote, whether the first record is a header, whether to
 * trim, whether rows may be ragged. Reading and writing with the same dialect round-trips the file.
 *
 * The parser is deliberately strict about quoting -- a bare quote inside an unquoted field is a
 * `parse_error`, not content -- and deliberately lax about everything else: CRLF, LF and a lone CR
 * all end a record, and the last record need not be terminated. Malformed input is reported as a
 * `parse_error` value through `std::expected`, never thrown, and carries the record and field number
 * as well as the byte offset, because that is what someone staring at a spreadsheet can act on.
 *
 * The scanning underneath is `catalyst::text::scan`, with the stop set that module's documentation
 * predicts for CSV: the delimiter, the quote, CR and LF.
 */
namespace catalyst::resource::csv
{
}
