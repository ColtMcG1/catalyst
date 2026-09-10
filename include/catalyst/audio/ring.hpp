/**
 * @file ring.hpp
 * @brief The lock-free bounded queue one thread writes and one thread reads, which is how anything
 * reaches the render thread without a mutex.
 * @details block.hpp states the rule this header exists to make keepable: *a renderer that needs to
 * talk to the rest of the program does it through a lock-free queue it owns*. This is that queue.
 *
 * Single producer, single consumer, and both halves are the point. A ring with one writer and one
 * reader needs no compare-exchange and no retry loop: the producer owns the write index and the
 * consumer owns the read index, each publishes its own with a release store, and each reads the
 * other's with an acquire load. Every operation completes in a bounded number of instructions with
 * no possibility of spinning, which is what "real-time safe" has to mean on a thread that has
 * roughly a millisecond to produce a block. Two threads pushing, or two popping, is undefined -
 * that is the trade that buys the guarantee.
 *
 * Three consequences worth stating, because each is a decision and not an accident:
 *
 * **Capacity is fixed at construction.** The buffer is allocated once, by whichever thread built
 * the ring, and never again. A ring that grew would allocate on whichever thread happened to fill
 * it, which on the render thread is the failure this type exists to prevent.
 *
 * **A full ring refuses the new item rather than dropping the old one.** @ref notice_queue drops
 * the oldest, because for device topology the newest state is the only one worth having. A ring
 * carrying commands is the opposite: applying "set gain" while silently discarding the "stop voice"
 * that preceded it produces a state nothing asked for. Refusing is visible - `try_push` returns
 * false - and the caller decides.
 *
 * **Nothing here throws.** Every operation after construction is `noexcept`, and the element type
 * must be nothrow-movable and nothrow-destructible, so there is no path from a queue operation to
 * an exception on a thread that cannot unwind.
 *
 * The type is generic because more than one thing needs it - commands going one way, retired
 * buffers coming back the other - and it lives in `catalyst::audio` because audio is what needs it
 * today. Nothing about it is audio-specific; if a second module ever wants it, it moves to
 * `catalyst::utils` unchanged.
 * License: CDDL-1.0 (see LICENSE).
 */

#pragma once

#include <atomic>
#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>

namespace catalyst::audio
{

    /**
     * @brief The separation, in bytes, that keeps two hot atomics off one cache line.
     * @details Spelled out rather than taken from `std::hardware_destructive_interference_size`,
     * which is an ABI-visible constant that some toolchains warn about using in a header and whose
     * value differs between them. 64 is right for every x86-64 and AArch64 target this project
     * builds for, and being wrong here costs a little padding, never correctness.
     */
    inline constexpr std::size_t cache_line_bytes = 64;

    /**
     * @concept ring_element
     * @brief What a @ref spsc_ring can carry: something that can be moved into and out of a slot,
     * and destroyed, without ever throwing.
     * @tparam T The candidate element type.
     * @details Move-only types qualify and are expected - a ring is a hand-off, so a `unique_ptr`
     * crossing it is the ownership transfer spelled correctly. What does *not* qualify is anything
     * whose move or destructor can throw, because both happen on threads with nothing to catch.
     */
    template <typename T>
    concept ring_element =
        std::is_nothrow_move_constructible_v<T> &&
        std::is_nothrow_move_assignable_v<T> &&
        std::is_nothrow_destructible_v<T>;

    /**
     * @class spsc_ring
     * @brief A bounded lock-free queue for exactly one producer thread and one consumer thread.
     * @tparam T The element type; see @ref ring_element.
     * @details Usage is two calls and a rule:
     *
     *     audio::spsc_ring<job> jobs(256);
     *
     *     // producer thread
     *     if (!jobs.try_push(job{...}))
     *         ++dropped;                       // the ring is full; the caller decides what that means
     *
     *     // consumer thread
     *     job next;
     *     while (jobs.try_pop(next))
     *         apply(next);
     *
     * The rule is that "the producer thread" and "the consumer thread" are each one specific
     * thread, for the life of the ring. Which thread pushes may differ from which pops - that is
     * the whole idea - but two threads doing the same one is undefined behaviour, not a race that
     * costs an element.
     *
     * Neither copyable nor movable: it contains the atomics two threads are reading, so relocating
     * one while it is in use would move the memory out from under them. Hold it by value in the
     * object that owns both ends, or behind a `unique_ptr` if it must be handed around.
     */
    template <ring_element T>
    class spsc_ring
    {
    public:
        /** @brief The element type carried. */
        using value_type = T;

        /**
         * @brief Allocates room for at least @p capacity elements, rounded up to a power of two.
         * @param capacity The smallest number of elements the ring must hold. Zero is treated as
         * one, so a ring is never a hole that silently swallows everything.
         * @throws std::bad_alloc If the storage cannot be allocated - which is why this is the only
         * operation on the type that is not `noexcept`. Construct rings during setup.
         * @details Rounding to a power of two is what lets the index wrap with a mask instead of a
         * modulo, so `try_push` and `try_pop` contain no division on the real-time path.
         */
        explicit spsc_ring(std::size_t capacity)
            : capacity_(std::bit_ceil(capacity == 0 ? std::size_t{1} : capacity)),
              mask_(capacity_ - 1),
              slots_(std::make_unique_for_overwrite<slot[]>(capacity_))
        {
        }

        spsc_ring(const spsc_ring &) = delete;
        spsc_ring &operator=(const spsc_ring &) = delete;
        spsc_ring(spsc_ring &&) = delete;
        spsc_ring &operator=(spsc_ring &&) = delete;

        /** @brief Destroys anything still queued. No thread may be using the ring by this point. */
        ~spsc_ring()
        {
            const std::uint64_t head = head_.load(std::memory_order_relaxed);
            for (std::uint64_t i = tail_.load(std::memory_order_relaxed); i != head; ++i)
                std::destroy_at(at(i));
        }

        /**
         * @brief Producer thread. Constructs an element in place at the back of the ring.
         * @tparam Args Constructor arguments for @ref value_type; construction must be `noexcept`.
         * @return True if it was queued, false if the ring is full - in which case nothing was
         * constructed and the arguments are untouched.
         */
        template <typename... Args>
            requires std::is_nothrow_constructible_v<T, Args...>
        [[nodiscard]] bool try_emplace(Args &&...args) noexcept
        {
            const std::uint64_t head = head_.load(std::memory_order_relaxed);

            // The consumer's index is read only when the ring looks full from the cached one, so
            // the common push touches no cache line the other thread writes.
            if (head - cached_tail_ >= capacity_)
            {
                cached_tail_ = tail_.load(std::memory_order_acquire);
                if (head - cached_tail_ >= capacity_)
                    return false;
            }

            std::construct_at(slot_pointer(head), std::forward<Args>(args)...);

            // Release: the element is fully constructed before the consumer can see the index that
            // makes it visible.
            head_.store(head + 1, std::memory_order_release);
            return true;
        }

        /**
         * @brief Producer thread. Moves @p value to the back of the ring.
         * @param value The element, taken by value so that a caller may pass either a copy or a
         * `std::move`, and a move-only type works without a second overload.
         * @return True if it was queued; false if the ring is full, leaving @p value destroyed with
         * the call - if the caller needs it back on failure, use @ref try_emplace.
         */
        [[nodiscard]] bool try_push(T value) noexcept
        {
            return try_emplace(std::move(value));
        }

        /**
         * @brief Consumer thread. Moves the front element into @p out.
         * @param out Assigned only when this returns true; left alone when the ring is empty.
         * @return True if an element was taken.
         */
        [[nodiscard]] bool try_pop(T &out) noexcept
        {
            const std::uint64_t tail = tail_.load(std::memory_order_relaxed);

            if (tail == cached_head_)
            {
                cached_head_ = head_.load(std::memory_order_acquire);
                if (tail == cached_head_)
                    return false;
            }

            T *front = at(tail);
            out = std::move(*front);
            std::destroy_at(front);

            // Release: the slot is free only after the move and the destructor have happened.
            tail_.store(tail + 1, std::memory_order_release);
            return true;
        }

        /**
         * @brief Consumer thread. Invokes @p fn on the front element in place, then removes it.
         * @tparam F A callable taking `T &`, which must be `noexcept`.
         * @return True if an element was consumed.
         * @details The point is what it avoids: @ref try_pop moves the element into the caller's
         * frame first, which for a large element is a copy the consumer did not need. Here the
         * callable sees the element where it already is.
         *
         * @p fn must not push to or pop from this ring - the slot is still occupied while it runs.
         */
        template <typename F>
            requires std::is_nothrow_invocable_r_v<void, F &, T &>
        [[nodiscard]] bool try_consume(F &&fn) noexcept
        {
            const std::uint64_t tail = tail_.load(std::memory_order_relaxed);

            if (tail == cached_head_)
            {
                cached_head_ = head_.load(std::memory_order_acquire);
                if (tail == cached_head_)
                    return false;
            }

            T *front = at(tail);
            fn(*front);
            std::destroy_at(front);

            tail_.store(tail + 1, std::memory_order_release);
            return true;
        }

        /**
         * @brief How many elements are queued.
         * @details Exact on either participating thread for what *that* thread can act on: the
         * producer can only ever see fewer than are really there, the consumer only more. Both are
         * the safe direction. Treat it as a diagnostic rather than a decision.
         */
        [[nodiscard]] std::size_t size() const noexcept
        {
            const std::uint64_t head = head_.load(std::memory_order_acquire);
            const std::uint64_t tail = tail_.load(std::memory_order_acquire);
            return static_cast<std::size_t>(head - tail);
        }

        /** @brief True when nothing is queued. See @ref size for what "when" means here. */
        [[nodiscard]] bool empty() const noexcept { return size() == 0; }

        /** @brief How many elements the ring holds: the constructor argument, rounded up. */
        [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }

    private:
        /** @brief One element's worth of uninitialised, correctly aligned storage. */
        struct alignas(T) slot
        {
            std::byte bytes[sizeof(T)];
        };

        /** @brief The slot a monotonic index names, without constructing anything. */
        [[nodiscard]] T *slot_pointer(std::uint64_t index) noexcept
        {
            return reinterpret_cast<T *>(slots_[static_cast<std::size_t>(index & mask_)].bytes);
        }

        /** @brief The live element a monotonic index names. Only valid for occupied slots. */
        [[nodiscard]] T *at(std::uint64_t index) noexcept { return std::launder(slot_pointer(index)); }

        // Indices are monotonic and masked only when addressing a slot, so `head - tail` is the
        // count and full is distinguishable from empty without wasting a slot. At one push per
        // nanosecond a 64-bit index takes five centuries to wrap.
        alignas(cache_line_bytes) std::atomic<std::uint64_t> head_{0};
        std::uint64_t cached_tail_ = 0; ///< Producer's private, stale view of `tail_`.

        alignas(cache_line_bytes) std::atomic<std::uint64_t> tail_{0};
        std::uint64_t cached_head_ = 0; ///< Consumer's private, stale view of `head_`.

        alignas(cache_line_bytes) const std::size_t capacity_;
        const std::uint64_t mask_;
        const std::unique_ptr<slot[]> slots_;
    };

} // namespace catalyst::audio
