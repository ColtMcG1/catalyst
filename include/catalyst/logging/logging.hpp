/**
 * @file logging.hpp
 * @brief Umbrella header for the catalyst::logging module.
 * @details Including this header pulls in the whole module: the levels and the event record, the
 * router, its middleware and filters, every stock sink, and the log functions themselves.
 * Individual headers can be included instead when only part of the module is needed - code that
 * only logs wants log.hpp, a sink of its own wants sink.hpp, and the place that decides where the
 * log goes wants sinks.hpp.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/logging/event.hpp>
#include <catalyst/logging/filter.hpp>
#include <catalyst/logging/format.hpp>
#include <catalyst/logging/level.hpp>
#include <catalyst/logging/log.hpp>
#include <catalyst/logging/middleware.hpp>
#include <catalyst/logging/router.hpp>
#include <catalyst/logging/sink.hpp>
#include <catalyst/logging/sinks.hpp>

/**
 * @namespace catalyst::logging
 * @brief Structured logging: one record per call, one router, and as many sinks as there are
 * audiences for it.
 * @details Code calls a level function - `info`, `warn`, `error` - optionally naming a category type
 * that identifies its subsystem. The call formats nothing unless some sink wants it. The
 * `default_logger()` router then stamps a sequence number, runs the event through whatever
 * middleware is registered, and hands it to every sink: a console, a file, a ring buffer a GUI panel
 * draws from, or a callback. A registration can carry a filter so one router serves several
 * audiences, and the router can hold individual categories at their own level while everything else
 * runs at the global minimum.
 */
namespace catalyst::logging
{

    /**
     * @fn module_name
     * @brief Returns the name of this module as a string. This can be used for logging, debugging, or any situation where you want to identify the module by name.
     * @return A string literal representing the name of this module.
     */
    const char *module_name();

} // namespace catalyst::logging

namespace catalyst
{
    /// Short name for the logging namespace, so call sites read `log::info(...)`.
    namespace log = logging;
} // namespace catalyst
