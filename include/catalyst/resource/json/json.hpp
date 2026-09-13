/**
 * @file json.hpp
 * @brief Umbrella header for the catalyst::resource::json module.
 * @details Including this header pulls in the whole JSON module: the owning @ref value tree, the flat
 * @ref document with its @ref cursor, the parser entry points @ref parse and @ref parse_document, and the
 * @ref dump serializers. Individual headers can be included instead when only part of the module is
 * needed.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/resource/json/document.hpp>
#include <catalyst/resource/json/error.hpp>
#include <catalyst/resource/json/parser.hpp>
#include <catalyst/resource/json/serializer.hpp>
#include <catalyst/resource/json/tape.hpp>
#include <catalyst/resource/json/value.hpp>

/**
 * @namespace catalyst::resource::json
 * @brief JSON parsing and serialization with two document representations.
 * @details `parse` builds an owning `value` tree: a `std::variant`-backed node per value, with array
 * and insertion-ordered object containers, convenient to hold, edit and build up in code. `parse_document`
 * builds a `document`: the same tree flattened into one tape of 64-bit words plus one string arena, read
 * through a non-owning `cursor`. It allocates nothing per node and is the right choice for parse-once,
 * read-once workloads such as loading an asset manifest. Both share a single parser and a single
 * serializer, so they accept and produce identical text; `to_value` bridges from a cursor to a `value`.
 *
 * Malformed input is reported as a `parse_error` value through `std::expected`, never thrown. Typed
 * accessors come in two flavours: `as_*` throw `type_error` on a mismatch, `try_*` return an empty
 * `std::optional` (or a null pointer for containers). Lookups by key (`find`, `contains`) never throw;
 * `at` and `operator[]` on a const value are checked and throw like `std::vector::at`.
 */
namespace catalyst::resource::json
{
}
