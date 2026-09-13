#pragma once

#include <chrono>
#include <cstddef>
#include <functional>
#include <iomanip>
#include <iostream>
#include <string_view>

namespace catalyst::bench
{
    template <typename Operation>
    void run(std::string_view name, std::size_t iterations, Operation&& operation)
    {
        using clock = std::chrono::steady_clock;

        std::invoke(operation);

        const auto start = clock::now();
        for (std::size_t iteration = 0; iteration < iterations; ++iteration)
            std::invoke(operation);
        const auto elapsed = clock::now() - start;

        const auto elapsed_seconds = std::chrono::duration<double>(elapsed).count();
        const auto nanoseconds_per_operation =
            std::chrono::duration<double, std::nano>(elapsed).count() / static_cast<double>(iterations);
        const auto operations_per_second = static_cast<double>(iterations) / elapsed_seconds;

        std::cout << name << '\n'
                  << "  iterations: " << iterations << '\n'
                  << "  total:      " << std::fixed << std::setprecision(3)
                  << (elapsed_seconds * 1'000.0) << " ms\n"
                  << "  average:    " << nanoseconds_per_operation << " ns/op\n"
                  << "  throughput: " << operations_per_second << " op/s\n";
    }
} // namespace catalyst::bench