#include <benchmark.hpp>

#include <atomic>
#include <barrier>
#include <chrono>
#include <concepts>
#include <cstddef>
#include <functional>
#include <iomanip>
#include <iostream>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <catalyst/events/bus.hpp>

namespace
{
    struct benchmark_event final
    {
        std::size_t value;
    };

    template <int N>
    struct idle_event final
    {
    };

    std::atomic_size_t handled_events{0u};

    void handle_event(const benchmark_event &event)
    {
        handled_events.fetch_add(event.value, std::memory_order_relaxed);
    }

    void handle_event_thread_local(const benchmark_event &event)
    {
        static thread_local std::size_t local_total = 0u;
        local_total += event.value;
        if (local_total == ~std::size_t{0})
            handled_events.fetch_add(1u, std::memory_order_relaxed);
    }

    catalyst::events::task<void> handle_event_async(const benchmark_event &event)
    {
        handle_event(event);
        co_return;
    }

    template <int... Is>
    void add_idle_listeners(catalyst::events::bus &bus,
                            std::vector<catalyst::events::token> &tokens,
                            int listeners_per_type,
                            std::integer_sequence<int, Is...>)
    {
        ((void)[&]
        {
            for (int listener = 0; listener < listeners_per_type; ++listener)
                tokens.push_back(bus.add_listener<idle_event<Is>>([](const idle_event<Is> &) {}));
        }(), ...);
    }

    template <typename Body>
    void run_parallel(std::string_view name, unsigned threads, std::size_t per_thread, Body &&body)
    {
        using clock = std::chrono::steady_clock;

        std::barrier start_gate{static_cast<std::ptrdiff_t>(threads + 1u)};
        std::vector<std::thread> workers;
        workers.reserve(threads);
        for (unsigned thread = 0u; thread < threads; ++thread)
            workers.emplace_back([&]
            {
                start_gate.arrive_and_wait();
                body(per_thread);
            });

        const auto start = clock::now();
        start_gate.arrive_and_wait();
        for (auto &worker : workers)
            worker.join();

        const auto elapsed = clock::now() - start;
        const auto total = static_cast<double>(per_thread) * threads;
        const auto elapsed_seconds = std::chrono::duration<double>(elapsed).count();

        std::cout << name << '\n'
                  << "  iterations: " << static_cast<std::size_t>(total) << " (" << threads << " threads)\n"
                  << "  total:      " << std::fixed << std::setprecision(3) << (elapsed_seconds * 1'000.0) << " ms\n"
                  << "  average:    " << (std::chrono::duration<double, std::nano>(elapsed).count() / total) << " ns/op\n"
                  << "  throughput: " << (total / elapsed_seconds) << " op/s\n";
    }
} // namespace

int main()
{
    constexpr std::size_t iterations = 1'000'000u;

    catalyst::bench::run("steady_clock::now() (cost of one timestamp)", iterations, []
    {
        handled_events.fetch_add(static_cast<std::size_t>(std::chrono::steady_clock::now().time_since_epoch().count() & 1),
                                 std::memory_order_relaxed);
    });

    {
        catalyst::events::bus bus;
        catalyst::bench::run("bus.dispatch (no listeners or middleware)", iterations, [&]
        {
            bus.dispatch(benchmark_event{1u});
        });
    }

    {
        catalyst::events::bus bus;
        const auto listener = bus.add_listener<benchmark_event>(handle_event);

        catalyst::bench::run("bus.dispatch (one synchronous listener)", iterations, [&]
        {
            bus.dispatch(benchmark_event{1u});
        });
    }

    {
        catalyst::events::bus bus;
        const auto listener = bus.add_listener<benchmark_event>(handle_event);
        std::vector<catalyst::events::token> idle;
        add_idle_listeners(bus, idle, 8, std::make_integer_sequence<int, 256>{});

        catalyst::bench::run("bus.dispatch (one listener + 256 idle types x 8 listeners)", iterations, [&]
        {
            bus.dispatch(benchmark_event{1u});
        });

        catalyst::bench::run("bus.add_listener + token.remove (with idle types present)", iterations, [&]
        {
            auto transient = bus.add_listener<benchmark_event>([](const benchmark_event &) {});
            transient.remove();
        });
    }

    {
        catalyst::events::bus bus;
        std::vector<catalyst::events::token> listeners;
        for (int listener = 0; listener < 64; ++listener)
            listeners.push_back(bus.add_listener<benchmark_event>(handle_event));

        catalyst::bench::run("bus.dispatch (64 synchronous listeners)", iterations / 10u, [&]
        {
            bus.dispatch(benchmark_event{1u});
        });
    }

    {
        catalyst::events::bus bus;
        const auto listener = bus.add_listener<benchmark_event>(handle_event);
        const auto middleware = bus.add_middleware<benchmark_event>([](benchmark_event &event, const auto &next)
        {
            next(event);
        });

        catalyst::bench::run("bus.dispatch (one chain middleware + one listener)", iterations, [&]
        {
            bus.dispatch(benchmark_event{1u});
        });
    }

    {
        catalyst::events::bus bus;
        const auto listener = bus.add_listener<benchmark_event>(handle_event);
        const auto middleware = bus.add_middleware<benchmark_event>([](benchmark_event &) { return false; });

        catalyst::bench::run("bus.dispatch (middleware stops before one listener)", iterations, [&]
        {
            bus.dispatch(benchmark_event{1u});
        });
    }

    {
        catalyst::events::bus bus;
        const auto listener = bus.add_listener<benchmark_event>(handle_event_async);

        catalyst::bench::run("bus.dispatch_async (one asynchronous listener)", iterations / 10u, [&]
        {
            bus.dispatch_async(benchmark_event{1u}).get();
        });
    }

    {
        catalyst::events::bus bus;
        const auto listener = bus.add_listener<benchmark_event>(handle_event_async);
        const auto middleware = bus.add_async_middleware<benchmark_event>([](benchmark_event &event, const auto &next) -> catalyst::events::task<void>
        {
            co_await next(event);
        });

        catalyst::bench::run("bus.dispatch_async (one chain middleware + one listener)", iterations / 10u, [&]
        {
            bus.dispatch_async(benchmark_event{1u}).get();
        });
    }

    {
        catalyst::events::bus bus;
        const auto listener = bus.add_listener<benchmark_event>(handle_event_thread_local);

        catalyst::bench::run("bus.dispatch (1 thread, thread-local listener)", iterations, [&]
        {
            bus.dispatch(benchmark_event{1u});
        });

        for (const unsigned threads : {2u, 4u, 8u})
        {
            run_parallel("bus.dispatch (" + std::to_string(threads) + " threads, one listener)",
                         threads,
                         iterations / threads,
                         [&](std::size_t count)
                         {
                             for (std::size_t iteration = 0u; iteration < count; ++iteration)
                                 bus.dispatch(benchmark_event{1u});
                         });
        }
    }

    {
        catalyst::events::bus bus;
        const auto listener = bus.add_listener<benchmark_event>(handle_event_thread_local);
        std::atomic<bool> churning{true};

        std::thread churn([&]
        {
            while (churning.load(std::memory_order_acquire))
            {
                auto transient = bus.add_listener<benchmark_event>([](const benchmark_event &) {});
                transient.remove();
            }
        });

        run_parallel("bus.dispatch (4 threads + add/remove churn thread)",
                     4u,
                     iterations / 4u,
                     [&](std::size_t count)
                     {
                         for (std::size_t iteration = 0u; iteration < count; ++iteration)
                             bus.dispatch(benchmark_event{1u});
                     });

        churning.store(false, std::memory_order_release);
        churn.join();
    }

    return handled_events.load(std::memory_order_relaxed) == 0u;
}