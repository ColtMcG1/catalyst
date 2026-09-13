/**
 * @file windows_lean.hpp
 * @brief The single correct way to pull in <windows.h> anywhere in Catalyst.
 * @details Every Win32 translation unit needs the same two macros defined before the header is
 * parsed, and getting the guard wrong is silent: several backends used to write
 *
 *     #ifndef WIN32_LEAN_AND_MEAN
 *     #define WIN32_LEAN_AND_MEAN
 *     #include <windows.h>
 *     #endif
 *
 * which skips the include entirely whenever the macro is already defined -- by another header, by
 * a precompiled header, or by the build system. Including this header instead makes the macros and
 * the include independent, so the order in which files are included stops mattering.
 *
 * WIN32_LEAN_AND_MEAN drops the RPC, OLE, socket and shell headers that nothing here uses; a file
 * that needs one of them includes it directly afterwards. NOMINMAX suppresses the min/max function
 * macros, which otherwise break every spelled-out std::min/std::max.
 *
 * On non-Windows targets this header is empty, so it can be included unconditionally.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#if defined(_WIN32)

#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif

#  ifndef NOMINMAX
#    define NOMINMAX
#  endif

#  include <windows.h>

#endif // _WIN32
