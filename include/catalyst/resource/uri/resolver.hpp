/**
 * @file resolver.hpp
 * @brief The RFC 3986 section 5 and 6 algorithms behind @ref catalyst::resource::uri::resolve and
 * @ref catalyst::resource::uri::normalized.
 * @details These are the two operations a resource system actually needs. An asset manifest that
 * says `../textures/stone_d.png` is only meaningful against the URI the manifest itself came from,
 * which is `resolve`; a texture cache is only correct if two spellings of one URI hash the same,
 * which is `normalized`. Both end by recomposing through @ref catalyst::resource::uri::from_parts,
 * so neither can produce a `uri` whose own text would not reparse. Both are members, so they are
 * declared in reference.hpp with the rest of the class and defined alongside these in resolver.cpp.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <string>
#include <string_view>

#include "reference.hpp"

namespace catalyst::resource
{

    namespace detail
    {
        /**
         * @fn remove_dot_segments
         * @brief RFC 3986 section 5.2.4: interpret `.` and `..` and drop them from @p path.
         *
         * Written as the section's output-buffer algorithm rather than as a segment stack, because
         * the trailing-slash cases (`a/b/..` becomes `a/`, not `a`) fall out of it for free and are
         * the part a hand-rolled version gets wrong.
         */
        [[nodiscard]] std::string remove_dot_segments(std::string_view path);

        /// @brief RFC 3986 section 5.2.3: splice a relative path onto the base's path.
        [[nodiscard]] std::string merge_paths(const uri &base, std::string_view ref_path);

        /**
         * @fn normalize_escapes
         * @brief Uppercase every `%XX` and decode the ones that escape an unreserved byte.
         *
         * Both halves are required by RFC 3986 section 6.2.2.2 and both are safe: `%7E` and `~` name
         * the same byte in every component, and hex case never carries meaning.
         */
        [[nodiscard]] std::string normalize_escapes(std::string_view s);

    } // namespace detail

} // namespace catalyst::resource
