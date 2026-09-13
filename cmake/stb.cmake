# SPDX-License-Identifier: CDDL-1.0
# SPDX-FileCopyrightText: 2026-Current Catalyst
#
# stb_image, fetched at a pinned commit and exposed as the INTERFACE target `catalyst_stb`.
#
# stb is a set of single-file headers with no build system of its own, so there is nothing to
# compile here: the target carries an include directory and the handful of configuration macros the
# implementation and the callers must agree on. The one translation unit that stamps out the
# implementation is src/resource/detail/stb_image_impl.cpp.
#
# The commit is pinned rather than tracking master because stb has no releases: a floating ref would
# mean a decoder that changes under the project without a commit in it to point at. Re-pinning is a
# deliberate edit to the SHA below.
#
# License: stb is dual-licensed public domain (Unlicense) / MIT -- see the LICENSE file in the
# fetched source tree. Catalyst itself remains CDDL-1.0.

include_guard(GLOBAL)

include(FetchContent)

set(CATALYST_STB_GIT_TAG "2c980bb59875b0d32144a71867fbdebb2f77cd20" CACHE STRING
    "Commit of nothings/stb to build against. Pinned; see cmake/stb.cmake.")
mark_as_advanced(CATALYST_STB_GIT_TAG)

FetchContent_Declare(stb
  GIT_REPOSITORY https://github.com/nothings/stb.git
  GIT_TAG        ${CATALYST_STB_GIT_TAG}

  # stb ships no CMakeLists.txt at the pinned commit, so FetchContent_MakeAvailable would not call
  # add_subdirectory anyway. Naming a subdirectory that does not exist keeps that true if a later
  # re-pin lands on a commit where upstream has added one -- we want the headers, not stb's own
  # test and tool targets.
  SOURCE_SUBDIR  cmake-intentionally-absent
)

FetchContent_MakeAvailable(stb)

add_library(catalyst_stb INTERFACE)
add_library(catalyst::stb ALIAS catalyst_stb)

# SYSTEM so stb's own warnings do not land in Catalyst's build log. It does not silence warnings in
# the implementation TU -- that one turns them off per-source in src/resource/CMakeLists.txt.
target_include_directories(catalyst_stb SYSTEM INTERFACE ${stb_SOURCE_DIR})

# These MUST be identical for the implementation TU and for every TU that includes stb_image.h,
# because they change the declarations the header emits. Keeping them on the interface target is
# what guarantees that.
target_compile_definitions(catalyst_stb
  INTERFACE
    # Every decode in this module starts from bytes already in memory -- the vfs owns file access,
    # and a decoder that could open paths of its own would route around the mount table. Dropping
    # the stdio half of the API makes that a compile error rather than a convention.
    STBI_NO_STDIO

    # Sentence-shaped failure reasons ("bad PNG signature") instead of the terse internal tokens.
    # They go straight into resource::error::detail.
    STBI_FAILURE_USERMSG
)
