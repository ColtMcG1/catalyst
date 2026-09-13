/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file stb_image_impl.cpp
 * @brief The one translation unit that stamps out stb_image's implementation.
 * @details stb_image is a single-file header: including it normally yields declarations, and
 * including it once with `STB_IMAGE_IMPLEMENTATION` defined yields the code. That once is here, in
 * a file that contains nothing else, so image_decode.cpp can include the header for its
 * declarations without every edit to the decoder recompiling ~8k lines of image codecs.
 *
 * The configuration macros that change what the header *declares* -- `STBI_NO_STDIO`,
 * `STBI_FAILURE_USERMSG` -- are deliberately not set here. They live on the `catalyst_stb`
 * interface target in cmake/stb.cmake, because this TU and every consumer must agree on them and a
 * define in one .cpp cannot guarantee that. Only the allocator hooks, which are implementation-only,
 * are set below.
 *
 * `stbi_failure_reason` is thread-local automatically: stb picks `thread_local` for any C++11 or
 * later build, and Catalyst is C++23. So a decode failing on a worker thread cannot clobber the
 * reason another thread is about to read -- which matters once Tier 2 moves decoding off the
 * owning thread.
 *
 * Warnings are disabled for this file in src/resource/CMakeLists.txt rather than by patching
 * upstream: the code is vendored verbatim at a pinned commit and should stay diff-free.
 * License: CDDL-1.0 (see LICENSE). stb itself is public domain / MIT -- see the LICENSE file in the
 * fetched stb source tree.
 */

#include <cstdlib>

// stb's default is malloc/realloc/free reached through <stdlib.h>. Naming them explicitly costs
// nothing and leaves one obvious place to route decoding at an arena or a tracking allocator, which
// is the shape the rest of the asset system is heading toward.
#define STBI_MALLOC(sz) std::malloc(sz)
#define STBI_REALLOC(p, newsz) std::realloc(p, newsz)
#define STBI_FREE(p) std::free(p)

#define STB_IMAGE_IMPLEMENTATION
#include <stb_image.h>
