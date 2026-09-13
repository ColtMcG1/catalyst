/**
 * @file command.hpp
 * @brief A change to be applied on the render thread, and the ring that carries a queue of them
 * there from the thread that decided on it.
 * @details A game thread cannot touch what a renderer is reading. Not because of a convention, but
 * because the only tools that would make it safe - a mutex, a `shared_ptr` copy, an allocation -
 * are the three things block.hpp forbids on the render thread. So the change travels instead: the
 * game thread describes what it wants done, the render thread does it at a moment of its choosing,
 * between blocks, where nothing is half-written.
 *
 * That is what a @ref command is. It captures its arguments by value into a fixed-size payload,
 * crosses an @ref spsc_ring, and runs on the other side:
 *
 *     // game thread
 *     commands.post([&mixer, voice, gain]() noexcept { mixer.set_gain(voice, gain); });
 *
 *     // render thread, at the top of the block
 *     commands.execute();
 *
 * **A command owns its captures; @ref renderer does not.** The two look alike and are opposites, so
 * it is worth being explicit. `renderer` refers to a callable that must outlive the stream, and
 * binding a temporary to it is a compile error. A command is posted, sits in a ring for an unknown
 * number of milliseconds and runs later - so it copies. That is exactly why a temporary lambda is
 * the *right* way to write one, and why the capture list may not contain a reference to anything
 * that could go away first.
 *
 * **The payload is small, trivially copyable and trivially destructible, and that is enforced.**
 * Small because a ring of commands is preallocated and a command is copied twice; trivially
 * copyable because it crosses to another thread as bytes; trivially destructible because the render
 * thread runs it and then forgets it - running a destructor there is how a `shared_ptr` capture
 * ends up calling `free` at 48 kHz. A capture that does not fit these rules fails to compile, which
 * is the point: the alternative is a click nobody can reproduce.
 *
 * **Ownership travels back the same way.** When a command hands the render thread something to
 * replace - a buffer, a graph node - the old one must not be freed there. It goes into a second
 * ring pointing the other way, and the game thread drains that one and destroys what it finds. The
 * two directions are two @ref command_ring objects; there is no separate type for the return path,
 * because it is the same mechanism with the threads swapped.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/audio/ring.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <type_traits>

namespace catalyst::audio
{

    /**
     * @brief How many bytes of captured state a @ref command can carry.
     * @details Chosen so that a command is exactly one @ref cache_line_bytes wide once the function
     * pointer is added. It is enough for a handle and several floats - `[voice, gain, pan, when]` -
     * which is the shape of nearly every message a game sends a mixer. A command that needs more
     * than this is usually one that should have posted a pointer to something built in advance.
     */
    inline constexpr std::size_t command_payload_bytes = cache_line_bytes - sizeof(void (*)());

    /**
     * @concept commandable
     * @brief A callable that may be posted to a @ref command_ring: `noexcept`, argument-free, and
     * small, trivially copyable and trivially destructible.
     * @tparam F The candidate type, usually a lambda.
     * @details Each clause rules out a specific way of breaking the render thread, so a failure to
     * satisfy this is worth reading rather than working around:
     *
     * - not `noexcept`: there is nothing above a driver callback to catch what it throws.
     * - not trivially copyable: it cannot be moved to another thread as bytes.
     * - not trivially destructible: destroying it on the render thread would run arbitrary code -
     *   a `shared_ptr` release, a `std::string` free - under a deadline.
     * - too large: it would not fit the preallocated slot, and growing the slot slows every
     *   command to pay for one.
     *
     * A lambda capturing handles, numbers, and raw pointers to things that outlive the post
     * satisfies all four. One capturing a `std::string`, a `std::function` or a `shared_ptr` does
     * not - build the value first and post a pointer to it.
     */
    template <typename F>
    concept commandable =
        std::is_nothrow_invocable_r_v<void, F &> &&
        std::is_trivially_copyable_v<F> &&
        std::is_trivially_destructible_v<F> &&
        sizeof(F) <= command_payload_bytes &&
        alignof(F) <= alignof(std::max_align_t);

    /**
     * @class command
     * @brief One unit of work to run on the render thread, with its arguments copied inside it.
     * @details Fixed size, trivially copyable, and it allocates nothing - so it can be stored in a
     * preallocated ring and handed across a thread boundary as bytes. An empty command does
     * nothing when invoked, which is what a default-constructed one is.
     */
    class command
    {
    public:
        /** @brief How many bytes of captured state fit. See @ref command_payload_bytes. */
        static constexpr std::size_t payload_bytes = command_payload_bytes;

        /** @brief A command that does nothing. */
        command() noexcept = default;

        /**
         * @brief Copies @p callable and its captures into this command.
         * @tparam F Any type satisfying @ref commandable.
         * @param callable The work to do later, taken by value - a temporary lambda is the expected
         * argument, unlike @ref renderer, which refuses one.
         */
        template <commandable F>
        explicit command(F callable) noexcept
            : invoke_([](void *payload) noexcept { (*std::launder(static_cast<F *>(payload)))(); })
        {
            ::new (static_cast<void *>(payload_)) F(callable);
        }

        /** @brief True if this command carries work. */
        [[nodiscard]] bool valid() const noexcept { return invoke_ != nullptr; }

        /** @brief True if this command carries work. */
        [[nodiscard]] explicit operator bool() const noexcept { return valid(); }

        /** @brief Runs it. Real-time thread. A no-op when empty. */
        void operator()() noexcept
        {
            if (invoke_)
                invoke_(static_cast<void *>(payload_));
        }

    private:
        alignas(std::max_align_t) std::byte payload_[payload_bytes]{};
        void (*invoke_)(void *) noexcept = nullptr;
    };

    static_assert(std::is_trivially_copyable_v<command>,
                  "a command crosses a thread boundary as bytes");
    static_assert(std::is_trivially_destructible_v<command>,
                  "destroying a command must not run code on the render thread");
    static_assert(sizeof(command) == cache_line_bytes,
                  "a command should be one cache line; adjust command_payload_bytes if this fires");

    /**
     * @class command_ring
     * @brief The one-way channel a thread sends @ref command objects down, and the counters that
     * say whether any were lost.
     * @details One producer and one consumer, as @ref spsc_ring requires: for a mixer that means
     * one ring the game thread posts to and the render thread executes, and a second, separate ring
     * for anything travelling back. Commands execute in the order they were posted, which is why a
     * full ring refuses the newest rather than discarding the oldest - a "stop" that was dropped
     * while the "set gain" after it was applied leaves state nobody asked for.
     *
     * Filling it is still possible, so refusals are counted rather than hidden. @ref refused
     * growing means the consumer is not draining often enough, or the ring is too small for the
     * burst - real problems with a number attached, which is better than the silence they would
     * otherwise make. It counts refusals rather than losses because only the caller knows which it
     * was: a game thread that gives up has lost a command, and one that retries has not.
     */
    class command_ring
    {
    public:
        /** @brief The default depth: a burst of a few hundred messages between two blocks. */
        static constexpr std::size_t default_capacity = 256;

        /**
         * @brief Preallocates room for @p capacity commands, rounded up to a power of two.
         * @param capacity How many commands may be in flight at once.
         * @throws std::bad_alloc If the storage cannot be allocated. Build rings during setup.
         */
        explicit command_ring(std::size_t capacity = default_capacity)
            : ring_(capacity)
        {
        }

        /**
         * @brief Producer thread. Queues @p callable to run on the consumer thread.
         * @tparam F Any type satisfying @ref commandable - see there for what a capture may hold.
         * @param callable The work, copied into the ring along with its captures.
         * @return True if it was queued; false if the ring is full, in which case nothing was
         * queued and @ref refused has been incremented. Whether that is a lost command or a call
         * to make again is the caller's decision - see @ref refused.
         */
        template <commandable F>
        bool post(F callable) noexcept
        {
            if (!ring_.try_emplace(command(callable)))
            {
                refused_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            posted_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        /**
         * @brief Producer thread. Queues an already-built command.
         * @param work The command to run on the consumer thread.
         * @return True if it was queued; false if the ring is full.
         */
        bool post(command work) noexcept
        {
            if (!ring_.try_emplace(work))
            {
                refused_.fetch_add(1, std::memory_order_relaxed);
                return false;
            }

            posted_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }

        /**
         * @brief Consumer thread. Runs the queued commands, oldest first.
         * @param budget The most to run in this call. The default runs everything queued.
         * @return How many ran.
         * @details The budget is the real-time escape hatch. Executing an unbounded queue at the
         * top of a block makes the block's cost depend on how busy the game thread was, so a stream
         * under a tight deadline caps it and lets the rest wait for the next block - late is a
         * choice, and an xrun is not.
         *
         * Even the unbounded default terminates while the producer is still posting: it stops at
         * the depth observed on entry rather than at an empty ring.
         */
        std::size_t execute(std::size_t budget = std::numeric_limits<std::size_t>::max()) noexcept
        {
            const std::size_t queued = ring_.size();
            const std::size_t limit = budget < queued ? budget : queued;

            std::size_t ran = 0;
            while (ran < limit && ring_.try_consume([](command &work) noexcept { work(); }))
                ++ran;

            executed_.fetch_add(ran, std::memory_order_relaxed);
            return ran;
        }

        /** @brief How many commands are waiting. A diagnostic; see @ref spsc_ring::size. */
        [[nodiscard]] std::size_t pending() const noexcept { return ring_.size(); }

        /** @brief How many commands the ring holds at once. */
        [[nodiscard]] std::size_t capacity() const noexcept { return ring_.capacity(); }

        /** @brief How many commands have been accepted for delivery. */
        [[nodiscard]] std::uint64_t posted() const noexcept
        {
            return posted_.load(std::memory_order_relaxed);
        }

        /** @brief How many commands have run. */
        [[nodiscard]] std::uint64_t executed() const noexcept
        {
            return executed_.load(std::memory_order_relaxed);
        }

        /**
         * @brief How many `post` calls found the ring full.
         * @details A refusal is a lost command only if the caller does not post it again, so this
         * counts attempts rather than losses. On the arrangement this type is built for - a game
         * thread that must not wait for the audio thread - they are the same number, and it should
         * stay at zero.
         */
        [[nodiscard]] std::uint64_t refused() const noexcept
        {
            return refused_.load(std::memory_order_relaxed);
        }

    private:
        spsc_ring<command> ring_;

        std::atomic<std::uint64_t> posted_{0};   ///< Producer thread only.
        std::atomic<std::uint64_t> refused_{0};  ///< Producer thread only.
        std::atomic<std::uint64_t> executed_{0}; ///< Consumer thread only.
    };

} // namespace catalyst::audio
