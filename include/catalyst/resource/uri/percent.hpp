/**
 * @file percent.hpp
 * @brief Percent-encoding: @ref catalyst::resource::percent_encode and
 * @ref catalyst::resource::percent_decode.
 * @details These are the only way bytes get into and out of a URI component intact, so they are
 * public rather than an implementation detail: composing a URI from strings means encoding each
 * piece with the @ref catalyst::resource::uri::encode_set for the place it is going, and reading one
 * back means decoding it.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <expected>
#include <string>
#include <string_view>

#include "error.hpp"

namespace catalyst::resource
{

    /**
     * @fn percent_encode
     * @brief Percent-encode every byte of @p text that is not unreserved and not in @p extra_safe.
     *
     * The unreserved set (`ALPHA DIGIT - . _ ~`) is always left literal because encoding it changes
     * nothing but the spelling. Everything else is encoded unless the caller names it in
     * @p extra_safe, which is what @ref uri::encode_set is for: a byte that separates components in
     * one place is ordinary content in another.
     *
     * Bytes `>= 0x80` are always encoded, one escape per byte, so UTF-8 text survives a round trip
     * through @ref percent_decode unchanged.
     *
     * @param text Bytes to encode.
     * @param extra_safe Additional bytes to leave literal; see @ref uri::encode_set.
     * @return A freshly allocated string containing only ASCII.
     */
    [[nodiscard]] std::string percent_encode(std::string_view text, std::string_view extra_safe = {});

    /**
     * @fn percent_decode
     * @brief Decode every `%XX` escape in @p text.
     *
     * @param text Text to decode; need not come from a @ref uri.
     * @param plus_is_space When `true`, `+` decodes to a space, the way an HTML form decoder reads a
     *        query value. Off by default, because in a path `+` is a literal plus.
     * @return The decoded bytes, or @ref uri_error_code::invalid_percent_encoding with the offset of
     *         the offending `%`.
     */
    [[nodiscard]] std::expected<std::string, uri_error> percent_decode(std::string_view text,
                                                                      bool plus_is_space = false);

} // namespace catalyst::resource
