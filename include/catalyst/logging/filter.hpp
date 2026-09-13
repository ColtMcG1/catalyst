/**
 * @file filter.hpp
 * @brief Predicates over events, for whoever wants to ask a question about one.
 * @details Filtering is how one router feeds several audiences: the console gets everything, a log
 * panel gets info and above, and a subsystem's own view gets only its own category. A predicate
 * from here goes in either of two places, and they answer different questions.
 *
 * On a registration, it decides which sink sees the event:
 *
 *     router.add_sink(console_sink{});
 *     router.add_sink(panel, level_at_least(log_level::info));
 *     router.add_sink(tab, all_of(category_under("physics"), level_at_least(log_level::warn)));
 *
 * As middleware, it decides whether the event goes anywhere at all:
 *
 *     router.add_middleware(none_of(category_under("vulkan.verbose")));
 *
 * The same predicates serve both, because a filter takes the event by const reference and a
 * middleware predicate is allowed to. See middleware.hpp for when each is the right place.
 *
 * The three basic filters are named types rather than lambdas so that they can be stored, compared
 * against in a debugger, and implemented out of line.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/logging/event.hpp>
#include <catalyst/logging/level.hpp>

#include <concepts>
#include <string_view>

namespace catalyst::logging
{

    /**
     * @concept event_filter
     * @brief A predicate over events.
     * @tparam F The candidate filter type.
     */
    template <typename F>
    concept event_filter = std::predicate<F, const log_event &>;

    /**
     * @struct level_at_least
     * @brief Passes events at a level and above.
     */
    struct level_at_least
    {
        log_level level;

        [[nodiscard]] bool operator()(const log_event &event) const noexcept;
    };

    /**
     * @struct category_is
     * @brief Passes events whose category is exactly the one given.
     */
    struct category_is
    {
        std::string_view category;

        [[nodiscard]] bool operator()(const log_event &event) const noexcept;
    };

    /**
     * @struct category_under
     * @brief Passes events in a category or one below it.
     * @details A category is below `prefix` when it starts with `prefix` followed by a dot, so
     * "physics" matches both "physics" and "physics.solver", but not "physicsx".
     */
    struct category_under
    {
        std::string_view prefix;

        [[nodiscard]] bool operator()(const log_event &event) const noexcept;
    };

    /// Passes events that pass every filter given.
    template <event_filter... Fs>
    [[nodiscard]] auto all_of(Fs... filters)
    {
        return [=](const log_event &e) { return (filters(e) && ...); };
    }

    /// Passes events that pass any filter given.
    template <event_filter... Fs>
    [[nodiscard]] auto any_of(Fs... filters)
    {
        return [=](const log_event &e) { return (filters(e) || ...); };
    }

    /// Passes events that fail the filter given.
    template <event_filter F>
    [[nodiscard]] auto none_of(F filter)
    {
        return [=](const log_event &e) { return !filter(e); };
    }

} // namespace catalyst::logging
