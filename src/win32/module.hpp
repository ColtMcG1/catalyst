/**
 * @file module.hpp
 * @brief Typed, null-safe resolution of Win32 entry points that may not exist on the running
 * system.
 * @details Catalyst targets a range of Windows versions wider than any single import library
 * covers, so a backend that wants GetDpiForWindow, SetProcessDpiAwarenessContext or
 * AdjustWindowRectExForDpi cannot link against it -- doing so makes the executable fail to load on
 * an older system, whether or not the call is ever reached. The alternative is to look the symbol
 * up at run time and degrade when it is absent, and every backend that does so had grown its own
 * copy of the same four-step sequence: get the module, GetProcAddress, reinterpret_cast the
 * FARPROC, cache the result in a function-local static.
 *
 * These helpers collapse the first three steps and leave the caching to the call site, which is
 * where it belongs -- `static const auto fn = linked_symbol<...>(...)` reads clearly and each
 * caller keeps control of when the lookup happens.
 *
 * The distinction between the two lookups is deliberate. linked_symbol() asks only about modules
 * already mapped into the process (user32, kernel32 -- anything the loader brought in), and never
 * causes a load. loaded_symbol() maps the module if it is absent, which is required for optional
 * components like shcore.dll; the handle is intentionally never released, because the resolved
 * function pointers outlive any scope that could own it and unmapping the module would leave them
 * dangling. One extra module reference for the life of the process is the correct trade.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#if defined(_WIN32)

#  include "windows_lean.hpp"

#  include <type_traits>

namespace catalyst::detail::win32
{

    /// Returns the named module if it is already mapped into the process, or null. Never loads.
    [[nodiscard]] HMODULE linked_module(const wchar_t *name) noexcept;

    /// Returns the named module, mapping it if necessary, or null if it cannot be loaded. The
    /// reference taken is never released; see the file comment.
    [[nodiscard]] HMODULE load_module(const wchar_t *name) noexcept;

    /// GetProcAddress that tolerates a null module.
    [[nodiscard]] FARPROC symbol_address(HMODULE module, const char *symbol) noexcept;

    /// Resolves @p symbol in a module that is expected to be loaded already. Null if either the
    /// module or the symbol is missing, which is the normal answer on an older Windows.
    template <typename Fn>
    [[nodiscard]] Fn linked_symbol(const wchar_t *module, const char *symbol) noexcept
    {
        static_assert(std::is_pointer_v<Fn> && std::is_function_v<std::remove_pointer_t<Fn>>,
                      "Fn must be a function pointer type");

        return reinterpret_cast<Fn>(symbol_address(linked_module(module), symbol));
    }

    /// Resolves @p symbol, loading the module first if it is not mapped yet. Null if either the
    /// module or the symbol is missing.
    template <typename Fn>
    [[nodiscard]] Fn loaded_symbol(const wchar_t *module, const char *symbol) noexcept
    {
        static_assert(std::is_pointer_v<Fn> && std::is_function_v<std::remove_pointer_t<Fn>>,
                      "Fn must be a function pointer type");

        return reinterpret_cast<Fn>(symbol_address(load_module(module), symbol));
    }

} // namespace catalyst::detail::win32

#endif // _WIN32
