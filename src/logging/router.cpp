/**
 * @file router.cpp
 * @brief Implements the router declared in router.hpp.
 * @details Two pieces of state, guarded differently for two different access patterns. The category
 * levels sit behind a shared_mutex because they are read far more often than written, with an atomic
 * flag in front so the common case - no overrides at all - never takes the lock. The sink list is
 * copy-on-write behind a plain mutex: a dispatch copies the shared_ptr, drops the lock, and calls
 * sinks from there, so a sink is free to add or remove sinks, including itself, while it runs.
 *
 * Because a dispatch may be holding a snapshot the router has already replaced, everything a
 * registration needs at call time lives in its `sink_control`, held by shared_ptr: the lock a
 * `serialized` sink is called under, the count of threads currently inside it, and its counters.
 * That count is what lets `remove_sink` promise the sink is quiet by the time it returns, which is
 * the whole basis on which a sink registered by reference can safely be destroyed.
 *
 * The middleware chain is copy-on-write too, and simpler, because nothing outside the router ever
 * owns a middleware: entries are held by shared_ptr, so a removal builds a list without one while a
 * dispatch already running keeps the old list, and the callable with it, alive until it is done.
 * Only an `active` flag has to be shared, to stop that same dispatch starting it again on its way
 * through. There is nothing to wait for, and so no quiescence machinery on this side.
 *
 * Two thread_locals keep sinks from turning the router against itself. One is the stack of routers
 * this thread is dispatching through, so an event logged from inside a sink is dropped instead of
 * recursing. The other is the sink this thread is currently inside, so that a sink removing itself
 * is not asked to wait for itself to finish.
 * License: MIT (see LICENSE).
 */

#include <catalyst/logging/router.hpp>

#include <algorithm>
#include <cstddef>
#include <string>
#include <string_view>
#include <utility>

namespace catalyst::logging
{

    namespace
    {
        // ---------------------------------------------------------------------
        // Reentrancy
        // ---------------------------------------------------------------------

        /// Deep enough for a sink forwarding to another router, and far short of a runaway.
        constexpr std::size_t max_nesting = 8;

        thread_local const router *dispatching[max_nesting]{};
        thread_local std::size_t dispatch_depth = 0;

        /// The sink this thread is inside right now, so it is not asked to wait for itself.
        thread_local detail::sink_control *current_sink = nullptr;

        /**
         * @class dispatch_guard
         * @brief Marks a router as being dispatched on this thread, and says whether it already was.
         */
        class dispatch_guard
        {
        public:
            explicit dispatch_guard(const router *r) noexcept
            {
                if (dispatch_depth >= max_nesting)
                    return;
                for (std::size_t i = 0; i < dispatch_depth; ++i)
                    if (dispatching[i] == r)
                        return;
                dispatching[dispatch_depth++] = r;
                held_ = true;
            }

            dispatch_guard(const dispatch_guard &) = delete;
            dispatch_guard &operator=(const dispatch_guard &) = delete;

            ~dispatch_guard()
            {
                if (held_)
                    --dispatch_depth;
            }

            /// False when this thread is already inside this router, and the event must be dropped.
            [[nodiscard]] explicit operator bool() const noexcept { return held_; }

        private:
            bool held_ = false;
        };

        // ---------------------------------------------------------------------
        // Quiescence
        // ---------------------------------------------------------------------

        void leave(detail::sink_control &control) noexcept
        {
            if (control.in_flight.fetch_sub(1) == 1 && !control.active.load())
            {
                // Taken before the notify so a retiring thread cannot check the count, find it
                // non-zero, and start waiting after the notify has already gone out.
                std::scoped_lock lock(control.retire_mutex);
                control.retired.notify_all();
            }
        }

        /// Registers this thread as being inside the sink, or answers false if it has been removed.
        [[nodiscard]] bool enter(detail::sink_control &control) noexcept
        {
            control.in_flight.fetch_add(1);
            if (control.active.load())
                return true;
            leave(control);
            return false;
        }

        /// Stops new dispatches reaching the sink, then waits for the ones already inside to leave.
        void retire(const std::shared_ptr<detail::sink_control> &control)
        {
            if (!control)
                return;
            control->active.store(false);

            // A sink removing itself from inside its own write is one of the threads in the count,
            // and waiting for it would be waiting for the caller.
            const std::uint32_t mine = current_sink == control.get() ? 1u : 0u;

            std::unique_lock lock(control->retire_mutex);
            control->retired.wait(lock, [&] { return control->in_flight.load() <= mine; });
        }

        /**
         * @class sink_scope
         * @brief Holds a sink open for the length of one call, under whatever lock its contract asks.
         */
        class sink_scope
        {
        public:
            explicit sink_scope(const detail::sink_entry &entry) noexcept
                : control_(entry.control.get()), entered_(control_ != nullptr && enter(*control_))
            {
                if (!entered_)
                    return;
                if (entry.threading == sink_threading::serialized)
                {
                    control_->serialize.lock();
                    locked_ = true;
                }
                previous_ = current_sink;
                current_sink = control_;
            }

            sink_scope(const sink_scope &) = delete;
            sink_scope &operator=(const sink_scope &) = delete;

            ~sink_scope()
            {
                if (!entered_)
                    return;
                current_sink = previous_;
                if (locked_)
                    control_->serialize.unlock();
                leave(*control_);
            }

            /// False when the sink was removed before this call could start.
            [[nodiscard]] explicit operator bool() const noexcept { return entered_; }

        private:
            detail::sink_control *control_ = nullptr;
            detail::sink_control *previous_ = nullptr;
            bool entered_ = false;
            bool locked_ = false;
        };

        /// Raises `target` to `value` if it is lower, for the high-water timing.
        void raise_to(std::atomic<std::uint64_t> &target, std::uint64_t value) noexcept
        {
            std::uint64_t seen = target.load(std::memory_order_relaxed);
            while (seen < value && !target.compare_exchange_weak(seen, value, std::memory_order_relaxed))
            {
            }
        }
    } // namespace

    // ---------------------------------------------------------------------
    // Sinks
    // ---------------------------------------------------------------------

    router::~router()
    {
        clear_sinks();
        clear_middleware();
    }

    void router::install(detail::sink_entry entry)
    {
        std::scoped_lock lock(sinks_mutex_);
        auto next = std::make_shared<detail::sink_list>(*sinks_);
        next->push_back(std::move(entry));
        sinks_ = std::move(next);
    }

    bool router::remove_sink(sink_id id)
    {
        if (id == no_sink)
            return false;

        std::shared_ptr<detail::sink_control> removed;
        {
            std::scoped_lock lock(sinks_mutex_);
            auto next = std::make_shared<detail::sink_list>();
            next->reserve(sinks_->size());
            for (const detail::sink_entry &e : *sinks_)
            {
                if (e.id == id)
                    removed = e.control;
                else
                    next->push_back(e);
            }
            if (!removed)
                return false;
            sinks_ = std::move(next);
        }

        // Outside the lock: a dispatch still inside the sink is free to add or remove sinks of its
        // own before it finishes, and it cannot do that if this thread is holding the list.
        retire(removed);
        return true;
    }

    void router::clear_sinks()
    {
        std::shared_ptr<const detail::sink_list> removed;
        {
            std::scoped_lock lock(sinks_mutex_);
            removed = std::exchange(sinks_, std::make_shared<detail::sink_list>());
        }
        for (const detail::sink_entry &e : *removed)
            retire(e.control);
    }

    std::size_t router::sink_count() const
    {
        std::scoped_lock lock(sinks_mutex_);
        return sinks_->size();
    }

    std::shared_ptr<const detail::sink_list> router::active_sinks() const
    {
        std::scoped_lock lock(sinks_mutex_);
        return sinks_;
    }

    bool router::assign_filter(sink_id id, std::function<bool(const log_event &)> filter)
    {
        if (id == no_sink)
            return false;

        std::scoped_lock lock(sinks_mutex_);
        auto next = std::make_shared<detail::sink_list>(*sinks_);
        const auto at = std::ranges::find(*next, id, &detail::sink_entry::id);
        if (at == next->end())
            return false;

        at->filter = std::move(filter);
        sinks_ = std::move(next);
        return true;
    }

    // ---------------------------------------------------------------------
    // Middleware
    // ---------------------------------------------------------------------

    middleware_id router::install_middleware(int priority, detail::middleware_fn invoke)
    {
        const middleware_id id = next_middleware_id_.fetch_add(1, std::memory_order_relaxed);
        auto entry = std::make_shared<detail::middleware_entry>(id, priority, std::move(invoke));

        std::scoped_lock lock(middleware_mutex_);
        auto next = std::make_shared<detail::middleware_list>(*middleware_);
        // Descending priority, inserted after everything of equal priority, so the highest priority
        // is the outermost layer and equal priorities run in the order they were registered.
        const auto at = std::ranges::find_if(*next, [priority](const std::shared_ptr<detail::middleware_entry> &e)
                                             { return e->priority < priority; });
        next->insert(at, std::move(entry));
        middleware_ = std::move(next);
        has_middleware_.store(true, std::memory_order_release);
        return id;
    }

    bool router::remove_middleware(middleware_id id)
    {
        if (id == no_middleware)
            return false;

        std::scoped_lock lock(middleware_mutex_);
        const auto at = std::ranges::find(*middleware_, id, [](const std::shared_ptr<detail::middleware_entry> &e)
                                          { return e->id; });
        if (at == middleware_->end())
            return false;

        // Cleared before the list is swapped, so a dispatch part-way through the old snapshot steps
        // over the entry rather than running something the caller has finished with.
        (*at)->active.store(false, std::memory_order_release);

        auto next = std::make_shared<detail::middleware_list>(*middleware_);
        next->erase(next->begin() + (at - middleware_->begin()));
        has_middleware_.store(!next->empty(), std::memory_order_release);
        middleware_ = std::move(next);
        return true;
    }

    void router::clear_middleware()
    {
        std::scoped_lock lock(middleware_mutex_);
        for (const std::shared_ptr<detail::middleware_entry> &e : *middleware_)
            e->active.store(false, std::memory_order_release);
        middleware_ = std::make_shared<detail::middleware_list>();
        has_middleware_.store(false, std::memory_order_release);
    }

    std::size_t router::middleware_count() const
    {
        std::scoped_lock lock(middleware_mutex_);
        return middleware_->size();
    }

    std::shared_ptr<const detail::middleware_list> router::active_middleware() const
    {
        std::scoped_lock lock(middleware_mutex_);
        return middleware_;
    }

    // ---------------------------------------------------------------------
    // Levels
    // ---------------------------------------------------------------------

    void router::set_category_level(std::string_view category, log_level level)
    {
        std::unique_lock lock(levels_mutex_);
        category_levels_.insert_or_assign(std::string(category), level);
        has_category_levels_.store(true, std::memory_order_release);
    }

    void router::clear_category_level(std::string_view category)
    {
        std::unique_lock lock(levels_mutex_);
        if (auto it = category_levels_.find(category); it != category_levels_.end())
            category_levels_.erase(it);
        has_category_levels_.store(!category_levels_.empty(), std::memory_order_release);
    }

    void router::clear_category_levels()
    {
        std::unique_lock lock(levels_mutex_);
        category_levels_.clear();
        has_category_levels_.store(false, std::memory_order_release);
    }

    std::optional<log_level> router::get_category_level(std::string_view category) const
    {
        if (!has_category_levels_.load(std::memory_order_acquire))
            return std::nullopt;
        std::shared_lock lock(levels_mutex_);
        if (auto it = category_levels_.find(category); it != category_levels_.end())
            return it->second;
        return std::nullopt;
    }

    std::optional<log_level> router::inherited_category_level(std::string_view category) const
    {
        if (!has_category_levels_.load(std::memory_order_acquire))
            return std::nullopt;
        std::shared_lock lock(levels_mutex_);
        // Longest match first: "physics.solver.narrowphase", then "physics.solver", then "physics".
        // One lock for the whole walk, since a category is rarely more than three deep.
        for (std::string_view scope = category; !scope.empty();)
        {
            if (auto it = category_levels_.find(scope); it != category_levels_.end())
                return it->second;
            const std::size_t dot = scope.find_last_of('.');
            if (dot == std::string_view::npos)
                break;
            scope = scope.substr(0, dot);
        }
        return std::nullopt;
    }

    log_level router::effective_level(std::string_view category) const
    {
        return inherited_category_level(category).value_or(get_minimum_level());
    }

    bool router::should_log_checked(log_level level, std::string_view category) const
    {
        return level >= effective_level(category);
    }

    // ---------------------------------------------------------------------
    // Dispatch
    // ---------------------------------------------------------------------

    void router::dispatch_to(const detail::sink_entry &entry, const log_event &event, line_cache &cache,
                             const watchdog *guard) const
    {
        detail::sink_control *const control = entry.control.get();

        // Asked before the sink is entered, so a registration that turns the event away costs no
        // lock and no in-flight count. The predicate belongs to the entry, which this dispatch's
        // snapshot keeps alive, so it stays callable even as the registration is being removed.
        if (entry.filter)
        {
            bool accepted = false;
            try
            {
                accepted = entry.filter(event);
            }
            catch (...)
            {
                // The same bargain as a sink that throws: counted, and this registration skipped.
                control->exceptions.fetch_add(1, std::memory_order_relaxed);
                return;
            }
            if (!accepted)
            {
                control->filtered.fetch_add(1, std::memory_order_relaxed);
                return;
            }
        }

        const bool timed = guard != nullptr || timing_.load(std::memory_order_relaxed);

        std::chrono::nanoseconds elapsed{};
        {
            const sink_scope scope(entry);
            if (!scope)
                return;

            const auto started = timed ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};

            // A sink that throws would otherwise put an exception in the one place no caller has a
            // handler for it: the log statement. It is counted instead, and the next sink still runs.
            try
            {
                entry.write(event, cache);
            }
            catch (...)
            {
                control->exceptions.fetch_add(1, std::memory_order_relaxed);
            }
            control->written.fetch_add(1, std::memory_order_relaxed);

            if (!timed)
                return;

            elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
            control->nanoseconds.fetch_add(static_cast<std::uint64_t>(elapsed.count()), std::memory_order_relaxed);
            raise_to(control->max_nanoseconds, static_cast<std::uint64_t>(elapsed.count()));
        }

        // Reported after the sink has been left, so the handler is not running inside the lock a
        // serialized sink is called under, nor counted as being in flight in it.
        if (guard != nullptr && guard->handler && elapsed >= guard->budget)
            guard->handler(entry.id, elapsed);
    }

    void router::flush_one(const detail::sink_entry &entry) const
    {
        if (!entry.flush)
            return;
        const sink_scope scope(entry);
        if (!scope)
            return;
        try
        {
            entry.flush();
        }
        catch (...)
        {
            entry.control->exceptions.fetch_add(1, std::memory_order_relaxed);
        }
    }

    struct router::chain_ctx
    {
        const detail::middleware_list *middleware; ///< The chain, or null when there is none.
        std::size_t index;                         ///< The layer to carry on from.
        const detail::sink_list *sinks;            ///< What the chain comes out into.
        const router *self;
        const watchdog *guard;
        bool delivered; ///< Whether the event reached the sinks rather than being stopped.
    };

    void router::run_chain(void *context, log_event &event)
    {
        chain_ctx &c = *static_cast<chain_ctx *>(context);

        // Each layer gets a context pointing at the next one, so a middleware calling next() lands
        // back here one layer further in. An entry removed while this dispatch was under way is
        // stepped over rather than run.
        if (c.middleware != nullptr)
        {
            for (std::size_t i = c.index; i < c.middleware->size(); ++i)
            {
                detail::middleware_entry &layer = *(*c.middleware)[i];
                if (!layer.active.load(std::memory_order_acquire))
                    continue;

                chain_ctx rest{c.middleware, i + 1, c.sinks, c.self, c.guard, false};
                layer.invoke(event, &rest, &run_chain);
                // A layer that never called next leaves this false, which is how the router tells a
                // dropped event from a delivered one.
                c.delivered = rest.delivered;
                return;
            }
        }

        c.self->deliver(*c.sinks, event, c.guard);
        c.delivered = true;
    }

    void router::deliver(const detail::sink_list &sinks, const log_event &event, const watchdog *guard) const
    {
        // One cache for the whole dispatch: two text sinks that print the same columns format the
        // event once between them.
        line_cache cache(event);
        for (const detail::sink_entry &e : sinks)
            dispatch_to(e, event, cache, guard);
    }

    void router::log(log_event event)
    {
        // Checked again here: an event can arrive through emit() without having passed a level
        // check, and the level may have moved since one that did.
        if (!should_log(event.level, event.category))
            return;

        // A sink or a middleware that logs while it is running would arrive back here on the same
        // thread and recurse until the stack gave out. Dropping is the one answer that neither
        // recurses nor deadlocks; the count says how often it happened.
        const dispatch_guard guard(this);
        if (!guard)
        {
            dropped_reentrant_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // Stamped before the middleware runs, so a middleware sees the number the event would have
        // had, and an event stopped there leaves a gap that says something was dropped.
        event.sequence = next_sequence_.fetch_add(1, std::memory_order_relaxed) + 1;

        const auto active = active_sinks();
        const auto chain = has_middleware_.load(std::memory_order_acquire) ? active_middleware() : nullptr;
        const auto dog = has_watchdog_.load(std::memory_order_acquire) ? active_watchdog() : nullptr;

        chain_ctx context{chain.get(), 0, active.get(), this, dog.get(), false};
        try
        {
            run_chain(&context, event);
        }
        catch (...)
        {
            // Sinks swallow their own exceptions, so anything arriving here came from a middleware.
            // The event is abandoned rather than half-delivered: a chain cannot be resumed from the
            // middle of a layer that gave up, and the layers around it have already unwound.
            middleware_exceptions_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        if (!context.delivered)
        {
            dropped_by_middleware_.fetch_add(1, std::memory_order_relaxed);
            return;
        }

        // The record of a subsystem or a process giving up belongs on disk, not in a buffer.
        if (event.level >= flush_level_.load(std::memory_order_relaxed))
            for (const detail::sink_entry &e : *active)
                flush_one(e);
    }

    void router::flush()
    {
        const auto active = active_sinks();
        for (const detail::sink_entry &e : *active)
            flush_one(e);
    }

    // ---------------------------------------------------------------------
    // Diagnostics
    // ---------------------------------------------------------------------

    namespace
    {
        [[nodiscard]] sink_stats read_stats(const detail::sink_entry &entry) noexcept
        {
            sink_stats stats;
            stats.threading = entry.threading;
            if (!entry.control)
                return stats;
            stats.written = entry.control->written.load(std::memory_order_relaxed);
            stats.filtered = entry.control->filtered.load(std::memory_order_relaxed);
            stats.exceptions = entry.control->exceptions.load(std::memory_order_relaxed);
            stats.total_time =
                std::chrono::nanoseconds(entry.control->nanoseconds.load(std::memory_order_relaxed));
            stats.max_time =
                std::chrono::nanoseconds(entry.control->max_nanoseconds.load(std::memory_order_relaxed));
            return stats;
        }
    } // namespace

    std::optional<sink_stats> router::sink_statistics(sink_id id) const
    {
        const auto active = active_sinks();
        const auto it = std::ranges::find(*active, id, &detail::sink_entry::id);
        if (it == active->end())
            return std::nullopt;
        return read_stats(*it);
    }

    std::vector<std::pair<sink_id, sink_stats>> router::all_sink_statistics() const
    {
        const auto active = active_sinks();
        std::vector<std::pair<sink_id, sink_stats>> out;
        out.reserve(active->size());
        for (const detail::sink_entry &e : *active)
            out.emplace_back(e.id, read_stats(e));
        return out;
    }

    std::shared_ptr<const router::watchdog> router::active_watchdog() const
    {
        std::scoped_lock lock(watchdog_mutex_);
        return watchdog_;
    }

    void router::set_slow_sink_handler(slow_sink_handler handler, std::chrono::nanoseconds budget)
    {
        const bool wanted = static_cast<bool>(handler);
        auto next = wanted ? std::make_shared<const watchdog>(watchdog{std::move(handler), budget}) : nullptr;
        {
            std::scoped_lock lock(watchdog_mutex_);
            watchdog_ = std::move(next);
        }
        // Timing is what the budget is measured against, so asking for one asks for the other. It is
        // left on afterwards, because turning it off again would surprise a caller who set it.
        if (wanted)
            timing_.store(true, std::memory_order_relaxed);
        has_watchdog_.store(wanted, std::memory_order_release);
    }

    // ---------------------------------------------------------------------
    // The default router
    // ---------------------------------------------------------------------

    router &default_logger()
    {
        static router instance;
        return instance;
    }

} // namespace catalyst::logging
