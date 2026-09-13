/**
 * @file middleware.hpp
 * @brief The layer an event passes through before any sink sees it.
 * @details Middleware is where an event is inspected, changed, or stopped - once, for the whole
 * router, rather than once per sink. It is the same idea as `catalyst::events` middleware, spelled
 * the same way, because it is the same job: a chain of callables wrapped around the delivery, each
 * free to do something before it, after it, or instead of it.
 *
 * Two forms. The short one answers whether the event should carry on:
 *
 *     router.add_middleware([](log_event &e) { return !e.message.contains(secret); });
 *
 * The long one is handed the rest of the chain and decides when, or whether, to call it - which is
 * what lets middleware do something on the way back out as well:
 *
 *     router.add_middleware([](log_event &e, const next &n)
 *     {
 *         const auto started = std::chrono::steady_clock::now();
 *         n(e);
 *         record(std::chrono::steady_clock::now() - started);
 *     });
 *
 * The event is passed by non-const reference, so middleware may rewrite it: redact a password out of
 * a message, put a frame number in the category, attach a payload the sinks downstream know how to
 * read. Everything after it in the chain, and every sink, sees what it left behind.
 *
 * Ordering is by descending priority, and registration order within a priority, so the
 * highest-priority middleware is the outermost layer - the first to see the event and the last to
 * see the chain return. A middleware that drops an event costs the sinks nothing, so a check that
 * discards a lot belongs at a high priority.
 *
 * What middleware must not do is log. It runs inside the dispatch, so an event logged from in there
 * is dropped by the same reentrancy guard that protects the sinks, and counted in
 * `router::dropped_reentrant()`.
 *
 * Middleware is not the level check. Levels are decided before an event is built at all, and this
 * runs after the event exists and its message has been formatted. Turning a whole category off
 * belongs in `router::set_category_level`, which costs nothing per call; middleware is for the
 * decisions that need to look at the event.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/logging/event.hpp>

#include <concepts>
#include <cstdint>

namespace catalyst::logging
{

    /// Identifies one registration of a middleware with a router.
    using middleware_id = std::uint64_t;

    /// The id no registration ever has; returned when a middleware could not be added.
    inline constexpr middleware_id no_middleware = 0;

    namespace detail
    {
        /**
         * @struct chain_access
         * @brief Tag that gates construction of `next`.
         * @note Only the router builds chain links; requiring this tag keeps that clear without a
         *       friend declaration that would have to reach across headers.
         */
        struct chain_access
        {
            explicit chain_access() = default;
        };
    }

    /**
     * @class next
     * @brief The rest of the chain: the middleware after this one, and the sinks behind them.
     * @details Handed to a chain-style middleware, and valid only for the length of that call. A
     * middleware that does not call it stops the event there; one that calls it twice delivers the
     * event twice.
     */
    class next
    {
    public:
        next(detail::chain_access, void *context, void (*resume)(void *, log_event &)) noexcept
            : context_(context), resume_(resume)
        {
        }

        /// Runs the rest of the chain against `event`, including whatever this middleware changed.
        void operator()(log_event &event) const { resume_(context_, event); }

    private:
        void *context_;                          ///< Where in the chain to carry on from.
        void (*resume_)(void *, log_event &);    ///< The router's chain runner.
    };

    /**
     * @concept log_middleware
     * @brief A middleware that says whether the event carries on.
     * @tparam F The candidate middleware type.
     * @details Returns true to continue to the next layer, false to stop the event here.
     */
    template <typename F>
    concept log_middleware = requires(F f, log_event &event) {
        { f(event) } -> std::same_as<bool>;
    };

    /**
     * @concept chain_middleware
     * @brief A middleware handed the rest of the chain, which it calls when it chooses.
     * @tparam F The candidate middleware type.
     * @details Not calling `next` stops the event, the same as returning false from the short form.
     * The reason to write this one is to do something after the chain returns as well as before it.
     */
    template <typename F>
    concept chain_middleware = requires(F f, log_event &event, const next &rest) {
        { f(event, rest) } -> std::same_as<void>;
    };

    class router;

    /**
     * @class scoped_middleware
     * @brief Removes a middleware from its router when destroyed.
     * @details The counterpart of `scoped_sink`, and there for the same reason: a middleware that
     * captures something with a lifetime - a frame counter, a capture buffer, a subsystem - must
     * stop being reachable before that thing goes away.
     *
     * Unlike `scoped_sink`, destruction does not wait for a dispatch already inside the middleware
     * to finish. It cannot: the callable is owned by the router and stays alive as long as any
     * dispatch that started with it is still running. What removal does promise is that no dispatch
     * will start it again. See `router::remove_middleware`.
     *
     * Built by `router::add_scoped_middleware`, or from an id directly. The router must outlive the
     * guard.
     */
    class scoped_middleware
    {
    public:
        scoped_middleware() = default;

        /// Takes over an id already returned by `add_middleware`.
        scoped_middleware(router &owner, middleware_id id) noexcept : router_(&owner), id_(id) {}

        scoped_middleware(const scoped_middleware &) = delete;
        scoped_middleware &operator=(const scoped_middleware &) = delete;
        scoped_middleware(scoped_middleware &&other) noexcept;
        scoped_middleware &operator=(scoped_middleware &&other) noexcept;
        ~scoped_middleware();

        /// Removes the middleware now, if it has not been removed already.
        void reset() noexcept;

        /// Gives up the registration without removing it; the caller takes the id back.
        [[nodiscard]] middleware_id release() noexcept;

        [[nodiscard]] middleware_id id() const noexcept { return id_; }
        [[nodiscard]] explicit operator bool() const noexcept { return id_ != no_middleware; }

    private:
        router *router_ = nullptr;
        middleware_id id_ = no_middleware;
    };

} // namespace catalyst::logging
