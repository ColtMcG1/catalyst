/**
 * @file text.hpp
 * @brief Umbrella header for the catalyst::text module.
 * @details Including this header pulls in the whole module: the @ref catalyst::text::utf8 codec and
 * surrogate helpers, and the @ref catalyst::text::scan byte scanners. Individual headers can be
 * included instead when only part of the module is needed.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/text/scan.hpp>
#include <catalyst/text/utf8.hpp>

/**
 * @namespace catalyst::text
 * @brief Low-level text primitives shared by every module that reads, writes or transcodes text.
 * @details This module exists because the same handful of routines kept being written once per
 * consumer: UTF-8 encoding in the JSON parser, in the Win32 window backend and in an example; the
 * SWAR stop-byte scan in the JSON parser, and needed again, byte-for-byte, by anything else with a
 * delimited grammar. They are small enough that copying one is easy and reviewing the copy is hard,
 * which is exactly the profile of code that should be written once and tested hard.
 *
 * What is here is deliberately narrow: primitives that operate on bytes and code points, with no
 * notion of any particular file format. A format's grammar, error vocabulary and document
 * representation stay in that format's own module -- JSON's tape encodes a tree and CSV's a
 * rectangle, and pretending otherwise would cost more than the duplication it removed.
 *
 * The module is header-only and depends on nothing but the standard library, so there is no target
 * to link and no build option to enable; every module already has `include/` on its search path.
 */
namespace catalyst::text
{
}
