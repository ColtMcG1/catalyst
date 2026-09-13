/**
 * @file test_command.cpp
 * @brief Exercises the lock-free hand-off Tier 2 is built on: `spsc_ring`, `command` and
 * `command_ring`.
 * @details These are the pieces every later voice and bus API will reach the render thread through,
 * so their failures are the expensive kind - an off-by-one in the wrap-around is a command applied
 * twice or never, and a full ring that drops silently is a voice that keeps playing after the game
 * stopped it. Most of what follows is single-threaded and therefore deterministic; the last two
 * cases run a real producer and consumer against each other, because ordering and wrap-around only
 * mean something once two threads are looking at the indices at the same time.
 *
 * What no test here can prove is the absence of a race - a passing run under a strong memory model
 * says less than it looks like. What it does catch is every way of getting the arithmetic and the
 * ownership wrong, which is where the bugs in a queue like this actually live.
 * License: MIT (see LICENSE).
 */

#include "../test_common.hpp"

#include <catalyst/audio/audio.hpp>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

using namespace catalyst;
using namespace catalyst::audio;

namespace
{

    // ------------------------------------------------------------------------------------------------------------------
    // spsc_ring
    // ------------------------------------------------------------------------------------------------------------------

    /// Capacity is rounded up so the index can wrap with a mask, and is never zero.
    void test_capacity_is_rounded_up()
    {
        CT_REQUIRE(spsc_ring<int>(1).capacity() == 1);
        CT_REQUIRE(spsc_ring<int>(4).capacity() == 4);
        CT_REQUIRE(spsc_ring<int>(5).capacity() == 8);
        CT_REQUIRE(spsc_ring<int>(1000).capacity() == 1024);

        // Zero would be a ring that accepts nothing and reports no error; one is the honest floor.
        CT_REQUIRE(spsc_ring<int>(0).capacity() == 1);
    }

    /// Every slot is usable - the monotonic indices distinguish full from empty without a spare.
    void test_ring_fills_to_capacity_and_refuses_more()
    {
        spsc_ring<int> ring(4);
        CT_REQUIRE(ring.empty());

        for (int i = 0; i < 4; ++i)
            CT_REQUIRE(ring.try_push(i));

        CT_REQUIRE(ring.size() == 4);
        CT_REQUIRE(!ring.try_push(99));

        // Refusing must not disturb what is queued: the ring still holds 0..3, in order.
        int value = -1;
        for (int i = 0; i < 4; ++i)
        {
            CT_REQUIRE(ring.try_pop(value));
            CT_REQUIRE(value == i);
        }

        CT_REQUIRE(ring.empty());
    }

    void test_pop_from_empty_leaves_the_target_alone()
    {
        spsc_ring<int> ring(4);

        int value = 1234;
        CT_REQUIRE(!ring.try_pop(value));
        CT_REQUIRE(value == 1234);
    }

    /// Many more items than slots, so the mask wraps repeatedly and every wrap is checked.
    void test_ring_wraps_without_losing_order()
    {
        spsc_ring<int> ring(4);

        for (int i = 0; i < 1000; ++i)
        {
            CT_REQUIRE(ring.try_push(i));

            int value = -1;
            CT_REQUIRE(ring.try_pop(value));
            CT_REQUIRE(value == i);
        }

        CT_REQUIRE(ring.empty());
    }

    /// A ring is a hand-off, so the thing handed over may be one that cannot be copied.
    void test_ring_carries_move_only_elements()
    {
        spsc_ring<std::unique_ptr<int>> ring(2);

        CT_REQUIRE(ring.try_push(std::make_unique<int>(7)));

        std::unique_ptr<int> taken;
        CT_REQUIRE(ring.try_pop(taken));
        CT_REQUIRE(taken != nullptr);
        CT_REQUIRE(*taken == 7);
    }

    /// Counts its own instances, to prove the ring destroys what it still holds.
    struct counted
    {
        static inline int alive = 0;

        counted() noexcept { ++alive; }
        counted(const counted &) noexcept { ++alive; }
        counted(counted &&) noexcept { ++alive; }
        counted &operator=(const counted &) noexcept { return *this; }
        counted &operator=(counted &&) noexcept { return *this; }
        ~counted() { --alive; }
    };

    void test_ring_destroys_what_it_still_holds()
    {
        counted::alive = 0;

        {
            spsc_ring<counted> ring(8);
            for (int i = 0; i < 5; ++i)
                CT_REQUIRE(ring.try_push(counted{}));

            CT_REQUIRE(counted::alive == 5);
        }

        // Three would survive a destructor that only cleared the indices, and five would leak from
        // one that destroyed nothing.
        CT_REQUIRE(counted::alive == 0);
    }

    /// A popped slot is destroyed, not left occupied - otherwise a wrapped ring leaks every lap.
    void test_popping_destroys_the_element()
    {
        counted::alive = 0;

        spsc_ring<counted> ring(2);
        CT_REQUIRE(ring.try_push(counted{}));
        CT_REQUIRE(counted::alive == 1);

        {
            counted taken;
            CT_REQUIRE(counted::alive == 2);
            CT_REQUIRE(ring.try_pop(taken));
            CT_REQUIRE(counted::alive == 1); // the slot's copy is gone; `taken` is what is left
        }

        CT_REQUIRE(counted::alive == 0);
    }

    /// `try_consume` sees the element where it lies, then frees the slot.
    void test_consume_runs_in_place_and_removes()
    {
        spsc_ring<int> ring(4);
        CT_REQUIRE(ring.try_push(41));

        int seen = 0;
        CT_REQUIRE(ring.try_consume([&seen](int &value) noexcept { seen = value + 1; }));
        CT_REQUIRE(seen == 42);
        CT_REQUIRE(ring.empty());

        // Nothing to consume, so the callable must not run.
        int calls = 0;
        CT_REQUIRE(!ring.try_consume([&calls](int &) noexcept { ++calls; }));
        CT_REQUIRE(calls == 0);
    }

    // ------------------------------------------------------------------------------------------------------------------
    // command
    // ------------------------------------------------------------------------------------------------------------------

    void test_empty_command_does_nothing()
    {
        command work;
        CT_REQUIRE(!work.valid());
        CT_REQUIRE(!static_cast<bool>(work));
        work(); // A default-constructed command is invocable and inert, not a null-pointer call.
    }

    /// A command is a snapshot: it copies its captures at the post, not at the run.
    void test_command_copies_its_captures()
    {
        int gain = 3;
        int applied = 0;

        command work([&applied, gain]() noexcept { applied = gain; });

        gain = 9; // Changed after the capture, so a command that read it late would apply 9.
        work();

        CT_REQUIRE(applied == 3);
    }

    void test_command_is_one_cache_line()
    {
        CT_REQUIRE(sizeof(command) == cache_line_bytes);
        CT_REQUIRE(command::payload_bytes >= 4 * sizeof(void *));
    }

    /// The four ways a capture can break the render thread, each rejected at compile time.
    void test_the_concept_rejects_what_it_should()
    {
        const auto throwing = []() {};
        static_assert(!commandable<decltype(throwing)>, "a command that can throw is not a command");

        struct big
        {
            char pad[128];
        };
        const auto oversized = [payload = big{}]() noexcept { (void)payload; };
        static_assert(!commandable<decltype(oversized)>, "a capture must fit the preallocated slot");

        const auto owning = [held = std::make_unique<int>(1)]() noexcept { (void)held; };
        static_assert(!commandable<decltype(owning)>, "a capture that frees must not reach the ring");

        // And the shape that is meant to work: handles and numbers, by value.
        const auto ordinary = [voice = 3u, gain = 0.5f]() noexcept { (void)voice; (void)gain; };
        static_assert(commandable<decltype(ordinary)>, "handles and floats are the intended capture");
    }

    // ------------------------------------------------------------------------------------------------------------------
    // command_ring
    // ------------------------------------------------------------------------------------------------------------------

    void test_commands_run_in_the_order_they_were_posted()
    {
        command_ring commands(16);
        std::vector<int> applied;

        for (int i = 0; i < 5; ++i)
            CT_REQUIRE(commands.post([&applied, i]() noexcept { applied.push_back(i); }));

        CT_REQUIRE(commands.pending() == 5);
        CT_REQUIRE(commands.executed() == 0);

        CT_REQUIRE(commands.execute() == 5);

        CT_REQUIRE(applied.size() == 5);
        for (int i = 0; i < 5; ++i)
            CT_REQUIRE(applied[static_cast<std::size_t>(i)] == i);

        CT_REQUIRE(commands.posted() == 5);
        CT_REQUIRE(commands.executed() == 5);
        CT_REQUIRE(commands.refused() == 0);
        CT_REQUIRE(commands.pending() == 0);

        // Executing an empty ring is not an error and runs nothing.
        CT_REQUIRE(commands.execute() == 0);
    }

    /// A full ring refuses the newest and says so, rather than dropping an earlier command.
    void test_a_full_ring_refuses_the_newest_and_counts_it()
    {
        command_ring commands(4);
        std::vector<int> applied;

        for (int i = 0; i < 4; ++i)
            CT_REQUIRE(commands.post([&applied, i]() noexcept { applied.push_back(i); }));

        CT_REQUIRE(!commands.post([&applied]() noexcept { applied.push_back(99); }));
        CT_REQUIRE(!commands.post([&applied]() noexcept { applied.push_back(98); }));

        CT_REQUIRE(commands.posted() == 4);
        CT_REQUIRE(commands.refused() == 2);

        CT_REQUIRE(commands.execute() == 4);
        CT_REQUIRE(applied.size() == 4);

        // The survivors are the *first* four. Keeping the newest would apply a "set gain" whose
        // "start voice" was thrown away.
        for (int i = 0; i < 4; ++i)
            CT_REQUIRE(applied[static_cast<std::size_t>(i)] == i);
    }

    /// The budget is what keeps a burst on the game thread from becoming an xrun on the audio one.
    void test_execute_honours_its_budget()
    {
        command_ring commands(16);
        int applied = 0;

        for (int i = 0; i < 5; ++i)
            CT_REQUIRE(commands.post([&applied]() noexcept { ++applied; }));

        CT_REQUIRE(commands.execute(2) == 2);
        CT_REQUIRE(applied == 2);
        CT_REQUIRE(commands.pending() == 3);

        CT_REQUIRE(commands.execute(0) == 0);
        CT_REQUIRE(applied == 2);

        CT_REQUIRE(commands.execute(100) == 3);
        CT_REQUIRE(applied == 5);
        CT_REQUIRE(commands.pending() == 0);
    }

    /// The documented way to free something the render thread is done with: a second ring, back.
    void test_ownership_travels_back_on_a_second_ring()
    {
        counted::alive = 0;

        command_ring to_audio(8);
        spsc_ring<std::unique_ptr<counted>> retired(8);

        auto *live = new counted{};
        CT_REQUIRE(counted::alive == 1);

        // The render thread replaces what it was using and hands the old one back rather than
        // deleting it, which is the whole point - `delete` on the render thread is an allocation.
        CT_REQUIRE(to_audio.post([&retired, live]() noexcept
                                 { (void)retired.try_push(std::unique_ptr<counted>(live)); }));
        CT_REQUIRE(to_audio.execute() == 1);
        CT_REQUIRE(counted::alive == 1); // still alive: nothing was destroyed on the audio side

        std::unique_ptr<counted> collected;
        CT_REQUIRE(retired.try_pop(collected));
        CT_REQUIRE(collected.get() == live);

        collected.reset(); // freed here, on the thread that is allowed to free
        CT_REQUIRE(counted::alive == 0);
    }

    // ------------------------------------------------------------------------------------------------------------------
    // Two threads
    // ------------------------------------------------------------------------------------------------------------------

    /// One producer, one consumer, a ring far smaller than the traffic: every wrap is contended.
    void test_ring_survives_a_real_producer_and_consumer()
    {
        constexpr int total = 200'000;

        spsc_ring<int> ring(64);
        std::atomic<bool> failed{false};

        std::thread consumer([&ring, &failed]
                             {
            int expected = 0;
            int value = 0;
            while (expected < total)
            {
                if (ring.try_pop(value))
                {
                    if (value != expected)   // a lost, duplicated or reordered element
                        failed.store(true, std::memory_order_relaxed);
                    ++expected;
                }
                else
                {
                    std::this_thread::yield();
                }
            } });

        for (int i = 0; i < total; ++i)
        {
            while (!ring.try_push(i))
                std::this_thread::yield();
        }

        consumer.join();

        CT_REQUIRE(!failed.load(std::memory_order_relaxed));
        CT_REQUIRE(ring.empty());
    }

    /// The same, one layer up: commands posted from one thread run, in order, on the other.
    void test_commands_cross_threads_in_order()
    {
        constexpr int total = 50'000;

        command_ring commands(64);
        std::atomic<bool> failed{false};
        std::atomic<int> executed{0};

        // Owned by the consumer thread alone, which is the arrangement a mixer has: the state a
        // command touches belongs to the thread that runs the command.
        int applied = 0;

        std::thread consumer([&commands, &executed, &failed, &applied]
                             {
            while (executed.load(std::memory_order_relaxed) < total)
            {
                const std::size_t ran = commands.execute(16);   // a block's worth at a time
                if (ran == 0)
                    std::this_thread::yield();
                else
                    executed.fetch_add(static_cast<int>(ran), std::memory_order_relaxed);
            }

            if (applied != total)
                failed.store(true, std::memory_order_relaxed);
        });

        std::uint64_t retries = 0;
        for (int i = 0; i < total; ++i)
        {
            while (!commands.post([&applied, i]() noexcept
                                  {
                                      if (applied != i)   // out of order, or one ran twice
                                          applied = -1;
                                      else
                                          ++applied;
                                  }))
            {
                ++retries;
                std::this_thread::yield();
            }
        }

        consumer.join();

        CT_REQUIRE(!failed.load(std::memory_order_relaxed));

        // A refusal is not a loss here, because this producer posts again - but the two counts must
        // agree, or the ring refused a command it had room for.
        CT_REQUIRE(commands.refused() == retries);
        CT_REQUIRE(commands.posted() == static_cast<std::uint64_t>(total));
        CT_REQUIRE(commands.executed() == static_cast<std::uint64_t>(total));
    }

} // namespace

int main()
{
    test_capacity_is_rounded_up();
    test_ring_fills_to_capacity_and_refuses_more();
    test_pop_from_empty_leaves_the_target_alone();
    test_ring_wraps_without_losing_order();
    test_ring_carries_move_only_elements();
    test_ring_destroys_what_it_still_holds();
    test_popping_destroys_the_element();
    test_consume_runs_in_place_and_removes();

    test_empty_command_does_nothing();
    test_command_copies_its_captures();
    test_command_is_one_cache_line();
    test_the_concept_rejects_what_it_should();

    test_commands_run_in_the_order_they_were_posted();
    test_a_full_ring_refuses_the_newest_and_counts_it();
    test_execute_honours_its_budget();
    test_ownership_travels_back_on_a_second_ring();

    test_ring_survives_a_real_producer_and_consumer();
    test_commands_cross_threads_in_order();

    return 0;
}
