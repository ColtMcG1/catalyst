/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file parser.cpp
 * @brief Implements the OBJ parser declared in parser.hpp.
 * @details The per-record routines return `std::expected<void, std::string>` -- just the sentence
 * describing what was wrong. The line number and the asset name are the loop's business, not
 * theirs, so they are stamped on in one place in @ref parser::parse and every message reads the
 * same way.
 * License: MIT (see LICENSE).
 */

#include <catalyst/resource/obj/parser.hpp>

#include <catalyst/resource/error.hpp>
#include <catalyst/resource/obj/obj.hpp>
#include <catalyst/text/scan.hpp>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <format>
#include <ranges>
#include <string>
#include <string_view>
#include <system_error>

namespace catalyst::resource::obj
{
    namespace
    {
        /// @brief Whether @p c separates two OBJ fields. CR is in the set so a CRLF file needs no
        /// pre-pass -- the trailing CR is simply the separator that ends the last token on the line.
        [[nodiscard]] constexpr bool is_separator(unsigned char c) noexcept
        {
            return c == ' ' || c == '\t' || c == '\r';
        }

        /**
         * @fn next_token
         * @brief Take the next whitespace-delimited token off the front of @p rest.
         *
         * Advances @p rest to the separator that ended the token; returns an empty view, and empties
         * @p rest, once the line is exhausted. A token is never empty, so "empty" unambiguously
         * means "no more fields" -- which is what lets the record routines loop on it without a
         * separate count.
         *
         * This is deliberately not `std::views::split`. OBJ pads its columns, and splitting on a
         * single space yields an empty subrange for every repeat; collapsing runs is the whole job
         * here, and two scans do it with no filtering.
         *
         * The two scans are deliberately asymmetric, which `bench/obj/obj.cpp` measures directly.
         * The token body is scanned eight bytes at a time by @ref catalyst::text::scan::swar, the
         * same primitive the CSV parser uses: an OBJ token is a coordinate or an index run, long
         * enough that batching pays. The leading separator run is left byte-at-a-time for the reason
         * `scan::skip_ws` gives -- between two columns it is almost always exactly one byte, and a
         * SWAR word's fixed setup never amortizes over a run that short.
         *
         * Both replaced a `find_first_not_of` / `find_first_of` pair. The library calls take the stop
         * set as a runtime-length range and rescan it per byte; spelling the three bytes out is worth
         * about 3x on the tokenizer alone, and the SWAR body scan another ~15% on top of that.
         */
        [[nodiscard]] std::string_view next_token(std::string_view &rest) noexcept
        {
            const std::size_t n = rest.size();

            std::size_t start = 0;
            while (start < n && is_separator(static_cast<unsigned char>(rest[start])))
                ++start;
            if (start == n)
            {
                rest = {};
                return {};
            }

            const std::size_t stop =
                text::scan::swar<text::scan::control_bytes::allowed, ' ', '\t', '\r'>(rest, start);

            const std::string_view token = rest.substr(start, stop - start);
            rest = (stop == n) ? std::string_view{} : rest.substr(stop);
            return token;
        }

        /**
         * @fn from_text
         * @brief `std::from_chars` over a whole token: the entire view must be consumed.
         *
         * A partial match is a failure here. `1.0.0` parsing as `1.0` and leaving `.0` behind is how
         * a corrupt file turns into a mesh that is subtly wrong instead of an error, so the trailing
         * bytes are checked.
         *
         * The one relaxation is a leading `+`, which `from_chars` rejects by design and a handful of
         * exporters emit anyway.
         */
        template <typename T>
        [[nodiscard]] bool from_text(std::string_view text, T &out) noexcept
        {
            if (text.starts_with('+'))
                text.remove_prefix(1);

            const char *const begin = text.data();
            const char *const end = begin + text.size();
            const auto [ptr, ec] = std::from_chars(begin, end, out);
            return ec == std::errc() && ptr == end;
        }

        /**
         * @fn read_floats
         * @brief Read up to @p Max floats from @p rest, requiring at least @p minimum of them.
         *
         * Stops at the first of: @p Max values read, or the line running out. Any token that is not
         * a number fails, including one past @p minimum -- a record whose tail is garbage is a
         * record to complain about, even where the tail is not stored.
         *
         * @return How many were read, or the message explaining why none of it counts.
         */
        template <std::size_t Max>
        [[nodiscard]] std::expected<std::size_t, std::string> read_floats(std::string_view rest,
                                                                         std::size_t minimum,
                                                                         float (&out)[Max])
        {
            std::size_t count = 0;
            while (count < Max)
            {
                const std::string_view token = next_token(rest);
                if (token.empty())
                    break;

                if (!from_text(token, out[count]))
                    return std::unexpected(std::format("'{}' is not a number", token));
                ++count;
            }

            if (count < minimum)
                return std::unexpected(
                    std::format("expected at least {} number{}, found {}", minimum,
                                minimum == 1 ? "" : "s", count));

            return count;
        }

        /**
         * @fn resolve_index
         * @brief Turn one OBJ index into a 0-based offset into an array of @p count elements.
         *
         * OBJ numbers from 1, and a negative index counts back from the most recently declared
         * element: -1 is the last one. Both are resolved against the counts *so far*, which is the
         * reason faces cannot be deferred to a second pass -- a relative index means something
         * different depending on where in the file it appears.
         *
         * @return False if @p raw is 0, or names an element that has not been declared.
         */
        [[nodiscard]] bool resolve_index(std::int64_t raw, std::size_t count,
                                         std::int32_t &out) noexcept
        {
            const auto total = static_cast<std::int64_t>(count);

            if (raw > 0 && raw <= total)
            {
                out = static_cast<std::int32_t>(raw - 1);
                return true;
            }
            if (raw < 0 && -raw <= total)
            {
                out = static_cast<std::int32_t>(total + raw);
                return true;
            }
            return false; // 0, or out of range.
        }

        /**
         * @fn parse_corner
         * @brief Parse one `f` operand: `v`, `v/vt`, `v//vn` or `v/vt/vn`.
         *
         * The slashes are positional, so an empty middle field is meaningful -- `1//4` is a position
         * and a normal with no texture coordinate -- while an empty first field is not.
         */
        [[nodiscard]] std::expected<face_vertex, std::string> parse_corner(std::string_view token,
                                                                          const obj &out)
        {
            std::string_view parts[3];
            std::size_t count = 0;
            std::string_view rest = token;

            for (;;)
            {
                if (count == 3)
                    return std::unexpected(std::format("'{}' has more than three fields", token));

                const auto slash = rest.find('/');
                if (slash == std::string_view::npos)
                {
                    parts[count++] = rest;
                    break;
                }
                parts[count++] = rest.substr(0, slash);
                rest.remove_prefix(slash + 1);
            }

            // Position, texture coordinate, normal -- in the order the slashes put them, against the
            // array each one indexes.
            const std::size_t sizes[3] = {out.vertices.size(), out.texcoords.size(),
                                          out.normals.size()};
            const std::string_view names[3] = {"vertex", "texture coordinate", "normal"};

            face_vertex corner;
            std::int32_t *const fields[3] = {&corner.position, &corner.texcoord, &corner.normal};

            for (std::size_t i = 0; i < count; ++i)
            {
                if (parts[i].empty())
                {
                    // Only the position is mandatory; the other two are absent by default.
                    if (i == 0)
                        return std::unexpected(
                            std::format("'{}' has no vertex index", token));
                    continue;
                }

                std::int64_t raw = 0;
                if (!from_text(parts[i], raw))
                    return std::unexpected(std::format("'{}' is not an index", parts[i]));

                if (!resolve_index(raw, sizes[i], *fields[i]))
                    return std::unexpected(std::format("{} index {} is out of range; {} declared so far",
                                                       names[i], raw, sizes[i]));
            }

            return corner;
        }

        /// @brief `v x y z [w]` -- or `v x y z r g b`, which some exporters write. Keeps x, y, z.
        [[nodiscard]] std::expected<void, std::string> parse_vertex(std::string_view rest, obj &out)
        {
            float values[6]{};
            const auto count = read_floats(rest, 3, values);
            if (!count)
                return std::unexpected(count.error());

            out.vertices.push_back(math::vec3f{values[0], values[1], values[2]});
            return {};
        }

        /// @brief `vn i j k`. Not normalized here -- the file is taken at its word.
        [[nodiscard]] std::expected<void, std::string> parse_normal(std::string_view rest, obj &out)
        {
            float values[3]{};
            const auto count = read_floats(rest, 3, values);
            if (!count)
                return std::unexpected(count.error());

            out.normals.push_back(math::vec3f{values[0], values[1], values[2]});
            return {};
        }

        /// @brief `vt u [v] [w]`. A 1D texture omits `v`, and `w` is not represented.
        [[nodiscard]] std::expected<void, std::string> parse_texcoord(std::string_view rest, obj &out)
        {
            float values[3]{};
            const auto count = read_floats(rest, 1, values);
            if (!count)
                return std::unexpected(count.error());

            out.texcoords.push_back(math::vec2f{values[0], values[1]});
            return {};
        }

        /**
         * @fn parse_face
         * @brief `f` plus three or more corners.
         *
         * Appends to the tape only once the whole face has parsed, so a failure leaves @p out with
         * no half-written face in it. The offsets array is seeded with its leading 0 on first use,
         * which is what keeps the sentinel invariant true without a constructor.
         */
        [[nodiscard]] std::expected<void, std::string> parse_face(std::string_view rest, obj &out)
        {
            const std::size_t first = out.face_vertices.size();

            for (;;)
            {
                const std::string_view token = next_token(rest);
                if (token.empty())
                    break;

                auto corner = parse_corner(token, out);
                if (!corner)
                {
                    out.face_vertices.resize(first);
                    return std::unexpected(std::move(corner.error()));
                }
                out.face_vertices.push_back(*corner);
            }

            const std::size_t corners = out.face_vertices.size() - first;
            if (corners < 3)
            {
                out.face_vertices.resize(first);
                return std::unexpected(std::format("a face needs at least 3 corners, found {}", corners));
            }

            if (out.face_offsets.empty())
                out.face_offsets.push_back(0);
            out.face_offsets.push_back(static_cast<std::uint32_t>(out.face_vertices.size()));

            return {};
        }

    } // namespace

    std::expected<obj, error> parser::parse(std::string_view data, std::string_view uri)
    {
        obj result;
        std::size_t line_number = 0;

        // A single-byte delimiter that cannot occur inside a record is exactly what views::split is
        // for. The subranges are contiguous, so each one becomes a string_view over the caller's
        // bytes with no copy.
        for (const auto chunk : std::views::split(data, '\n'))
        {
            ++line_number;

            std::string_view line(chunk.begin(), chunk.end());

            // Everything from a '#' is a comment. Safe to do before dispatch because no keyword this
            // parser implements takes an operand that could contain one.
            if (const auto hash = line.find('#'); hash != std::string_view::npos)
                line = line.substr(0, hash);

            const std::string_view keyword = next_token(line);
            if (keyword.empty())
                continue; // Blank, whitespace-only, or comment-only.

            std::expected<void, std::string> outcome;
            if (keyword == "v")
                outcome = parse_vertex(line, result);
            else if (keyword == "vn")
                outcome = parse_normal(line, result);
            else if (keyword == "vt")
                outcome = parse_texcoord(line, result);
            else if (keyword == "f")
                outcome = parse_face(line, result);
            else
                continue; // A record this parser does not implement. Not an error.

            if (!outcome)
                return std::unexpected(make_error(
                    error_code::decode_failed, uri,
                    std::format("line {}: {}: {}", line_number, keyword, outcome.error())));
        }

        return result;
    }

} // namespace catalyst::resource::obj
