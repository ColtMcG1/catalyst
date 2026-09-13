/**
 * @file filter.cpp
 * @brief Implements the stock event filters declared in filter.hpp.
 * License: CDDL-1.0 (see LICENSE).
 */

#include <catalyst/logging/filter.hpp>

namespace catalyst::logging
{

    bool level_at_least::operator()(const log_event &event) const noexcept
    {
        return event.level >= level;
    }

    bool category_is::operator()(const log_event &event) const noexcept
    {
        return event.category == category;
    }

    bool category_under::operator()(const log_event &event) const noexcept
    {
        if (event.category == prefix)
            return true;
        // A dot is what separates a category from the one it lives under, so "physics" covers
        // "physics.solver" but not "physicsx".
        return event.category.size() > prefix.size() && event.category.starts_with(prefix) &&
               event.category[prefix.size()] == '.';
    }

} // namespace catalyst::logging
