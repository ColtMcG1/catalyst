/**
 * @file device.hpp
 * @brief Enumerating audio endpoints, and saying which one a stream should open.
 * @details Enumeration is a free function rather than a member, because a program that has not
 * decided what to open yet has nothing to call a member on. Populating a device picker is
 * `audio::devices()`, and it costs no stream, no thread and no open handle.
 *
 * The interesting type here is @ref device_selector. The old surface had a single
 * `std::string preferred_device` documented as "an id, or a friendly name as a fallback for
 * convenience, but ambiguous" - which is a field that does two things and tells the caller to hope.
 * Two identical headsets have the same friendly name, so "Headphones" is a coin toss, and there is
 * no way to write "the default device, whatever it is" except by leaving the field empty and
 * knowing that means default. A selector says which question is being asked, and @ref
 * device_selector::by_id is the one to build a settings file on.
 * License: MIT (see LICENSE).
 */

#pragma once

#include <catalyst/audio/backend.hpp>
#include <catalyst/audio/error.hpp>
#include <catalyst/audio/types.hpp>

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

namespace catalyst::audio
{

    /**
     * @struct device_info
     * @brief One enumerated endpoint.
     * @details Owning values, so the result of @ref devices stays valid however long the caller
     * keeps it and whatever happens to the hardware afterwards.
     */
    struct device_info
    {
        /**
         * @brief Stable, backend-specific identifier.
         * @details The one to persist, and the one to hand to @ref device_selector::by_id. Opaque:
         * its shape is the platform's business (WASAPI's is an endpoint moniker, ASIO's is a driver
         * name), and nothing but the same backend should try to parse it.
         */
        std::string id;

        /** @brief Display name. Not unique - two identical headsets collide - and not stable across
         * driver updates. For showing a person, not for matching. */
        std::string name;

        /** @brief The backend that enumerated it. Resolved, never @ref backend_kind::automatic. */
        backend_kind backend = backend_kind::automatic;

        /** @brief Most channels it will render. Zero for a capture-only endpoint. */
        channel_count max_output_channels = 0;

        /** @brief Most channels it will capture. Zero for a render-only endpoint. */
        channel_count max_input_channels = 0;

        /** @brief The rate it runs at natively. Opening at this rate avoids a resampler. */
        sample_rate_t default_sample_rate = 0;

        /** @brief True if this is the system default endpoint for its direction. */
        bool is_default = false;

        /** @brief True if the endpoint can render at all. */
        [[nodiscard]] constexpr bool supports_output() const noexcept { return max_output_channels > 0; }

        /** @brief True if the endpoint can capture at all. */
        [[nodiscard]] constexpr bool supports_input() const noexcept { return max_input_channels > 0; }

        /**
         * @brief Whether this endpoint could serve @p direction at all.
         * @param direction The direction a stream wants.
         */
        [[nodiscard]] constexpr bool supports(stream_direction direction) const noexcept
        {
            return (!has_output(direction) || supports_output()) && (!has_input(direction) || supports_input());
        }
    };

    /**
     * @struct device_selector
     * @brief Which endpoint a stream should open, and by what question.
     * @details Build one with the named constructors; the default-constructed selector asks for the
     * system default, which is what most programs want and what an empty configuration should
     * therefore mean.
     *
     *     cfg.device = audio::device_selector::by_id(settings.audio_device_id);
     *     cfg.device = audio::device_selector::by_name("Focusrite");   // a picker's search box
     *     cfg.device = {};                                             // whatever the OS says
     */
    struct device_selector
    {
        /**
         * @enum match
         * @brief Which question the selector asks.
         */
        enum class match : std::uint8_t
        {
            /** @brief The system default endpoint for the stream's direction. */
            system_default = 0,
            /** @brief The endpoint whose @ref device_info::id equals @ref value, exactly. */
            id,
            /** @brief The first endpoint whose @ref device_info::name contains @ref value, matched
             * case-sensitively. Convenient and ambiguous; see the file comment. */
            name,
        };

        /** @brief The question. */
        match by = match::system_default;

        /** @brief The id or name to match. Ignored for @ref match::system_default. */
        std::string value;

        /** @brief Asks for the system default. Same as a default-constructed selector. */
        [[nodiscard]] static device_selector system_default() { return {}; }

        /** @brief Asks for an exact @ref device_info::id. The one to build a settings file on. */
        [[nodiscard]] static device_selector by_id(std::string id)
        {
            return device_selector{match::id, std::move(id)};
        }

        /** @brief Asks for the first device whose name contains @p text. */
        [[nodiscard]] static device_selector by_name(std::string text)
        {
            return device_selector{match::name, std::move(text)};
        }

        /**
         * @brief Whether @p device answers this selector's question.
         * @param device The candidate.
         * @return For @ref match::system_default, whether the device is the default. For the
         * others, whether the id matches exactly or the name contains @ref value.
         */
        [[nodiscard]] bool matches(const device_info &device) const noexcept
        {
            switch (by)
            {
            case match::system_default:
                return device.is_default;
            case match::id:
                return device.id == value;
            case match::name:
                return device.name.find(value) != std::string::npos;
            }
            return false;
        }

        [[nodiscard]] friend bool operator==(const device_selector &, const device_selector &) noexcept = default;
    };

    /**
     * @brief Every endpoint visible to the default backend.
     * @return Owning values, so the result outlives whatever produced it.
     */
    [[nodiscard]] std::expected<std::vector<device_info>, error> devices();

    /**
     * @brief Every endpoint visible to @p backend.
     * @param backend The backend to enumerate. @ref backend_kind::automatic means @ref
     * default_backend.
     * @return @ref error_code::backend_unavailable if the backend is not in this build.
     */
    [[nodiscard]] std::expected<std::vector<device_info>, error> devices(backend_kind backend);

    /**
     * @brief The endpoint a stream would open by default.
     * @param direction Which half of the device matters; a default render endpoint and a default
     * capture endpoint are different devices on most systems.
     * @param backend The backend to ask. @ref backend_kind::automatic means @ref default_backend.
     * @return @ref error_code::no_device if nothing is plugged in.
     */
    [[nodiscard]] std::expected<device_info, error> default_device(
        stream_direction direction = stream_direction::output,
        backend_kind backend = backend_kind::automatic);

    /**
     * @brief The first device in @p list that answers @p selector and can serve @p direction.
     * @details Exposed because a device picker wants to resolve a saved selector against a list it
     * already has, without opening anything. Streams use the same function internally, so a picker
     * that shows a green tick here will not surprise the user at open time.
     * @return A pointer into @p list, or null if nothing matched.
     */
    [[nodiscard]] const device_info *find_device(
        const std::vector<device_info> &list,
        const device_selector &selector,
        stream_direction direction = stream_direction::output) noexcept;

} // namespace catalyst::audio
