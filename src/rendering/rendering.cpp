/**
 * SPDX-License-Identifier: CDDL-1.0
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief Module-level entry points of the rendering library.
 */

#include <catalyst/rendering/rendering.hpp>

#include "detail_backend.hpp"

namespace catalyst::rendering
{

    const char *module_name()
    {
        return detail::backend_name();
    }

    backend_kind backend() noexcept
    {
        return detail::backend_type();
    }

} // namespace catalyst::rendering
