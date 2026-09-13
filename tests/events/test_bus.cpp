#include "../test_common.hpp"

#include <catalyst/events/bus.hpp>

#include <atomic>
#include <barrier>
#include <cstddef>
#include <string>
#include <thread>
#include <vector>

using catalyst::events::bus;
using catalyst::events::next;
using catalyst::events::scoped_token;
using catalyst::events::task;
using catalyst::events::token;

namespace
{
    struct ping
    {
        int value = 0;
    };

    struct pong
    {
    };

    // An event with a static tag: the bus keys on the tag rather than on
    // typeid, which is the only difference a consumer can observe.
    struct tagged_ping : catalyst::events::tagged<7>
    {
        int value = 0;
    };

    int g_free_function_calls = 0;
    void free_handler(const ping &) { ++g_free_function_calls; }

    // ---------------------------------------------------------------------
    // Listeners
    // ---------------------------------------------------------------------

    void test_basic_delivery_and_removal()
    {
        bus b;
        int calls = 0;
        int last = 0;

        token t = b.add_listener<ping>([&](const ping &e)
                                       {
                                           ++calls;
                                           last = e.value;
                                       });

        CT_REQUIRE(t.valid());

        b.dispatch(ping{42});
        CT_REQUIRE(calls == 1);
        CT_REQUIRE(last == 42);

        b.dispatch(ping{7});
        CT_REQUIRE(calls == 2);
        CT_REQUIRE(last == 7);

        t.remove();
        CT_REQUIRE(!t.valid());

        b.dispatch(ping{99});
        CT_REQUIRE(calls == 2); // nothing after removal
        CT_REQUIRE(last == 7);

        // Removing twice is a no-op rather than an error.
        t.remove();
    }

    void test_free_functions_and_many_listeners()
    {
        bus b;
        g_free_function_calls = 0;

        auto t1 = b.add_listener<ping>(&free_handler);
        auto t2 = b.add_listener<ping>(free_handler);

        b.dispatch(ping{1});
        CT_REQUIRE(g_free_function_calls == 2);

        t1.remove();
        b.dispatch(ping{1});
        CT_REQUIRE(g_free_function_calls == 3);

        t2.remove();
    }

    void test_event_types_are_isolated()
    {
        bus b;
        int pings = 0;
        int pongs = 0;
        int tagged = 0;

        auto t1 = b.add_listener<ping>([&](const ping &) { ++pings; });
        auto t2 = b.add_listener<pong>([&](const pong &) { ++pongs; });
        auto t3 = b.add_listener<tagged_ping>([&](const tagged_ping &) { ++tagged; });

        b.dispatch(ping{});
        CT_REQUIRE(pings == 1);
        CT_REQUIRE(pongs == 0);
        CT_REQUIRE(tagged == 0);

        b.dispatch(pong{});
        CT_REQUIRE(pings == 1);
        CT_REQUIRE(pongs == 1);
        CT_REQUIRE(tagged == 0);

        // A tagged event is its own type as far as the bus is concerned, even
        // though it carries the same payload as ping.
        b.dispatch(tagged_ping{});
        CT_REQUIRE(pings == 1);
        CT_REQUIRE(pongs == 1);
        CT_REQUIRE(tagged == 1);
    }

    void test_priority_order_and_stability()
    {
        bus b;
        std::vector<std::string> order;

        // Descending priority; equal priorities keep registration order.
        auto t1 = b.add_listener<ping>([&](const ping &) { order.push_back("low"); }, -10);
        auto t2 = b.add_listener<ping>([&](const ping &) { order.push_back("mid-a"); }, 0);
        auto t3 = b.add_listener<ping>([&](const ping &) { order.push_back("mid-b"); }, 0);
        auto t4 = b.add_listener<ping>([&](const ping &) { order.push_back("high"); }, 10);

        b.dispatch(ping{});

        CT_REQUIRE(order.size() == 4);
        CT_REQUIRE(order[0] == "high");
        CT_REQUIRE(order[1] == "mid-a");
        CT_REQUIRE(order[2] == "mid-b");
        CT_REQUIRE(order[3] == "low");
    }

    void test_registration_during_dispatch_is_seen_by_the_next_one()
    {
        bus b;
        int outer = 0;
        int added = 0;
        scoped_token inner;

        auto t = b.add_listener<ping>([&](const ping &)
                                      {
                                          ++outer;
                                          if (!inner.valid())
                                              inner = b.add_listener<ping>([&](const ping &) { ++added; });
                                      });

        // A dispatch works on the snapshot it took when it started, so the
        // listener registered here does not run until the next one.
        b.dispatch(ping{});
        CT_REQUIRE(outer == 1);
        CT_REQUIRE(added == 0);

        b.dispatch(ping{});
        CT_REQUIRE(outer == 2);
        CT_REQUIRE(added == 1);
    }

    void test_removal_during_dispatch_stops_the_later_listener()
    {
        bus b;
        int high_calls = 0;
        int low_calls = 0;

        token low = b.add_listener<ping>([&](const ping &) { ++low_calls; }, 0);

        // Runs first, and removes the one that would have run after it. A
        // removal takes effect even later in the dispatch that is already
        // running.
        auto high = b.add_listener<ping>([&](const ping &)
                                         {
                                             ++high_calls;
                                             low.remove();
                                         },
                                         10);

        b.dispatch(ping{});
        CT_REQUIRE(high_calls == 1);
        CT_REQUIRE(low_calls == 0);

        high.remove();
    }

    void test_nested_dispatch()
    {
        bus b;
        int pings = 0;
        int pongs = 0;

        auto t1 = b.add_listener<ping>([&](const ping &)
                                       {
                                           ++pings;
                                           if (pings < 3)
                                               b.dispatch(pong{});
                                       });
        auto t2 = b.add_listener<pong>([&](const pong &)
                                       {
                                           ++pongs;
                                           b.dispatch(ping{});
                                       });

        b.dispatch(ping{});
        CT_REQUIRE(pings == 3);
        CT_REQUIRE(pongs == 2);
    }

    void test_exception_from_a_listener_leaves_the_bus_usable()
    {
        bus b;
        int calls = 0;

        auto thrower = b.add_listener<ping>([](const ping &) -> void
                                            { throw std::runtime_error("listener"); },
                                            10);
        auto counter = b.add_listener<ping>([&](const ping &) { ++calls; }, 0);

        bool caught = false;
        try
        {
            b.dispatch(ping{});
        }
        catch (const std::runtime_error &)
        {
            caught = true;
        }

        CT_REQUIRE(caught);

        // The throwing listener aborted that dispatch, but the bus is intact.
        thrower.remove();
        b.dispatch(ping{});
        CT_REQUIRE(calls == 1);
    }

    // ---------------------------------------------------------------------
    // Tokens
    // ---------------------------------------------------------------------

    void test_scoped_token_removes_on_destruction()
    {
        bus b;
        int calls = 0;

        {
            scoped_token t = b.add_listener<ping>([&](const ping &) { ++calls; });
            CT_REQUIRE(t.valid());

            b.dispatch(ping{});
            CT_REQUIRE(calls == 1);
        }

        b.dispatch(ping{});
        CT_REQUIRE(calls == 1);
    }

    void test_scoped_token_move_and_release()
    {
        bus b;
        int calls = 0;

        scoped_token a = b.add_listener<ping>([&](const ping &) { ++calls; });
        scoped_token moved = std::move(a);

        CT_REQUIRE(!a.valid());
        CT_REQUIRE(moved.valid());

        b.dispatch(ping{});
        CT_REQUIRE(calls == 1);

        // release() gives up ownership without unregistering.
        token raw = moved.release();
        CT_REQUIRE(!moved.valid());
        CT_REQUIRE(raw.valid());

        b.dispatch(ping{});
        CT_REQUIRE(calls == 2);

        raw.remove();
        b.dispatch(ping{});
        CT_REQUIRE(calls == 2);
    }

    void test_token_outliving_the_bus_is_simply_invalid()
    {
        token t;
        CT_REQUIRE(!t.valid());

        {
            bus b;
            t = b.add_listener<ping>([](const ping &) {});
            CT_REQUIRE(t.valid());
        }

        CT_REQUIRE(!t.valid());
        t.remove(); // a no-op, not a dangling write
    }

    // ---------------------------------------------------------------------
    // Middleware
    // ---------------------------------------------------------------------

    void test_predicate_middleware_stops_propagation()
    {
        bus b;
        int calls = 0;
        int seen = 0;

        auto listener = b.add_listener<ping>([&](const ping &e)
                                             {
                                                 ++calls;
                                                 seen = e.value;
                                             });

        // Returning false stops the event from reaching the listeners.
        auto filter = b.add_middleware<ping>([](ping &e) { return e.value >= 0; });

        b.dispatch(ping{5});
        CT_REQUIRE(calls == 1);
        CT_REQUIRE(seen == 5);

        b.dispatch(ping{-1});
        CT_REQUIRE(calls == 1); // filtered out
        CT_REQUIRE(seen == 5);

        filter.remove();
        b.dispatch(ping{-1});
        CT_REQUIRE(calls == 2);
        CT_REQUIRE(seen == -1);
    }

    void test_middleware_can_mutate_the_event()
    {
        bus b;
        int seen = 0;

        auto listener = b.add_listener<ping>([&](const ping &e) { seen = e.value; });
        auto doubler = b.add_middleware<ping>([](ping &e)
                                             {
                                                 e.value *= 2;
                                                 return true;
                                             });

        b.dispatch(ping{21});
        CT_REQUIRE(seen == 42);
    }

    void test_chain_middleware_controls_continuation()
    {
        bus b;
        std::vector<std::string> order;

        auto listener = b.add_listener<ping>([&](const ping &) { order.push_back("listener"); });

        auto outer = b.add_middleware<ping>([&](ping &e, const next<ping> &n)
                                            {
                                                order.push_back("outer-before");
                                                n(e);
                                                order.push_back("outer-after");
                                            },
                                            10);

        auto inner = b.add_middleware<ping>([&](ping &e, const next<ping> &n)
                                            {
                                                order.push_back("inner-before");
                                                n(e);
                                                order.push_back("inner-after");
                                            },
                                            0);

        b.dispatch(ping{});

        // The highest-priority middleware is the outermost layer, so it both
        // starts first and finishes last.
        CT_REQUIRE(order.size() == 5);
        CT_REQUIRE(order[0] == "outer-before");
        CT_REQUIRE(order[1] == "inner-before");
        CT_REQUIRE(order[2] == "listener");
        CT_REQUIRE(order[3] == "inner-after");
        CT_REQUIRE(order[4] == "outer-after");
    }

    void test_chain_middleware_that_never_calls_next_short_circuits()
    {
        bus b;
        int calls = 0;
        int inner_runs = 0;

        auto listener = b.add_listener<ping>([&](const ping &) { ++calls; });
        auto inner = b.add_middleware<ping>([&](ping &e, const next<ping> &n)
                                            {
                                                ++inner_runs;
                                                n(e);
                                            },
                                            0);

        // Swallows the event: it never calls next, so nothing below it runs.
        auto blocker = b.add_middleware<ping>([](ping &, const next<ping> &) {}, 10);

        b.dispatch(ping{});
        CT_REQUIRE(calls == 0);
        CT_REQUIRE(inner_runs == 0);

        blocker.remove();
        b.dispatch(ping{});
        CT_REQUIRE(calls == 1);
        CT_REQUIRE(inner_runs == 1);
    }

    void test_chain_middleware_can_dispatch_more_than_once()
    {
        bus b;
        int calls = 0;

        auto listener = b.add_listener<ping>([&](const ping &) { ++calls; });
        auto repeater = b.add_middleware<ping>([](ping &e, const next<ping> &n)
                                               {
                                                   n(e);
                                                   n(e);
                                               });

        b.dispatch(ping{});
        CT_REQUIRE(calls == 2);
    }

    // ---------------------------------------------------------------------
    // Asynchronous dispatch
    // ---------------------------------------------------------------------

    void test_async_listener_and_dispatch()
    {
        bus b;
        int calls = 0;
        int seen = 0;

        auto t = b.add_listener<ping>([&](const ping &e) -> task<void>
                                      {
                                          ++calls;
                                          seen = e.value;
                                          co_return;
                                      });

        b.dispatch_async(ping{11}).get();
        CT_REQUIRE(calls == 1);
        CT_REQUIRE(seen == 11);
    }

    void test_sync_and_async_registrations_are_separate()
    {
        bus b;
        int sync_calls = 0;
        int async_calls = 0;

        auto s = b.add_listener<ping>([&](const ping &) { ++sync_calls; });
        auto a = b.add_listener<ping>([&](const ping &) -> task<void>
                                      {
                                          ++async_calls;
                                          co_return;
                                      });

        b.dispatch(ping{});
        CT_REQUIRE(sync_calls == 1);
        CT_REQUIRE(async_calls == 0);

        b.dispatch_async(ping{}).get();
        CT_REQUIRE(sync_calls == 1);
        CT_REQUIRE(async_calls == 1);
    }

    void test_async_predicate_middleware_stops_propagation()
    {
        bus b;
        int calls = 0;

        auto listener = b.add_listener<ping>([&](const ping &) -> task<void>
                                             {
                                                 ++calls;
                                                 co_return;
                                             });

        auto filter = b.add_async_middleware<ping>([](ping &e) -> task<bool>
                                                   { co_return e.value >= 0; });

        b.dispatch_async(ping{1}).get();
        CT_REQUIRE(calls == 1);

        b.dispatch_async(ping{-1}).get();
        CT_REQUIRE(calls == 1);
    }

    // ---------------------------------------------------------------------
    // Threading
    // ---------------------------------------------------------------------

    void test_concurrent_dispatch()
    {
        bus b;
        std::atomic_int calls{0};

        auto t = b.add_listener<ping>([&](const ping &) { calls.fetch_add(1, std::memory_order_relaxed); });

        constexpr int k_threads = 4;
        constexpr int k_per_thread = 500;

        std::barrier start{k_threads};
        std::vector<std::thread> threads;
        threads.reserve(k_threads);

        for (int i = 0; i < k_threads; ++i)
        {
            threads.emplace_back([&]
                                 {
                                     start.arrive_and_wait();
                                     for (int n = 0; n < k_per_thread; ++n)
                                         b.dispatch(ping{n});
                                 });
        }

        for (auto &th : threads)
            th.join();

        CT_REQUIRE(calls.load() == k_threads * k_per_thread);
    }

    void test_registration_churn_during_concurrent_dispatch()
    {
        bus b;
        std::atomic_int calls{0};
        std::atomic_bool stop{false};

        auto resident = b.add_listener<ping>([&](const ping &) { calls.fetch_add(1, std::memory_order_relaxed); });

        std::thread dispatcher{[&]
                               {
                                   while (!stop.load(std::memory_order_acquire))
                                       b.dispatch(ping{1});
                               }};

        // Dispatching, not merely spawned: an optimised build can finish the whole loop below in
        // less time than it takes Windows to schedule the thread, in which case the dispatcher sees
        // stop on its first check, delivers nothing, and leaves calls at zero. That is a test that
        // overlaps nothing, not a fault in the bus.
        while (calls.load(std::memory_order_relaxed) == 0)
            std::this_thread::yield();

        // Adding and removing under a concurrent dispatch must not corrupt the
        // registry or trip on a callable a dispatch is still holding.
        for (int i = 0; i < 2000; ++i)
        {
            token t = b.add_listener<ping>([&](const ping &) { calls.fetch_add(1, std::memory_order_relaxed); });
            t.remove();
        }

        stop.store(true, std::memory_order_release);
        dispatcher.join();

        CT_REQUIRE(calls.load() > 0);
    }

    void test_removal_from_another_thread_stops_delivery()
    {
        bus b;
        std::atomic_int calls{0};

        token t = b.add_listener<ping>([&](const ping &) { calls.fetch_add(1, std::memory_order_relaxed); });

        b.dispatch(ping{});
        CT_REQUIRE(calls.load() == 1);

        std::thread remover{[&] { t.remove(); }};
        remover.join();

        CT_REQUIRE(!t.valid());

        b.dispatch(ping{});
        CT_REQUIRE(calls.load() == 1);
    }

} // namespace

int main()
{
    test_basic_delivery_and_removal();
    test_free_functions_and_many_listeners();
    test_event_types_are_isolated();
    test_priority_order_and_stability();
    test_registration_during_dispatch_is_seen_by_the_next_one();
    test_removal_during_dispatch_stops_the_later_listener();
    test_nested_dispatch();
    test_exception_from_a_listener_leaves_the_bus_usable();

    test_scoped_token_removes_on_destruction();
    test_scoped_token_move_and_release();
    test_token_outliving_the_bus_is_simply_invalid();

    test_predicate_middleware_stops_propagation();
    test_middleware_can_mutate_the_event();
    test_chain_middleware_controls_continuation();
    test_chain_middleware_that_never_calls_next_short_circuits();
    test_chain_middleware_can_dispatch_more_than_once();

    test_async_listener_and_dispatch();
    test_sync_and_async_registrations_are_separate();
    test_async_predicate_middleware_stops_propagation();

    test_concurrent_dispatch();
    test_registration_churn_during_concurrent_dispatch();
    test_removal_from_another_thread_stops_delivery();
    return 0;
}
