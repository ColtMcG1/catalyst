#pragma once

#include "concepts.hpp"
#include "detail/chain.hpp"
#include "detail/registry.hpp"
#include "next.hpp"
#include "tag.hpp"
#include "task.hpp"
#include "token.hpp"

#include <memory>
#include <mutex>
#include <utility>

namespace catalyst::events
{

    // ------------------------------------------------------------
    // Event Bus
    // ------------------------------------------------------------
    //
    // Ordering: middleware and listeners run in descending priority; equal
    // priorities run in registration order. The highest-priority middleware is
    // the outermost layer of the chain.
    //
    // Threading: every member function, and every token operation, may be
    // called concurrently from any thread. No lock is held while a listener or
    // middleware runs, so callables are free to register or remove anything.
    // A dispatch works on a snapshot taken when it starts: registrations added
    // during it are first seen by the next dispatch, and a removal stops the
    // callable from being started again, even later in the same dispatch. An
    // invocation already running on another thread when remove() returns may
    // still be in progress; the callable object itself stays alive until every
    // in-flight dispatch that captured it has finished. Concurrent dispatches
    // may invoke the same callable concurrently, so callables that share state
    // must synchronise it themselves.
    //
    // Lifetime: tokens may outlive the bus; they simply become invalid. The bus
    // must outlive its own in-flight dispatches.

    /**
     * @class bus
     * @brief Event bus for managing listeners and middleware.
     */
    class bus
    {
    public:
        bus() = default;
        bus(const bus &) = delete;
        bus &operator=(const bus &) = delete;

        // --------------------------------------------------------
        // Listeners
        // --------------------------------------------------------

        template <typename Event, typename F>
            requires listener_for<F, Event>
        token add_listener(F &&f, int priority = 0)
        {
            return install(&detail::state::sync_listeners, detail::kind::sync_listener, event_id<Event>(), priority,
                           [fn = std::forward<F>(f)](const void *ptr) mutable
                           {
                               fn(*static_cast<const Event *>(ptr));
                           });
        }

        template <typename Event, typename F>
            requires coroutine_listener_for<F, Event>
        token add_listener(F &&f, int priority = 0)
        {
            return install(&detail::state::async_listeners, detail::kind::async_listener, event_id<Event>(), priority,
                           [fn = std::forward<F>(f)](const void *ptr) mutable -> task<void>
                           {
                               return fn(*static_cast<const Event *>(ptr));
                           });
        }

        // --------------------------------------------------------
        // Middleware
        // --------------------------------------------------------

        template <typename Event, typename F>
            requires chain_middleware_for<F, Event>
        token add_middleware(F &&f, int priority = 0)
        {
            return install(&detail::state::sync_middleware, detail::kind::sync_middleware, event_id<Event>(), priority,
                           [fn = std::forward<F>(f)](void *ev, void *ctx, void (*resume)(void *, void *)) mutable
                           {
                               fn(*static_cast<Event *>(ev), next<Event>{detail::chain_access{}, ctx, resume});
                           });
        }

        template <typename Event, typename F>
            requires middleware_for<F, Event>
        token add_middleware(F &&f, int priority = 0)
        {
            return add_middleware<Event>(
                [fn = std::forward<F>(f)](Event &e, const next<Event> &n) mutable
                {
                    if (fn(e))
                        n(e);
                },
                priority);
        }

        template <typename Event, typename F>
            requires async_chain_middleware_for<F, Event>
        token add_async_middleware(F &&f, int priority = 0)
        {
            return install(&detail::state::async_middleware, detail::kind::async_middleware, event_id<Event>(), priority,
                           // A coroutine rather than a plain forwarder so that the
                           // async_next outlives the user's coroutine, which only
                           // holds a reference to it.
                           [fn = std::forward<F>(f)](void *ev, void *ctx, task<void> (*resume)(void *, void *)) mutable -> task<void>
                           {
                               async_next<Event> n{detail::chain_access{}, ctx, resume};
                               co_await fn(*static_cast<Event *>(ev), n);
                           });
        }

        template <typename Event, typename F>
            requires async_middleware_for<F, Event>
        token add_async_middleware(F &&f, int priority = 0)
        {
            return add_async_middleware<Event>(
                [fn = std::forward<F>(f)](Event &e, const async_next<Event> &n) mutable -> task<void>
                {
                    if (co_await fn(e))
                        co_await n(e);
                },
                priority);
        }

        // --------------------------------------------------------
        // Dispatch
        // --------------------------------------------------------

        template <typename Event>
        void dispatch(Event e)
        {
            std::shared_ptr<const detail::sync_middleware_list> middleware;
            std::shared_ptr<const detail::sync_listener_list> listeners;
            {
                std::lock_guard lock(state_->mutex);
                middleware = state_->sync_middleware.snapshot(event_id<Event>());
                listeners = state_->sync_listeners.snapshot(event_id<Event>());
            }

            detail::chain_ctx ctx{middleware.get(), listeners.get(), 0};
            detail::run_chain(&ctx, &e);
        }

        template <typename Event>
        task<void> dispatch_async(Event e)
        {
            std::shared_ptr<const detail::async_middleware_list> middleware;
            std::shared_ptr<const detail::async_listener_list> listeners;
            {
                std::lock_guard lock(state_->mutex);
                middleware = state_->async_middleware.snapshot(event_id<Event>());
                listeners = state_->async_listeners.snapshot(event_id<Event>());
            }

            detail::async_chain_ctx ctx{middleware.get(), listeners.get(), 0};
            co_await detail::run_chain_async(&ctx, &e);
        }

    private:
        template <typename Fn, typename Callable>
        token install(detail::registry<Fn> detail::state::*reg,
                      detail::kind k,
                      event_key type,
                      int priority,
                      Callable &&callable)
        {
            auto s = std::make_shared<detail::slot<Fn>>(state_->next_id++, priority, std::forward<Callable>(callable));
            {
                std::lock_guard lock(state_->mutex);
                ((*state_).*reg).add(type, s);
            }
            return token{state_, s, k, type};
        }

        std::shared_ptr<detail::state> state_ = std::make_shared<detail::state>();
    };

} // namespace catalyst::events
