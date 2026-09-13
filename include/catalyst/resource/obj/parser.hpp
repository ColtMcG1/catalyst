/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file parser.hpp
 * @brief The Wavefront OBJ parser: @ref catalyst::resource::obj::parser::parse.
 * @details OBJ is line-oriented and whitespace-delimited, so the parse is two splits and a pile of
 * `std::from_chars`: split the buffer on newlines, take the first token of each line as the keyword,
 * and hand the rest to the routine for that keyword.
 *
 * Lines come off `std::views::split`, which suits a single-byte delimiter that never appears inside
 * a record. Fields do not: OBJ separates them with *runs* of spaces and tabs, and `views::split`
 * would hand back an empty subrange for each repeat, so the field scanner is a `find_first_not_of` /
 * `find_first_of` pair instead. Neither copies -- every token is a `string_view` into the caller's
 * buffer, and the only allocation in a successful parse is the vectors growing.
 *
 * Failures are values, as everywhere else in `catalyst::resource`. The parse stops at the first line
 * it cannot read rather than guessing, and reports @ref error_code::decode_failed with the line
 * number and the offending token in @ref error::detail -- a malformed OBJ is nearly always an
 * exporter bug or a truncated download, and both are things someone has to go and look at.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/resource/error.hpp>

#include <expected>
#include <string_view>

#include "obj.hpp"

namespace catalyst::resource::obj
{

    /**
     * @class parser
     * @brief Turns the bytes of an OBJ file into an @ref obj.
     * @details Stateless: the whole parse is @ref parse, and the per-record routines are file-local
     * to the implementation rather than private members, so adding support for a new keyword does
     * not touch this header.
     */
    class parser
    {
    public:
        /**
         * @fn parse
         * @brief Parse an entire OBJ file.
         * @param data The file's bytes. Not retained -- every `string_view` in the result's error,
         * and every token examined during the parse, is copied or consumed before returning.
         * @param uri The asset name to put in a failure, for the log. Purely diagnostic.
         * @return The parsed geometry, or the first line that could not be read.
         *
         * @details What is accepted beyond the strict grammar, because real files do it:
         * - CRLF as well as LF, and a final line with no terminator at all.
         * - A `#` comment anywhere on a line; everything from it to the end of the line is dropped.
         * - Tabs as separators, and any amount of leading or trailing whitespace.
         * - A leading `+` on a number, which `std::from_chars` alone rejects.
         * - Extra trailing components: `v x y z w` and the `v x y z r g b` vertex-colour extension
         *   both parse, keeping x, y and z.
         * - Any keyword this parser does not implement -- `o`, `g`, `s`, `mtllib`, `usemtl`, `l`,
         *   `p`, the curve and surface records -- is skipped without inspecting its operands. An
         *   unknown keyword is not an error; an unreadable `v`, `vn`, `vt` or `f` is.
         *
         * What is rejected: a vertex record with fewer components than it needs, a face with fewer
         * than three corners, a token that is not a number, and an index -- 1-based positive or
         * negative-from-the-end -- that names an element the file has not declared yet. Index 0 is
         * rejected too; OBJ has no such index.
         *
         * @note Line continuations (a trailing `\`) are not joined. They are legal OBJ and vanishing
         * rare, and a file that uses one fails here with a clear line number rather than silently
         * losing the continued half.
         */
        [[nodiscard]] static std::expected<obj, error> parse(std::string_view data,
                                                             std::string_view uri = {});
    };

} // namespace catalyst::resource::obj
