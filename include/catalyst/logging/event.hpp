/**
 * @file event.hpp
 * @brief One record per call to a log function.
 * @details The router stamps the sequence number; the log functions stamp everything else. The
 * category is a `string_view` because it always points at a category type's static name, so an
 * event can be copied into a ring buffer without owning it.
 *
 * `payload` is for producers whose findings are more than text: a validation pass can attach its
 * diagnostic so a sink built for that pass gets the structured result back out, while a console
 * sink just prints the message. It is empty for ordinary log calls.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/logging/level.hpp>

#include <any>
#include <chrono>
#include <cstdint>
#include <source_location>
#include <string>
#include <string_view>
#include <thread>

namespace catalyst::logging
{

    /**
     * @struct log_event
     * @brief One log record: where it came from, when, and what it says.
     */
    struct log_event
    {
        log_level level = log_level::info;
        std::string_view category{};     ///< The category type's name; points at static storage.
        std::string message{};           ///< The formatted text.
        std::source_location location{}; ///< The call site.
        std::chrono::system_clock::time_point timestamp{};
        std::thread::id thread_id{};
        std::uint64_t sequence = 0; ///< Stamped by the router; strictly increasing per router.
        std::any payload{};         ///< Structured data from the producer, when there is any.
    };

    /**
     * @brief The payload of an event, as a concrete type.
     * @tparam T The type the producer attached.
     * @param event The event to read.
     * @return A pointer to the payload, or nullptr when the event carries none or carries something else.
     */
    template <typename T>
    [[nodiscard]] const T *payload_as(const log_event &event) noexcept
    {
        return std::any_cast<T>(&event.payload);
    }

} // namespace catalyst::logging
