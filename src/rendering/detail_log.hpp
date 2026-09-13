/**
 * SPDX-License-Identifier: MIT
 * SPDX-FileCopyrightText: 2026-Current Catalyst
 *
 * @file
 * @brief The logging category every rendering translation unit writes under. Not part of the public
 * API.
 * @details Rendering used to print diagnostics to `stderr` through a varargs `report()` that wrote
 * `"[catalyst.rendering.vulkan] "` and then whatever `vfprintf` made of the arguments. Nothing
 * could turn it off, redirect it, filter it by severity or read it back, which for the one module
 * whose failures are mostly the driver's account of what went wrong is exactly backwards.
 *
 * Everything now goes through `catalyst::logging` under the category named here, so a caller gets
 * what every other subsystem's diagnostics already gave them: a level to filter on, the sinks they
 * installed, and the call site the line came from. The backend prefix is gone because the category
 * column already carries it.
 *
 * **Which thread.** Vulkan's debug messenger calls back on whichever thread tripped the validation
 * layer, so lines can arrive from a driver thread. `logging::router` is synchronised and safe for
 * that; a `queued_sink` or `async_sink` is the right choice if a caller does not want the driver
 * thread doing formatting work.
 */

#pragma once

#include <catalyst/logging/log.hpp>

namespace catalyst::rendering::detail
{

    /**
     * @struct render_log
     * @brief Names the rendering module in the log's category column.
     * @details One category for the whole module rather than one per backend: which backend is
     * compiled in is a build-wide fact, and a caller filtering on `"rendering"` wants all of it.
     */
    struct render_log
    {
        static constexpr const char *name = "rendering";
    };

} // namespace catalyst::rendering::detail
