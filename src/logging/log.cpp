/**
 * @file log.cpp
 * @brief Implements the parts of the logging entry points that are not templates.
 * License: MIT (see LICENSE).
 */

#include <catalyst/logging/log.hpp>

#include <chrono>
#include <thread>
#include <utility>

namespace catalyst::logging
{

    void emit(log_event event)
    {
        if (event.timestamp == std::chrono::system_clock::time_point{})
            event.timestamp = std::chrono::system_clock::now();
        if (event.thread_id == std::thread::id{})
            event.thread_id = std::this_thread::get_id();
        default_logger().log(std::move(event));
    }

} // namespace catalyst::logging
