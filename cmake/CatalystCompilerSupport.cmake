# Catalyst is written against four C++23 features that only very recent toolchains implement:
# deducing this (P0847), std::expected (P0323), std::forward_like (P2445) and
# std::move_only_function (P0288). A toolchain missing any of them does not fail with one clear
# diagnostic -- it fails several minutes into the build with a few hundred lines of template errors
# out of <catalyst/math/vector.hpp> or <catalyst/events/detail/registry.hpp>. These checks turn that
# into a sentence at configure time.
#
# They probe features rather than compare version numbers, because no version number answers the
# question on its own -- the standard library decides as much as the compiler does, and the two are
# chosen separately:
#
#   - Clang 18 implements deducing this, but against libstdc++ 13 (the default pairing on Ubuntu
#     24.04) it cannot see <expected> or std::forward_like. Against libstdc++ 14 it can.
#   - libc++ through 18 has no std::move_only_function at all, so -stdlib=libc++ does not work
#     here however new the Clang in front of it is.
#
# So each check compiles the real construct against the standard library this build will link.

include(CheckCXXSourceCompiles)

function(catalyst_require_cxx23_support)
  # try_compile inherits CMAKE_CXX_STANDARD through CMP0067 (NEW at our cmake_minimum_required), so
  # these compile with the same -std= flag the library does.
  set(CMAKE_REQUIRED_QUIET ON)

  check_cxx_source_compiles("
    struct probe
    {
        int value;
        constexpr int get(this auto &&self) { return self.value; }
    };
    int main() { return probe{0}.get(); }
  " CATALYST_HAS_DEDUCING_THIS)

  check_cxx_source_compiles("
    #include <expected>
    int main()
    {
        std::expected<int, int> ok{0};
        return ok.has_value() ? *ok : 1;
    }
  " CATALYST_HAS_STD_EXPECTED)

  check_cxx_source_compiles("
    #include <utility>
    int main()
    {
        int value = 0;
        return std::forward_like<int &&>(value);
    }
  " CATALYST_HAS_STD_FORWARD_LIKE)

  check_cxx_source_compiles("
    #include <functional>
    int main()
    {
        std::move_only_function<int()> fn = [] { return 0; };
        return fn();
    }
  " CATALYST_HAS_STD_MOVE_ONLY_FUNCTION)

  set(_missing "")

  if(NOT CATALYST_HAS_DEDUCING_THIS)
    list(APPEND _missing
      "  - deducing this (P0847)          used by catalyst::math and catalyst::resource::json")
  endif()

  if(NOT CATALYST_HAS_STD_EXPECTED)
    list(APPEND _missing
      "  - std::expected (P0323)          the return type of every fallible Catalyst call")
  endif()

  if(NOT CATALYST_HAS_STD_FORWARD_LIKE)
    list(APPEND _missing
      "  - std::forward_like (P2445)      used by catalyst::math accessors")
  endif()

  if(NOT CATALYST_HAS_STD_MOVE_ONLY_FUNCTION)
    list(APPEND _missing
      "  - std::move_only_function        the listener and middleware storage in catalyst::events")
  endif()

  if(_missing STREQUAL "")
    return()
  endif()

  list(JOIN _missing "\n" _missing_text)

  message(FATAL_ERROR
    "Catalyst needs a C++23 toolchain, and this one is missing:\n"
    "${_missing_text}\n\n"
    "Detected: ${CMAKE_CXX_COMPILER_ID} ${CMAKE_CXX_COMPILER_VERSION} "
    "(${CMAKE_CXX_COMPILER})\n\n"
    "Known-good toolchains:\n"
    "  - GCC 14 or newer\n"
    "  - MSVC 19.40 (Visual Studio 2022 17.10) or newer\n\n"
    "Clang cannot build Catalyst today, with either standard library, and the version does not\n"
    "help -- 18, 19 and 20 all fail the same way:\n"
    "  - against libstdc++, std::forward_like does not compile ('function with deduced return\n"
    "    type cannot be used before it is defined')\n"
    "  - against libc++ (-stdlib=libc++), there is no std::move_only_function\n"
    "Both uses are contained -- forward_like to catalyst::math, move_only_function to\n"
    "catalyst::events -- so this is fixable in Catalyst rather than a wait on the toolchain.\n\n"
    "On Ubuntu 24.04 the default g++ is 13 and will not work: install g++-14 and configure with\n"
    "  cmake -S . -B build -DCMAKE_CXX_COMPILER=g++-14\n")
endfunction()
