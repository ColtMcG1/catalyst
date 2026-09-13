/**
 * @file uri.hpp
 * @brief Umbrella header for the URI module.
 * @details Including this header pulls in the whole module: the @ref catalyst::resource::uri_error
 * failure value, the RFC 3986 character classes, the @ref catalyst::resource::percent_encode /
 * @ref catalyst::resource::percent_decode helpers, the @ref catalyst::resource::uri value type, its
 * validating parser, and reference resolution and normalization. Individual headers can be included
 * instead when only part of the module is needed.
 *
 * A URI is the resource system's name for a thing: which pack a texture lives in, where a manifest
 * was loaded from, what a relative path in that manifest resolves against. That makes the two
 * interesting operations @ref catalyst::resource::uri::resolve and
 * @ref catalyst::resource::uri::normalized rather than parsing as such -- an asset manifest that
 * says `../textures/stone_d.png` is only meaningful against the URI the manifest itself came from,
 * and a texture cache is only correct if two spellings of one URI hash the same.
 *
 * Parsing is validating and reports failure as a value through `std::expected`, the same shape the
 * JSON parser uses, because a URI usually comes from a file or a user rather than from a literal.
 * A parsed `uri` owns its text and stores byte offsets into it, so every accessor is a `string_view`
 * with no allocation; only the operations that must build new text
 * (@ref catalyst::resource::uri::resolve, @ref catalyst::resource::uri::normalized,
 * @ref catalyst::resource::percent_decode) allocate.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/resource/uri/charset.hpp>
#include <catalyst/resource/uri/error.hpp>
#include <catalyst/resource/uri/parser.hpp>
#include <catalyst/resource/uri/percent.hpp>
#include <catalyst/resource/uri/reference.hpp>
#include <catalyst/resource/uri/resolver.hpp>
