/**
 * @file module.cpp
 * @brief Module and entry-point lookup for the Win32 backends.
 * License: CDDL-1.0 (see LICENSE).
 */

#include "module.hpp"

#if defined(_WIN32)

namespace catalyst::detail::win32
{

    HMODULE linked_module(const wchar_t *name) noexcept
    {
        if (!name)
            return nullptr;

        return GetModuleHandleW(name);
    }

    HMODULE load_module(const wchar_t *name) noexcept
    {
        if (!name)
            return nullptr;

        // GetModuleHandleW first so the common case does not walk the loader's search path, and so
        // repeated calls do not accumulate references on a module that is already present.
        if (HMODULE existing = GetModuleHandleW(name))
            return existing;

        return LoadLibraryW(name);
    }

    FARPROC symbol_address(HMODULE module, const char *symbol) noexcept
    {
        if (!module || !symbol)
            return nullptr;

        return GetProcAddress(module, symbol);
    }

} // namespace catalyst::detail::win32

#endif // _WIN32
