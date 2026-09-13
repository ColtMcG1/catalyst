/**
 * @file callback.hpp
 * @brief A `std::function` as a sink.
 * @details The sink for the cases not worth a type: a test collecting events into a vector, a tool
 * counting warnings, a one-off bridge into somebody else's logging.
 *
 *     std::vector<log_event> seen;
 *     router.add_sink(callback_sink{[&](const log_event &e) { seen.push_back(e); }});
 *
 * Anything that is going to be around for the life of the program, and called on every event, is
 * better off as a real sink type: it can then declare `reentrant` honestly and say what it touches.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/logging/event.hpp>
#include <catalyst/logging/sink.hpp>

#include <functional>
#include <utility>

namespace catalyst::logging
{

    /**
     * @struct callback_sink
     * @brief A `std::function` as a sink.
     * @details Declared `serialized`, alone among the stock sinks. The others know what they touch;
     * this one is a hole in the module through which arbitrary code is reached, and the lambda
     * written at the call site to collect events into a vector is not going to have a lock in it.
     * The router holds one instead, so the obvious thing to write is also the correct one.
     *
     * A callback that genuinely is safe to run several times over, and is hot enough to care, wants
     * its own sink type declaring `reentrant` rather than this.
     */
    struct callback_sink : sink_contract<sink_threading::serialized>
    {
        std::function<void(const log_event &)> callback;

        callback_sink() = default;

        /// Named rather than aggregate-initialised, so `callback_sink{fn}` means what it looks like:
        /// the contract base is empty, and an aggregate would want a `{}` for it first.
        explicit callback_sink(std::function<void(const log_event &)> callback)
            : callback(std::move(callback))
        {
        }

        void write(const log_event &event) const;
    };

} // namespace catalyst::logging
