/**
 * @file router.hpp
 * @brief The object that takes every event, runs it through the middleware, and hands it to every sink.
 * @details Sinks are added three ways, depending on who needs to keep hold of them:
 *
 *     add_sink(console_sink{})                        by value; the router owns the copy
 *     add_sink(std::make_shared<ring_buffer_sink>())  shared; the caller reads it later
 *     add_sink(std::ref(my_sink))                     by reference; the caller keeps it alive
 *
 * Each returns a `sink_id` that `remove_sink()` takes back, or wrap the id in a `scoped_sink` (see
 * sink.hpp) to have that happen when the owner goes away - a GUI panel that comes and goes registers
 * its sink for exactly as long as it exists.
 *
 * Whichever way, the sink has to have declared its threading contract (see sink.hpp) and it has to
 * be one the router can honour: a sink that only its own thread may touch cannot be called by a
 * logging thread on any terms, and `add_sink` refuses it at compile time rather than at three in the
 * morning. What the router does with the other two:
 *
 *   - `reentrant`  called directly, from as many threads as are logging.
 *   - `serialized` called under a lock the router keeps for that registration.
 *
 * A registration may also carry a filter, which is how one router feeds several audiences: the
 * console gets everything, a log panel gets info and above, and a subsystem's tab gets only its own
 * category.
 *
 *     add_sink(panel, level_at_least(log_level::info));
 *
 * The filter belongs to the registration rather than to the sink, so the same sink registered twice
 * can answer to two audiences, and a sink type never has to know it is being filtered.
 *
 * Before any of that, an event passes through the middleware chain, which is where it can be
 * changed or stopped for every sink at once - redacting a secret, tagging a frame number, timing the
 * whole dispatch. See middleware.hpp.
 *
 * Levels are decided before a message is formatted, so a trace call that nobody wants costs a load
 * and a compare. There is a minimum for everything and an optional override per category, so
 * "vulkan" can be held at warn while the rest runs at debug. Overrides nest: one set on "physics"
 * covers "physics.solver" as well, and the longest one covering a category wins.
 *
 * Three things the router does so that a sink cannot take the process with it:
 *
 *   - A sink that throws has the exception swallowed and counted, and the next sink still gets the
 *     event. A log call is the last place a caller expects a throw to come from. Middleware is
 *     covered the same way, though a middleware that throws does abandon the event: a chain cannot
 *     be resumed from the middle of a layer that gave up.
 *   - A sink or middleware that logs while it is running would re-enter the router on the same
 *     thread and recurse until the stack ran out. The nested event is dropped instead, and counted;
 *     see `dropped_reentrant()`. Something with a report to make makes it out of band.
 *   - `remove_sink()` does not return until no thread is inside that sink any more, so the moment it
 *     returns is the moment the sink can be destroyed. This is what makes `add_sink(std::ref(...))`
 *     and `scoped_sink` safe for something with a real lifetime, like a panel.
 *
 * The sink list and the middleware chain are both copy-on-write and are called with no router lock
 * held, so either may add or remove registrations - including its own - while it runs. Removing
 * itself from inside its own `write` returns immediately rather than waiting, since it plainly
 * cannot wait for itself. Removing a *different* sink from in there does wait for it, with the one
 * consequence worth naming: two sinks that each remove the other, from inside their own `write`, at
 * the same time, will wait for each other. Nothing else in here can deadlock on a sink.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/logging/detail/middleware_entry.hpp>
#include <catalyst/logging/detail/sink_entry.hpp>
#include <catalyst/logging/detail/traits.hpp>
#include <catalyst/logging/event.hpp>
#include <catalyst/logging/filter.hpp>
#include <catalyst/logging/level.hpp>
#include <catalyst/logging/middleware.hpp>
#include <catalyst/logging/sink.hpp>

#include <atomic>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace catalyst::logging
{

    /**
     * @struct sink_stats
     * @brief What one registration has done since it was added.
     * @details The counts are always kept. The timings are only accumulated while
     * `router::timing_enabled()`, and are zero otherwise: two clock reads per sink per event is not
     * a price a trace-heavy frame should pay to find out something it is not asking about.
     */
    struct sink_stats
    {
        sink_threading threading = sink_threading::reentrant; ///< The contract the sink declared.
        std::uint64_t written = 0;                            ///< Events handed to the sink.
        std::uint64_t filtered = 0;                           ///< Events this registration's filter turned away.
        std::uint64_t exceptions = 0;                         ///< Calls that threw, and were swallowed.
        std::chrono::nanoseconds total_time{};                ///< Time spent inside the sink.
        std::chrono::nanoseconds max_time{};                  ///< The worst single call.
    };

    /**
     * @class router
     * @brief Holds the sinks and the middleware, decides what is worth logging, and dispatches.
     */
    class router
    {
    public:
        router() = default;
        router(const router &) = delete;
        router &operator=(const router &) = delete;

        /// Retires every sink first, so no thread is left inside one while the router comes apart.
        ~router();

        // -----------------------------------------------------------------
        // Sinks
        // -----------------------------------------------------------------

        /**
         * @brief Adds a sink by value.
         * @tparam S The sink type.
         * @param sink The sink; the router owns the copy and nothing else can reach it afterwards.
         * @return The registration's id.
         */
        template <writable_sink S>
        sink_id add_sink(S sink)
        {
            check_registerable<S>();
            return add_shared(std::make_shared<S>(std::move(sink)), {});
        }

        /**
         * @brief Adds a sink by value, for the events that pass a filter.
         * @tparam S The sink type.
         * @tparam F The predicate; see filter.hpp.
         * @param sink The sink; the router owns the copy.
         * @param filter What this registration accepts. Events it turns away never reach the sink.
         * @return The registration's id.
         */
        template <writable_sink S, event_filter F>
        sink_id add_sink(S sink, F filter)
        {
            check_registerable<S>();
            return add_shared(std::make_shared<S>(std::move(sink)), std::move(filter));
        }

        /**
         * @brief Adds a sink the caller also holds, so it can read what the sink collected.
         * @tparam S The sink type.
         * @param sink The sink; kept alive by the router for as long as it is registered.
         * @return The registration's id, or `no_sink` when `sink` is null.
         */
        template <writable_sink S>
        sink_id add_sink(std::shared_ptr<S> sink)
        {
            check_registerable<S>();
            return add_shared(std::move(sink), {});
        }

        /// `add_sink`, for the events that pass a filter.
        template <writable_sink S, event_filter F>
        sink_id add_sink(std::shared_ptr<S> sink, F filter)
        {
            check_registerable<S>();
            return add_shared(std::move(sink), std::move(filter));
        }

        /**
         * @brief Adds a sink by reference.
         * @tparam S The sink type.
         * @param sink The sink; the caller keeps it alive until it is removed, or until the router goes.
         * @return The registration's id.
         * @details `remove_sink` waits for any dispatch still inside the sink before it returns, so
         * removing and then destroying is safe in that order, from any thread.
         */
        template <writable_sink S>
        sink_id add_sink(std::reference_wrapper<S> sink)
        {
            return add_referenced(sink, {});
        }

        /// `add_sink`, for the events that pass a filter.
        template <writable_sink S, event_filter F>
        sink_id add_sink(std::reference_wrapper<S> sink, F filter)
        {
            return add_referenced(sink, std::move(filter));
        }

        /**
         * @brief Removes a sink, and waits for it to be quiet.
         * @param id The registration to remove.
         * @return False when no sink has that id.
         * @details Returns once no thread is inside the sink's `write` or `flush`, which is what
         * makes it safe to destroy the sink next. The one exception is a sink removing itself from
         * inside its own `write`: that returns without waiting, because the only thread it would be
         * waiting for is the caller.
         */
        bool remove_sink(sink_id id);

        /// Removes every sink, waiting for each the way `remove_sink` does.
        void clear_sinks();

        /// How many sinks are registered.
        [[nodiscard]] std::size_t sink_count() const;

        /**
         * @brief `add_sink`, with the registration tied to the returned object's lifetime.
         * @tparam Args The sink, in any of the forms `add_sink` accepts, and optionally a filter.
         * @param args What to hand to `add_sink`.
         * @return The scope guard holding the registration.
         */
        template <typename... Args>
        [[nodiscard]] scoped_sink add_scoped_sink(Args &&...args)
        {
            return scoped_sink{*this, add_sink(std::forward<Args>(args)...)};
        }

        /**
         * @brief Builds a sink in place and adds it.
         * @tparam S The sink type to construct.
         * @param args The arguments to construct it from.
         * @return The registration's id.
         * @details The way to register a sink that cannot be moved, which is most sinks that hold a
         * lock: `add_sink(file_sink{"app.log"})` has to move the sink into the router and a
         * `std::mutex` member forbids that, while `emplace_sink<file_sink>("app.log")` constructs it
         * where it will live. A filter goes on afterwards, with `set_sink_filter`.
         */
        template <writable_sink S, typename... Args>
            requires std::constructible_from<S, Args...>
        sink_id emplace_sink(Args &&...args)
        {
            check_registerable<S>();
            return add_shared(std::make_shared<S>(std::forward<Args>(args)...), {});
        }

        /// `emplace_sink`, with the registration tied to the returned object's lifetime.
        template <writable_sink S, typename... Args>
            requires std::constructible_from<S, Args...>
        [[nodiscard]] scoped_sink emplace_scoped_sink(Args &&...args)
        {
            return scoped_sink{*this, emplace_sink<S>(std::forward<Args>(args)...)};
        }

        /**
         * @brief Sets or replaces what one registration accepts.
         * @tparam F The predicate; see filter.hpp.
         * @param id The registration to filter.
         * @param filter What it accepts from now on.
         * @return False when no sink has that id.
         * @details This is how an emplaced sink gets a filter, and how one already registered gets a
         * different one. A dispatch already under way may still be using the old filter; the next
         * one uses this.
         */
        template <event_filter F>
        bool set_sink_filter(sink_id id, F filter)
        {
            return assign_filter(id, std::function<bool(const log_event &)>(std::move(filter)));
        }

        /// Returns a registration to accepting everything.
        bool clear_sink_filter(sink_id id) { return assign_filter(id, {}); }

        // -----------------------------------------------------------------
        // Middleware
        // -----------------------------------------------------------------

        /**
         * @brief Adds a middleware that is handed the rest of the chain.
         * @tparam F The middleware type; see middleware.hpp.
         * @param middleware The middleware; the router owns it, and calls it on every logging thread.
         * @param priority Higher runs further out: first into the event, last out of it.
         * @return The registration's id.
         * @details Equal priorities run in registration order. The middleware may be called
         * concurrently by as many threads as are logging, so anything it shares it synchronises.
         */
        template <chain_middleware F>
        middleware_id add_middleware(F &&middleware, int priority = 0)
        {
            return install_middleware(
                priority,
                [fn = std::forward<F>(middleware)](log_event &event, void *context,
                                                   detail::resume_fn resume) mutable
                { fn(event, next{detail::chain_access{}, context, resume}); });
        }

        /**
         * @brief Adds a middleware that says whether the event carries on.
         * @tparam F The middleware type; a predicate over a mutable event.
         * @param middleware Returns true to continue the chain, false to stop the event here.
         * @param priority Higher runs first, and so gets to stop the event before the rest look at it.
         * @return The registration's id.
         */
        template <log_middleware F>
            requires(!chain_middleware<F>)
        middleware_id add_middleware(F &&middleware, int priority = 0)
        {
            return add_middleware(
                [fn = std::forward<F>(middleware)](log_event &event, const next &rest) mutable
                {
                    if (fn(event))
                        rest(event);
                },
                priority);
        }

        /**
         * @brief Removes a middleware.
         * @param id The registration to remove.
         * @return False when no middleware has that id.
         * @details Returns as soon as no dispatch can start the middleware again. Unlike
         * `remove_sink` it does not wait for one already inside it: there is nothing to wait for on
         * the caller's behalf, because the router owns the callable and keeps it alive until every
         * dispatch that started with it has finished. What that does mean is that a middleware may
         * still be running on another thread when this returns, so anything it captured by reference
         * has to outlive the router, or be removed by the thread that owns it and nothing else.
         */
        bool remove_middleware(middleware_id id);

        /// Removes every middleware.
        void clear_middleware();

        /// How many middleware are registered.
        [[nodiscard]] std::size_t middleware_count() const;

        /// `add_middleware`, with the registration tied to the returned object's lifetime.
        template <typename F>
        [[nodiscard]] scoped_middleware add_scoped_middleware(F &&middleware, int priority = 0)
        {
            return scoped_middleware{*this, add_middleware(std::forward<F>(middleware), priority)};
        }

        // -----------------------------------------------------------------
        // Levels
        // -----------------------------------------------------------------

        /// Sets the level everything is held at, unless its category says otherwise.
        void set_minimum_level(log_level level) noexcept { minimum_level_.store(level, std::memory_order_relaxed); }

        [[nodiscard]] log_level get_minimum_level() const noexcept
        {
            return minimum_level_.load(std::memory_order_relaxed);
        }

        /**
         * @brief Holds one category, and everything under it, at its own minimum.
         * @param category The category name, dotted: "physics" covers "physics.solver" too.
         * @param level The level to hold it at, above or below the global one.
         * @details Overrides nest the way the `category_under` filter does. Setting "physics" to
         * warn quietens every category below it, and setting "physics.solver" to debug afterwards
         * exempts that one: the longest override that covers a category is the one that applies.
         */
        void set_category_level(std::string_view category, log_level level);

        /// Returns a category to the global minimum.
        void clear_category_level(std::string_view category);

        /// Forgets every category override.
        void clear_category_levels();

        /// The override set for exactly this category, if it has one of its own.
        [[nodiscard]] std::optional<log_level> get_category_level(std::string_view category) const;

        /// The override that covers a category: its own, or the nearest one above it.
        [[nodiscard]] std::optional<log_level> inherited_category_level(std::string_view category) const;

        /// The level a category is held at: the override covering it, or the global minimum.
        [[nodiscard]] log_level effective_level(std::string_view category) const;

        /**
         * @brief Whether an event would reach the sinks.
         * @param level The level of the event.
         * @param category Its category; the global minimum applies when empty or unknown.
         * @return True when the event passes the level check.
         * @details This is the check every log call makes before it formats anything, so the common
         * case - no category overrides anywhere - is a load and a compare, and only a router that
         * actually has overrides pays for the lookup. It says nothing about the middleware, which
         * only sees an event that has already been built.
         */
        [[nodiscard]] bool should_log(log_level level, std::string_view category = {}) const
        {
            if (!has_category_levels_.load(std::memory_order_acquire))
                return level >= minimum_level_.load(std::memory_order_relaxed);
            return should_log_checked(level, category);
        }

        /**
         * @brief The level from which an event flushes every sink once it has been dispatched.
         * @param level The level to flush from; `critical` and above only, to flush as little as
         *        possible, or `trace` to flush everything.
         * @details `fatal` by default, so that the record of a subsystem or a process giving up is
         * on disk rather than in a buffer when it does.
         */
        void set_flush_level(log_level level) noexcept { flush_level_.store(level, std::memory_order_relaxed); }

        [[nodiscard]] log_level get_flush_level() const noexcept
        {
            return flush_level_.load(std::memory_order_relaxed);
        }

        // -----------------------------------------------------------------
        // Dispatch
        // -----------------------------------------------------------------

        /**
         * @brief Stamps the sequence number, runs the middleware, and hands the event to every sink.
         * @param event The event to dispatch; its level is checked again here.
         * @details The sequence number is stamped before the middleware runs, so middleware sees the
         * number the event would have had and an event stopped there leaves a gap - which is the
         * honest record of something having been dropped.
         *
         * Every sink of one dispatch shares a `line_cache`, so two text sinks configured alike
         * format the event once between them. A registration's filter is asked before its sink is
         * entered. An event at or above the flush level flushes every sink afterwards. A sink that
         * throws is counted and skipped; an event logged from inside a sink or a middleware is
         * dropped, since delivering it would re-enter this function on a thread already inside it.
         */
        void log(log_event event);

        /// Flushes every sink that can be.
        void flush();

        /// The sequence number the last event was given; 0 before any.
        [[nodiscard]] std::uint64_t last_sequence() const noexcept
        {
            return next_sequence_.load(std::memory_order_relaxed);
        }

        // -----------------------------------------------------------------
        // Diagnostics
        // -----------------------------------------------------------------

        /// What one registration has done, or nothing when no sink has that id.
        [[nodiscard]] std::optional<sink_stats> sink_statistics(sink_id id) const;

        /// What every registration has done, in registration order.
        [[nodiscard]] std::vector<std::pair<sink_id, sink_stats>> all_sink_statistics() const;

        /// How many events were dropped because a sink or a middleware logged while it was running.
        [[nodiscard]] std::uint64_t dropped_reentrant() const noexcept
        {
            return dropped_reentrant_.load(std::memory_order_relaxed);
        }

        /// How many events a middleware stopped before any sink saw them.
        [[nodiscard]] std::uint64_t dropped_by_middleware() const noexcept
        {
            return dropped_by_middleware_.load(std::memory_order_relaxed);
        }

        /// How many events were abandoned because a middleware threw. See `remove_middleware`.
        [[nodiscard]] std::uint64_t middleware_exceptions() const noexcept
        {
            return middleware_exceptions_.load(std::memory_order_relaxed);
        }

        /**
         * @brief Turns on the per-sink timing that fills in `sink_stats::total_time`.
         * @param enabled Whether to time each call.
         * @details Off by default: it is two clock reads per sink per event, which is worth paying
         * when chasing a hitch and not worth paying the rest of the time. Setting a slow-sink
         * handler turns it on by itself.
         */
        void set_timing_enabled(bool enabled) noexcept { timing_.store(enabled, std::memory_order_relaxed); }

        [[nodiscard]] bool timing_enabled() const noexcept { return timing_.load(std::memory_order_relaxed); }

        /// Called on the logging thread when a sink takes longer than its budget.
        using slow_sink_handler = std::function<void(sink_id, std::chrono::nanoseconds)>;

        /**
         * @brief Reports a sink that spent longer than `budget` on the logging thread.
         * @param handler What to call; an empty handler turns the reporting off again.
         * @param budget How long a single `write` may take before it is worth reporting.
         * @details This is how a sink that stalls a frame stops being a mystery. The handler runs on
         * the logging thread, immediately after the sink that was slow, with no router lock held -
         * and it must not log: the reentrancy guard would drop the event anyway. Write to a counter,
         * a debugger, or a file the handler owns.
         *
         * Setting a handler turns timing on. Clearing it leaves timing as it was.
         */
        void set_slow_sink_handler(slow_sink_handler handler, std::chrono::nanoseconds budget);

    private:
        [[nodiscard]] sink_id next_id() noexcept { return next_sink_id_.fetch_add(1, std::memory_order_relaxed); }

        /// The diagnostics a sink has to get past to be registered at all.
        template <typename S>
        static constexpr void check_registerable()
        {
            static_assert(detail::undeclared_sink_diagnostic<S>,
                          "This sink has a write() but has not said which threads the router may call it from. "
                          "Derive it from catalyst::logging::sink_contract<T>, or give it a "
                          "'static constexpr sink_threading log_sink_threading = T;', where T is: reentrant if it "
                          "synchronises itself or holds no state, serialized to have the router lock around it, or "
                          "owner_thread if only its own thread may touch it.");

            static_assert(detail::direct_sink_diagnostic<S>,
                          "This sink declared sink_threading::owner_thread, and the router has no way to honour "
                          "that: it dispatches on whichever thread logged. Wrap it in a catalyst::logging::"
                          "queued_sink and register that instead, then call drain() on it from the owning thread - "
                          "once a frame, for a GUI panel.");
        }

        /// The tail of `add_sink` shared by the by-value, shared and emplaced forms.
        template <typename S>
        sink_id add_shared(std::shared_ptr<S> sink, std::function<bool(const log_event &)> filter)
        {
            if (!sink)
                return no_sink;
            const sink_id id = next_id();
            detail::sink_entry entry = detail::make_entry(id, std::move(sink));
            entry.filter = std::move(filter);
            install(std::move(entry));
            return id;
        }

        /// The tail of the by-reference forms.
        template <typename S>
        sink_id add_referenced(std::reference_wrapper<S> sink, std::function<bool(const log_event &)> filter)
        {
            check_registerable<S>();
            const sink_id id = next_id();
            detail::sink_entry entry = detail::make_entry(id, &sink.get());
            entry.filter = std::move(filter);
            install(std::move(entry));
            return id;
        }

        void install(detail::sink_entry entry);

        bool assign_filter(sink_id id, std::function<bool(const log_event &)> filter);

        middleware_id install_middleware(int priority, detail::middleware_fn invoke);

        [[nodiscard]] bool should_log_checked(log_level level, std::string_view category) const;

        /// The sink list as it is right now. Sinks are called through this, with no lock held.
        [[nodiscard]] std::shared_ptr<const detail::sink_list> active_sinks() const;

        /// The middleware chain as it is right now, or null when there is none.
        [[nodiscard]] std::shared_ptr<const detail::middleware_list> active_middleware() const;

        /// The handler and budget as one object, so a dispatch reads both without tearing.
        struct watchdog
        {
            slow_sink_handler handler{};
            std::chrono::nanoseconds budget{};
        };

        [[nodiscard]] std::shared_ptr<const watchdog> active_watchdog() const;

        /// Where one dispatch has got to in the chain. Defined in router.cpp.
        struct chain_ctx;

        /// Runs the chain from `context`, ending in the sinks. This is what `next` calls back into.
        static void run_chain(void *context, log_event &event);

        /// Hands the event to every sink in `sinks`, which is where the chain comes out.
        void deliver(const detail::sink_list &sinks, const log_event &event, const watchdog *guard) const;

        /// Calls one sink's `write` under its contract, timing it and swallowing what it throws.
        void dispatch_to(const detail::sink_entry &entry, const log_event &event, line_cache &cache,
                         const watchdog *guard) const;

        /// Calls one sink's `flush` under its contract, swallowing what it throws.
        void flush_one(const detail::sink_entry &entry) const;

        std::atomic<log_level> minimum_level_{log_level::trace};
        std::atomic<log_level> flush_level_{log_level::fatal};
        std::atomic<bool> has_category_levels_{false};
        mutable std::shared_mutex levels_mutex_;
        std::unordered_map<std::string, log_level, detail::string_hash, std::equal_to<>> category_levels_;

        mutable std::mutex sinks_mutex_;
        std::shared_ptr<const detail::sink_list> sinks_ = std::make_shared<detail::sink_list>();

        std::atomic<bool> has_middleware_{false};
        mutable std::mutex middleware_mutex_;
        std::shared_ptr<const detail::middleware_list> middleware_ = std::make_shared<detail::middleware_list>();

        std::atomic<bool> timing_{false};
        std::atomic<bool> has_watchdog_{false};
        mutable std::mutex watchdog_mutex_;
        std::shared_ptr<const watchdog> watchdog_{};

        std::atomic<std::uint64_t> dropped_reentrant_{0};
        std::atomic<std::uint64_t> dropped_by_middleware_{0};
        std::atomic<std::uint64_t> middleware_exceptions_{0};
        std::atomic<sink_id> next_sink_id_{1};
        std::atomic<middleware_id> next_middleware_id_{1};
        std::atomic<std::uint64_t> next_sequence_{0};
    };

    /// The process-wide router every log function goes through.
    [[nodiscard]] router &default_logger();

} // namespace catalyst::logging
