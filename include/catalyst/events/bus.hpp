#pragma once

#include "tag.hpp"
#include "task.hpp"

#include <atomic>
#include <cstddef>
#include <functional>
#include <concepts>
#include <mutex>
#include <utility>

namespace catalyst::events
{

    class bus;

    /**
     * @class next
     * @brief Represents the next step in the event chain for synchronous events.
     * @tparam Event The type of event.
     */
    template <typename Event>
    class next
    {
    public:
        /**
         * @fn operator()
         * @brief Invokes the next step in the event chain with the given event.
         * @param e The event to pass to the next step.
         */
        void operator()(Event &e) const { resume_(ctx_, &e); }

    private:
        friend class bus;

        next(void *ctx, void (*resume)(void *, void *)) noexcept : ctx_(ctx), resume_(resume) {}

        void *ctx_;                      ///< The context pointer for the next step.
        void (*resume_)(void *, void *); ///< The function pointer to resume the next step.
    };

    /**
     * @class async_next
     * @brief Represents the next step in the event chain for asynchronous events.
     * @tparam Event The type of event.
     */
    template <typename Event>
    class async_next
    {
    public:
        /**
         * @fn operator()
         * @brief Invokes the next step in the event chain with the given event.
         * @param e The event to pass to the next step.
         */
        task<void> operator()(Event &e) const { return resume_(ctx_, &e); }

    private:
        friend class bus;

        async_next(void *ctx, task<void> (*resume)(void *, void *)) noexcept : ctx_(ctx), resume_(resume) {}

        void *ctx_;                            ///< The context pointer for the next step.
        task<void> (*resume_)(void *, void *); ///< The function pointer to resume the next step.
    };

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
    private:
        enum class kind
        {
            sync_listener,
            async_listener,
            sync_middleware,
            async_middleware
        };

        using sync_listener_fn = std::move_only_function<void(const void *)>;
        using async_listener_fn = std::move_only_function<task<void>(const void *)>;
        using sync_middleware_fn = std::move_only_function<void(void *, void *, void (*)(void *, void *))>;
        using async_middleware_fn = std::move_only_function<task<void>(void *, void *, task<void> (*)(void *, void *))>;

        struct slot_base
        {
            std::size_t id;
            int priority;
            std::atomic<bool> active{true};
        };

        template <typename Fn>
        struct slot : slot_base
        {
            Fn fn;

            slot(std::size_t id, int priority, Fn fn) : slot_base{id, priority}, fn(std::move(fn)) {}
        };

        // One table per callable kind, keyed by event type. Lists are immutable
        // once published: a change builds a new list and swaps the pointer, so a
        // dispatch holding the old one is never invalidated. All members assume
        // the owning state's mutex is held.
        template <typename Fn>
        struct registry
        {
            using slot_type = slot<Fn>;
            using list = std::vector<std::shared_ptr<slot_type>>;

            std::unordered_map<event_key, std::shared_ptr<const list>> live;

            std::shared_ptr<const list> snapshot(event_key type) const
            {
                auto it = live.find(type);
                return it == live.end() ? nullptr : it->second;
            }

            // Stable insert: after every entry of equal or higher priority.
            void add(event_key type, std::shared_ptr<slot_type> s)
            {
                auto &current = live[type];
                auto updated = current ? std::make_shared<list>(*current) : std::make_shared<list>();
                auto pos = std::ranges::find_if(*updated, [&](const auto &x)
                                                { return x->priority < s->priority; });
                updated->insert(pos, std::move(s));
                current = std::move(updated);
            }

            void remove(event_key type, std::size_t id)
            {
                auto it = live.find(type);
                if (it == live.end())
                    return;

                const list &current = *it->second;
                auto pos = std::ranges::find(current, id, [](const auto &x)
                                             { return x->id; });
                if (pos == current.end())
                    return;

                (*pos)->active.store(false, std::memory_order_release);

                if (current.size() == 1)
                {
                    live.erase(it);
                    return;
                }

                auto updated = std::make_shared<list>(current);
                updated->erase(updated->begin() + (pos - current.begin()));
                it->second = std::move(updated);
            }
        };

        // Shared between the bus and its tokens so a token can tell whether the
        // bus still exists.
        struct state
        {
            std::mutex mutex;
            std::atomic<std::size_t> next_id{0};

            registry<sync_listener_fn> sync_listeners;
            registry<async_listener_fn> async_listeners;
            registry<sync_middleware_fn> sync_middleware;
            registry<async_middleware_fn> async_middleware;

            void remove(kind k, event_key type, std::size_t id)
            {
                switch (k)
                {
                case kind::sync_listener:
                    sync_listeners.remove(type, id);
                    break;
                case kind::async_listener:
                    async_listeners.remove(type, id);
                    break;
                case kind::sync_middleware:
                    sync_middleware.remove(type, id);
                    break;
                case kind::async_middleware:
                    async_middleware.remove(type, id);
                    break;
                }
            }
        };

    public:

        /**
         * @class token
         * @brief Copyable, non-owning handle to a registration. Letting it go out of
         * scope leaves the registration in place; see scoped_token for RAII.
         * @note The token does not automatically unregister; use scoped_token for RAII management.
         */
        class token
        {
        public:
            token() = default;

            // True while the registration is live and the bus still exists.
            bool valid() const noexcept
            {
                auto s = slot_.lock();
                return s && s->active.load(std::memory_order_acquire);
            }

            explicit operator bool() const noexcept { return valid(); }

            // Unregisters. A no-op if already removed or the bus is gone.
            void remove()
            {
                auto st = owner_.lock();
                if (!st)
                    return;
                std::lock_guard lock(st->mutex);
                st->remove(kind_, type_, id_);
            }

        private:
            friend class bus;

            token(const std::shared_ptr<state> &owner, const std::shared_ptr<slot_base> &s, kind k, event_key type) noexcept
                : owner_(owner), slot_(s), kind_(k), type_(type), id_(s->id) {}

            std::weak_ptr<state> owner_;
            std::weak_ptr<slot_base> slot_;
            kind kind_{kind::sync_listener};
            event_key type_{};
            std::size_t id_{0};
        };

        /**
         * @class scoped_token
         * @brief Move-only owner of a registration: removes it on destruction.
         * @note Use this when you want RAII-style management of event registrations.
         */
        class scoped_token
        {
        public:
            scoped_token() = default;
            scoped_token(token t) noexcept : token_(std::move(t)) {}

            scoped_token(const scoped_token &) = delete;
            scoped_token &operator=(const scoped_token &) = delete;

            scoped_token(scoped_token &&other) noexcept : token_(std::exchange(other.token_, token{})) {}

            scoped_token &operator=(scoped_token &&other) noexcept
            {
                if (this != &other)
                {
                    reset();
                    token_ = std::exchange(other.token_, token{});
                }
                return *this;
            }

            scoped_token &operator=(token t) noexcept
            {
                reset();
                token_ = std::move(t);
                return *this;
            }

            ~scoped_token() { reset(); }

            bool valid() const noexcept { return token_.valid(); }
            explicit operator bool() const noexcept { return valid(); }

            // Unregisters now and empties this handle.
            void reset()
            {
                token_.remove();
                token_ = token{};
            }

            // Gives up ownership without unregistering.
            token release() noexcept { return std::exchange(token_, token{}); }

            const token &get() const noexcept { return token_; }

        private:
            token token_;
        };

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
            return install(&state::sync_listeners, kind::sync_listener, event_id<Event>(), priority,
                           [fn = std::forward<F>(f)](const void *ptr) mutable
                           {
                               fn(*static_cast<const Event *>(ptr));
                           });
        }

        template <typename Event, typename F>
            requires coroutine_listener_for<F, Event>
        token add_listener(F &&f, int priority = 0)
        {
            return install(&state::async_listeners, kind::async_listener, event_id<Event>(), priority,
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
            return install(&state::sync_middleware, kind::sync_middleware, event_id<Event>(), priority,
                           [fn = std::forward<F>(f)](void *ev, void *ctx, void (*resume)(void *, void *)) mutable
                           {
                               fn(*static_cast<Event *>(ev), next<Event>{ctx, resume});
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
            return install(&state::async_middleware, kind::async_middleware, event_id<Event>(), priority,
                           // A coroutine rather than a plain forwarder so that the
                           // async_next outlives the user's coroutine, which only
                           // holds a reference to it.
                           [fn = std::forward<F>(f)](void *ev, void *ctx, task<void> (*resume)(void *, void *)) mutable -> task<void>
                           {
                               async_next<Event> n{ctx, resume};
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
            std::shared_ptr<const sync_middleware_list> middleware;
            std::shared_ptr<const sync_listener_list> listeners;
            {
                std::lock_guard lock(state_->mutex);
                middleware = state_->sync_middleware.snapshot(event_id<Event>());
                listeners = state_->sync_listeners.snapshot(event_id<Event>());
            }

            chain_ctx ctx{middleware.get(), listeners.get(), 0};
            run_chain(&ctx, &e);
        }

        template <typename Event>
        task<void> dispatch_async(Event e)
        {
            std::shared_ptr<const async_middleware_list> middleware;
            std::shared_ptr<const async_listener_list> listeners;
            {
                std::lock_guard lock(state_->mutex);
                middleware = state_->async_middleware.snapshot(event_id<Event>());
                listeners = state_->async_listeners.snapshot(event_id<Event>());
            }

            async_chain_ctx ctx{middleware.get(), listeners.get(), 0};
            co_await run_chain_async(&ctx, &e);
        }

    private:
        using sync_listener_list = registry<sync_listener_fn>::list;
        using async_listener_list = registry<async_listener_fn>::list;
        using sync_middleware_list = registry<sync_middleware_fn>::list;
        using async_middleware_list = registry<async_middleware_fn>::list;

        template <typename Fn, typename Callable>
        token install(registry<Fn> state::*reg, kind k, event_key type, int priority, Callable &&callable)
        {
            auto s = std::make_shared<slot<Fn>>(state_->next_id++, priority, std::forward<Callable>(callable));
            {
                std::lock_guard lock(state_->mutex);
                ((*state_).*reg).add(type, s);
            }
            return token{state_, s, k, type};
        }

        struct chain_ctx
        {
            const sync_middleware_list *middleware;
            const sync_listener_list *listeners;
            std::size_t index;
        };

        struct async_chain_ctx
        {
            const async_middleware_list *middleware;
            const async_listener_list *listeners;
            std::size_t index;
        };

        static bool live(const slot_base &s) noexcept { return s.active.load(std::memory_order_acquire); }

        // Runs middleware [index, end) as an onion around the listeners. Each
        // layer gets a fresh context pointing at the next layer, so calling next
        // recurses into this function with index + 1.
        static void run_chain(void *raw, void *ev)
        {
            auto &c = *static_cast<chain_ctx *>(raw);
            if (c.middleware)
            {
                for (std::size_t i = c.index; i < c.middleware->size(); ++i)
                {
                    auto &mw = *(*c.middleware)[i];
                    if (!live(mw))
                        continue;
                    chain_ctx rest{c.middleware, c.listeners, i + 1};
                    mw.fn(ev, &rest, &run_chain);
                    return;
                }
            }

            if (c.listeners)
                for (const auto &s : *c.listeners)
                    if (live(*s))
                        s->fn(ev);
        }

        static task<void> run_chain_async(void *raw, void *ev)
        {
            auto &c = *static_cast<async_chain_ctx *>(raw);
            if (c.middleware)
            {
                for (std::size_t i = c.index; i < c.middleware->size(); ++i)
                {
                    auto &mw = *(*c.middleware)[i];
                    if (!live(mw))
                        continue;
                    async_chain_ctx rest{c.middleware, c.listeners, i + 1};
                    co_await mw.fn(ev, &rest, &run_chain_async);
                    co_return;
                }
            }

            if (c.listeners)
                for (const auto &s : *c.listeners)
                    if (live(*s))
                        co_await s->fn(ev);
        }

        std::shared_ptr<state> state_ = std::make_shared<state>();
    };

} // namespace catalyst::events