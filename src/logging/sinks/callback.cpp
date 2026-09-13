/**
 * @file callback.cpp
 * @brief Implements `callback_sink`.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/logging/sinks/callback.hpp>

namespace catalyst::logging
{

    void callback_sink::write(const log_event &event) const
    {
        if (callback)
            callback(event);
    }

} // namespace catalyst::logging
