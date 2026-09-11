/**
 * @file parser.hpp
 * @brief The component validators the URI parser is built out of.
 * @details The grammar is RFC 3986's, parsed in one left-to-right pass over the text: scheme, `//`
 * authority, path, `?` query, `#` fragment, each validated as it is delimited. Validation is where
 * the work is -- the split itself is a handful of `find_first_of` calls -- so the checks live in
 * `detail::validate_escaped` and `detail::validate_authority` and are shared with
 * @ref catalyst::resource::uri::from_parts, which composes text and then parses it back rather than
 * trusting its caller.
 *
 * Failure is a @ref catalyst::resource::uri_error value carrying the byte offset that was rejected,
 * which is what an error message wants to underline. The entry points themselves are members, so
 * they are declared in reference.hpp with the rest of the class and defined alongside these in
 * parser.cpp.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <cstddef>
#include <expected>
#include <string_view>

#include "charset.hpp"
#include "error.hpp"

namespace catalyst::resource
{

    namespace detail
    {
        /**
         * @fn validate_escaped
         * @brief Check that @p s contains only literal bytes accepted by @p allowed, plus well-formed
         * `%XX` escapes.
         * @param s The component text.
         * @param base Offset of @p s within the whole URI, so a reported offset is absolute.
         * @param allowed Predicate deciding whether a literal byte is acceptable here.
         * @param bad The error to report for a byte @p allowed rejects.
         * @return Nothing on success, or the first error.
         */
        template <class Predicate>
        [[nodiscard]] inline std::expected<void, uri_error> validate_escaped(std::string_view s, std::size_t base,
                                                                            Predicate allowed, uri_error_code bad)
        {
            using namespace uri_chars;

            for (std::size_t i = 0; i < s.size(); ++i)
            {
                const auto uc = static_cast<unsigned char>(s[i]);
                if (uc == '%')
                {
                    if (i + 2 >= s.size() || !is_hex(static_cast<unsigned char>(s[i + 1])) ||
                        !is_hex(static_cast<unsigned char>(s[i + 2])))
                        return std::unexpected(uri_error{uri_error_code::invalid_percent_encoding, base + i});
                    i += 2;
                }
                else if (!allowed(uc))
                {
                    return std::unexpected(uri_error{bad, base + i});
                }
            }
            return {};
        }

        /**
         * @fn validate_authority
         * @brief Check an authority's userinfo, host and port.
         * @param auth The authority text, without its leading `//`.
         * @param base Offset of @p auth within the whole URI.
         */
        [[nodiscard]] std::expected<void, uri_error> validate_authority(std::string_view auth, std::size_t base);

    } // namespace detail

} // namespace catalyst::resource
