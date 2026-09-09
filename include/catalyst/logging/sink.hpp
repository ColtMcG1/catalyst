/**
 * @file sink.hpp
 * @brief What a sink is and what it must promise. The sinks themselves are in sinks/.
 * @details A sink is anything with `void write(const log_event &)` that also states, in its own
 * type, which threads the router may call that from. The statement is not optional: a type with a
 * `write` and no contract is not a sink, and `router::add_sink` says so in as many words. The point
 * is that the decision gets made deliberately, once, by whoever knows what the sink touches - not
 * inferred by the router, which cannot know, and not left to a default, which would be wrong for
 * half of them.
 *
 * There are three contracts, declared as a member or through `sink_contract`:
 *
 *     struct my_sink : sink_contract<sink_threading::serialized>
 *     {
 *         void write(const log_event &event);
 *     };
 *
 *   - `reentrant`    Any thread, any number at once. The sink owns its synchronisation, or has no
 *                    state to synchronise. Every stock sink is one, because each holds its own
 *                    lock. The cheapest contract: the router adds nothing.
 *
 *   - `serialized`   Any thread, one at a time. The router holds a lock of its own around `write`,
 *                    so the sink's state is safe without the sink doing anything about it. This is
 *                    the contract for a sink that was written without threading in mind. It costs
 *                    one uncontended lock per event, and it does not make the sink fast - a slow
 *                    `serialized` sink now blocks the other threads trying to log, as well as the
 *                    one that got there first.
 *
 *   - `owner_thread` One particular thread, which is not the one that logged. A GUI panel that
 *                    touches widgets belongs here. The router refuses to register these directly -
 *                    it has no way to honour the requirement - and `add_sink` fails to compile
 *                    pointing at `queued_sink`, which takes the event on the logging thread and
 *                    hands it over when the owner asks for it.
 *
 * Note what the contract does not say. None of them make a sink safe to be slow: `write` runs on
 * the thread that logged, inside whatever that thread was doing, and a frame loop feels every
 * microsecond of it. A sink that talks to a file, a socket, or a GUI wants `async_sink` or
 * `queued_sink` in front of it, whatever contract it declares.
 *
 * `flush()` is optional; the router calls it when the sink has one.
 *
 * This header is the contract and nothing else, so a type declaring one does not drag in an
 * `<fstream>` or an `<iostream>` it has no use for. The sinks that come with the module are in
 * sinks/, one to a file, and sinks.hpp includes all of them.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <catalyst/logging/event.hpp>
#include <catalyst/logging/format.hpp>

#include <concepts>
#include <cstdint>

namespace catalyst::logging
{

    // ---------------------------------------------------------------------
    // The threading contract
    // ---------------------------------------------------------------------

    /**
     * @enum sink_threading
     * @brief Which threads a sink permits the router to call `write` from.
     * @details Every sink declares one. See the file comment for what each costs and what it buys.
     */
    enum class sink_threading : std::uint8_t
    {
        /// Any thread, concurrently. The sink synchronises itself, or holds nothing to synchronise.
        reentrant,

        /// Any thread, one at a time. The router serialises calls so the sink does not have to.
        serialized,

        /// Only the sink's own thread. Cannot be registered directly; put it behind a `queued_sink`.
        owner_thread,
    };

    /**
     * @struct sink_contract
     * @brief A base that declares a sink's threading contract.
     * @tparam Threading The contract to declare.
     * @details Empty, so it costs a sink nothing to derive from. Declaring the member directly does
     * the same thing, and a sink whose type cannot be changed is covered by specialising
     * `sink_traits` instead.
     */
    template <sink_threading Threading>
    struct sink_contract
    {
        static constexpr sink_threading log_sink_threading = Threading;
    };

    namespace detail
    {
        /// Whether `S` states its contract as a member of the type itself.
        template <typename S>
        concept has_threading_member =
            requires { requires std::same_as<std::remove_cv_t<decltype(S::log_sink_threading)>, sink_threading>; };
    }

    /**
     * @struct sink_traits
     * @brief Where the router looks for a sink's threading contract.
     * @tparam S The sink type.
     * @details The primary template is empty, which is what makes an undeclared sink a compile
     * error rather than a guess. A sink declares its contract by deriving from `sink_contract` or
     * by carrying the `log_sink_threading` member itself; a type from somewhere else, that cannot
     * be changed, is declared by specialising this:
     *
     *     template <>
     *     struct catalyst::logging::sink_traits<third_party::sink>
     *     {
     *         static constexpr sink_threading threading = sink_threading::serialized;
     *     };
     */
    template <typename S>
    struct sink_traits
    {
    };

    /// The specialisation that picks up a contract declared on the sink type.
    template <detail::has_threading_member S>
    struct sink_traits<S>
    {
        static constexpr sink_threading threading = S::log_sink_threading;
    };

    /**
     * @concept declares_threading
     * @brief Whether a type states which threads it may be written to from.
     * @tparam S The candidate sink type.
     */
    template <typename S>
    concept declares_threading = requires {
        { sink_traits<S>::threading } -> std::convertible_to<sink_threading>;
    };

    /// The contract `S` declares. Only valid for a type that declares one.
    template <declares_threading S>
    inline constexpr sink_threading sink_threading_of = sink_traits<S>::threading;

    /**
     * @concept writable_sink
     * @brief Anything shaped like a sink, whether or not it has stated its contract.
     * @tparam S The candidate sink type.
     * @details The router's registration functions take this rather than `log_sink`, so that a sink
     * missing its contract fails on a `static_assert` that says what to add instead of on an
     * unsatisfied constraint that says a `write` is missing when it is not.
     */
    template <typename S>
    concept writable_sink = requires(S sink, const log_event &ev) {
        { sink.write(ev) } -> std::same_as<void>;
    };

    /**
     * @concept log_sink
     * @brief A type the router can write events to: shaped like a sink, and clear about its threads.
     * @tparam S The candidate sink type.
     */
    template <typename S>
    concept log_sink = writable_sink<S> && declares_threading<S>;

    /**
     * @concept flushable_sink
     * @brief A sink that also buffers, and so can be asked to flush.
     * @tparam S The candidate sink type.
     */
    template <typename S>
    concept flushable_sink = requires(S sink) {
        { sink.flush() };
    };

    /**
     * @concept text_sink
     * @brief A sink that renders events as lines of text and will take them from a shared cache.
     * @tparam S The candidate sink type.
     * @details Opting in costs one overload - `write(event, cache)` alongside `write(event)` - and
     * saves formatting the same event once per text sink. The router prefers this overload when a
     * sink has it. The line handed over is borrowed for the length of the call: a sink that keeps it
     * copies it.
     */
    template <typename S>
    concept text_sink = log_sink<S> && requires(S sink, const log_event &ev, line_cache &cache) {
        { sink.write(ev, cache) } -> std::same_as<void>;
    };

    namespace detail
    {
        /**
         * @brief The contract `S` declares, or `reentrant` when it declares none.
         * @details Only so that the diagnostics in `router::add_sink` can name a contract without
         * hard-erroring on the sink that has not got one. Never used to actually run a sink: a sink
         * with no contract does not get that far.
         */
        template <typename S>
        inline constexpr sink_threading threading_or_reentrant = [] {
            if constexpr (declares_threading<S>)
                return sink_traits<S>::threading;
            else
                return sink_threading::reentrant;
        }();

        /// The message a sink that never stated its contract fails on.
        template <typename S>
        inline constexpr bool undeclared_sink_diagnostic = declares_threading<S>;

        /// The message an `owner_thread` sink handed straight to the router fails on.
        template <typename S>
        inline constexpr bool direct_sink_diagnostic =
            threading_or_reentrant<S> != sink_threading::owner_thread;
    }

    // ---------------------------------------------------------------------
    // Registration
    // ---------------------------------------------------------------------

    /// Identifies one registration of a sink with a router.
    using sink_id = std::uint64_t;

    /// The id no registration ever has; returned when a sink could not be added.
    inline constexpr sink_id no_sink = 0;

    class router;

    /**
     * @class scoped_sink
     * @brief Removes a sink from its router when destroyed.
     * @details Destruction waits for any dispatch still inside the sink, so a panel that holds one
     * of these is finished being logged to by the time its own destructor body runs. That is the
     * whole reason it exists: a sink whose lifetime is somebody else's - a panel that comes and
     * goes - has to stop being reachable before it stops existing, and `router::remove_sink` on its
     * own leaves getting the order right to the caller.
     *
     * Built by `router::add_scoped_sink` and `router::emplace_scoped_sink`, or from an id directly.
     * The router must outlive the guard.
     */
    class scoped_sink
    {
    public:
        scoped_sink() = default;

        /// Takes over an id already returned by `add_sink`.
        scoped_sink(router &owner, sink_id id) noexcept : router_(&owner), id_(id) {}

        scoped_sink(const scoped_sink &) = delete;
        scoped_sink &operator=(const scoped_sink &) = delete;
        scoped_sink(scoped_sink &&other) noexcept;
        scoped_sink &operator=(scoped_sink &&other) noexcept;
        ~scoped_sink();

        /// Removes the sink now, if it has not been removed already.
        void reset() noexcept;

        /// Gives up the registration without removing it; the caller takes the id back.
        [[nodiscard]] sink_id release() noexcept;

        [[nodiscard]] sink_id id() const noexcept { return id_; }
        [[nodiscard]] explicit operator bool() const noexcept { return id_ != no_sink; }

    private:
        router *router_ = nullptr;
        sink_id id_ = no_sink;
    };

} // namespace catalyst::logging
