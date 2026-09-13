#pragma once

#include "next.hpp"
#include "task.hpp"

#include <concepts>

namespace catalyst::events
{

    /**
     * @concept listener_for
     * @brief Concept for a synchronous event listener.
     * @tparam F The type of the listener function.
     * @tparam Event The type of event.
     */
    template <typename F, typename Event>
    concept listener_for = std::invocable<F, const Event &>;

    /**
     * @concept coroutine_listener_for
     * @brief Concept for an asynchronous event listener that returns a task.
     * @tparam F The type of the listener function.
     * @tparam Event The type of event.
     * @note Spelled as a refinement of listener_for so that it subsumes it: a coroutine listener satisfies both,
     *       and the more constrained overload must win.
     */
    template <typename F, typename Event>
    concept coroutine_listener_for =
        listener_for<F, Event> &&
        requires(F f, const Event &e) {
            { f(e) } -> std::same_as<task<void>>;
        };

    /**
     * @concept middleware_for
     * @brief Concept for a synchronous middleware that can stop the event propagation.
     * @tparam F The type of the middleware function.
     * @tparam Event The type of event.
     * @note The middleware should return true to continue propagation, or false to stop it.
     */
    template <typename F, typename Event>
    concept middleware_for =
        requires(F f, Event &e) {
            { f(e) } -> std::same_as<bool>;
        };

    /**
     * @concept async_middleware_for
     * @brief Concept for an asynchronous middleware that can stop the event propagation.
     * @tparam F The type of the middleware function.
     * @tparam Event The type of event.
     * @note The middleware should return a task that resolves to true to continue propagation, or false to stop it.
     */
    template <typename F, typename Event>
    concept async_middleware_for =
        requires(F f, Event &e) {
            { f(e) } -> std::same_as<task<bool>>;
        };

    /**
     * @concept chain_middleware_for
     * @brief Concept for a chain-style middleware that can stop the event propagation by not calling next.
     * @tparam F The type of the middleware function.
     * @tparam Event The type of event.
     * @note The middleware should call next(e) to continue propagation, or not call it to stop.
     */
    template <typename F, typename Event>
    concept chain_middleware_for =
        requires(F f, Event &e, const next<Event> &n) {
            { f(e, n) } -> std::same_as<void>;
        };

    /**
     * @concept async_chain_middleware_for
     * @brief Concept for an asynchronous chain-style middleware that can stop the event propagation by not calling next.
     * @tparam F The type of the middleware function.
     * @tparam Event The type of event.
     * @note The middleware should call next(e) to continue propagation, or not call it to stop.
     */
    template <typename F, typename Event>
    concept async_chain_middleware_for =
        requires(F f, Event &e, const async_next<Event> &n) {
            { f(e, n) } -> std::same_as<task<void>>;
        };

} // namespace catalyst::events
