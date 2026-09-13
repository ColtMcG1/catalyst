/**
 * @file sink_entry.hpp
 * @brief The router's type-erased view of a registered sink, and the state it keeps alongside it.
 * @details The router stores sinks as two `std::function`s and an id, so that everything it does
 * with them - installing, removing, dispatching, flushing - is ordinary code compiled once, and only
 * the erasure itself has to be a template. `make_entry` is that template.
 *
 * Each entry also carries a `sink_control`, held by `shared_ptr` so that it outlives the copy-on-write
 * sink list it was installed into. Three things need somewhere to live that a dispatch already
 * holding a stale snapshot can still reach:
 *
 *   - the lock a `serialized` sink is called under,
 *   - the in-flight count that lets `remove_sink` wait until nobody is inside the sink any more,
 *     which is the only thing that makes registering a sink by reference safe to undo,
 *   - the counters and timings the router reports back.
 *
 * The entry also carries the registration's filter, when it was given one. It sits here rather than
 * in a wrapper around the sink because it is a property of the registration, not of the sink: the
 * same sink registered twice can answer to two different audiences, and a filter that lives in the
 * entry costs the sink type nothing and the router one null check.
 *
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/logging/event.hpp>
#include <catalyst/logging/sink.hpp>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace catalyst::logging::detail
{

    /**
     * @struct sink_control
     * @brief The per-registration state that has to outlive the sink list it was installed into.
     * @details Shared between the entry in the router's current sink list and every dispatch still
     * running against an older one.
     */
    struct sink_control
    {
        /// Cleared by `remove_sink` before it waits. A dispatch that sees it false leaves the sink alone.
        std::atomic<bool> active{true};

        /// How many threads are inside this sink right now.
        std::atomic<std::uint32_t> in_flight{0};

        /// Held around `write` and `flush` for a sink that declared `serialized`.
        std::mutex serialize;

        std::atomic<std::uint64_t> written{0};    ///< Events handed to the sink.
        std::atomic<std::uint64_t> filtered{0};   ///< Events the registration's filter turned away.
        std::atomic<std::uint64_t> exceptions{0}; ///< Calls that threw, and were swallowed.
        std::atomic<std::uint64_t> nanoseconds{0};     ///< Total time in the sink, when timing is on.
        std::atomic<std::uint64_t> max_nanoseconds{0}; ///< The worst single call, when timing is on.

        /// Woken when the last thread leaves a sink that is no longer active.
        std::mutex retire_mutex;
        std::condition_variable retired;
    };

    /**
     * @struct sink_entry
     * @brief One registered sink: its id, its contract, how to write to it, and how to flush it.
     * @details `flush` is empty for a sink that does not model `flushable_sink`, and `filter` is
     * empty for a registration that takes everything.
     */
    struct sink_entry
    {
        sink_id id = no_sink;
        sink_threading threading = sink_threading::reentrant;
        std::shared_ptr<sink_control> control{};
        std::function<void(const log_event &, line_cache &)> write{};
        std::function<void()> flush{};

        /// What this registration accepts. Empty means everything; see `router::set_sink_filter`.
        std::function<bool(const log_event &)> filter{};
    };

    /// Calls whichever `write` the sink has, passing the shared cache to one that can use it.
    template <log_sink S>
    void write_to(S &sink, const log_event &event, line_cache &cache)
    {
        if constexpr (text_sink<S>)
            sink.write(event, cache);
        else
            sink.write(event);
    }

    /// The router's sink table. Copy-on-write, so a dispatch can hold a snapshot without a lock.
    using sink_list = std::vector<sink_entry>;

    /**
     * @brief Erases a sink the router shares ownership of.
     * @tparam S The sink type.
     * @param id The id to give the registration.
     * @param sink The sink; kept alive by the returned entry.
     * @return The entry to install.
     */
    template <log_sink S>
    [[nodiscard]] sink_entry make_entry(sink_id id, std::shared_ptr<S> sink)
    {
        sink_entry entry;
        entry.id = id;
        entry.threading = sink_threading_of<S>;
        entry.control = std::make_shared<sink_control>();
        entry.write = [sink](const log_event &event, line_cache &cache) { write_to(*sink, event, cache); };
        if constexpr (flushable_sink<S>)
            entry.flush = [sink] { sink->flush(); };
        return entry;
    }

    /**
     * @brief Erases a sink somebody else owns.
     * @tparam S The sink type.
     * @param id The id to give the registration.
     * @param sink The sink; the caller keeps it alive until the registration is removed. Removal
     *        waits for any dispatch still inside it, so returning from `remove_sink` is the point
     *        after which destroying the sink is safe.
     * @return The entry to install.
     */
    template <log_sink S>
    [[nodiscard]] sink_entry make_entry(sink_id id, S *sink)
    {
        sink_entry entry;
        entry.id = id;
        entry.threading = sink_threading_of<S>;
        entry.control = std::make_shared<sink_control>();
        entry.write = [sink](const log_event &event, line_cache &cache) { write_to(*sink, event, cache); };
        if constexpr (flushable_sink<S>)
            entry.flush = [sink] { sink->flush(); };
        return entry;
    }

} // namespace catalyst::logging::detail
